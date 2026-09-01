#include "hyperliquid.h"
#include "wallet_menu.h"
#include "ranking.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <poll.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <curl/websockets.h>

#include "json.hpp"
#include "utils.h"
#include "ru.h"
#include "message_queue.h"
#include "premium.h"
#include "alert_settings.h"

using json = nlohmann::json;

extern std::atomic<bool> running;
extern const std::string SERVICE_CHAT_ID;
extern sqlite3* db;
extern std::mutex dbMutex;
std::string http(const std::string& url, const std::string& post, int timeout);
void logCritical(const std::string& msg);
std::string getUserLanguage(const std::string& chatId);
void rememberView(const std::string& chatId, const std::string& data);
std::string shortAddress(const std::string& a);

#include "hyperliquid_internal.h"

namespace hl {

const char* const HL_WS_URL   = "wss://api.hyperliquid.xyz/ws";
const char* const HL_INFO_URL = "https://api.hyperliquid.xyz/info";

constexpr long long HL_PING_INTERVAL_SEC = 50;

constexpr int HL_SUBSCRIBE_PACE_MS = 50;

constexpr long long HL_COINS_REFRESH_SEC = 3600;
constexpr long long HL_WALLETS_REFRESH_SEC = 60;

constexpr long long HL_SILENCE_TIMEOUT_SEC = 120;
constexpr int HL_RECONNECT_MIN_SEC = 1;
constexpr int HL_RECONNECT_MAX_SEC = 60;

constexpr int HL_BUDGET_PER_MINUTE = 1150;
constexpr int HL_WEIGHT_USER_FILLS = 20;
constexpr int HL_WEIGHT_META = 20;
constexpr int HL_WEIGHT_FUNDING_HIST = 80;

constexpr long long HL_USER_DEBOUNCE_SEC = 5;
constexpr long long HL_SERVICE_DEBOUNCE_SEC = 30;

constexpr int HL_HYPERACTIVE_FILLS = 200;
constexpr long long HL_HYPERACTIVE_DEBOUNCE_SEC = 600;

constexpr long long HL_CLEANUP_INTERVAL_SEC = 3600;
constexpr long long HL_RANK_REBUILD_SEC = 300;
constexpr size_t HL_POSITION_CACHE_CAP = 20000;

constexpr long long HL_FILL_TTL_SEC = 365LL * 86400LL;

constexpr int HL_MAX_BOT_TRADES = 200;

const char* const HL_CARD_SEPARATOR = "\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501";

sqlite3* g_hlDb = nullptr;
std::mutex g_hlDbMutex;

std::string hlDbFile() {
    const char* p = std::getenv("WHALE_HL_DB_FILE");
    return (p && *p) ? std::string(p) : std::string("hyperliquid.db");
}

long long nowSec() { return static_cast<long long>(std::time(nullptr)); }
long long nowMs()  { return nowSec() * 1000LL; }

bool parseDecimalToNanos(const std::string& s, long long& out) {
    out = 0;
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[i] == '-') { neg = true; i++; }
    else if (s[i] == '+') i++;

    long long intPart = 0;
    size_t intDigits = 0;
    constexpr long long MAX_WHOLE_UNITS = 9223372036LL;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
        intPart = intPart * 10 + (s[i] - '0');
        if (intPart > MAX_WHOLE_UNITS) return false;
        intDigits++;
    }
    if (intDigits == 0) return false;

    long long frac = 0;
    int fracDigits = 0;
    if (i < s.size() && s[i] == '.') {
        i++;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
            if (fracDigits < 9) { frac = frac * 10 + (s[i] - '0'); fracDigits++; }
        }
    }
    if (i != s.size()) return false;
    while (fracDigits < 9) { frac *= 10; fracDigits++; }

    out = intPart * NANOS_PER_UNIT + frac;
    if (neg) out = -out;
    return true;
}

bool notionalNanos(const std::string& pxStr, const std::string& szStr, long long& out) {
    long long px = 0, sz = 0;
    if (!parseDecimalToNanos(pxStr, px) || !parseDecimalToNanos(szStr, sz)) return false;
    if (px < 0) px = -px;
    if (sz < 0) sz = -sz;
    __int128 product = static_cast<__int128>(px) * static_cast<__int128>(sz);
    __int128 result = product / NANOS_PER_UNIT;
    if (result > static_cast<__int128>(9000000000000000000LL)) return false;
    out = static_cast<long long>(result);
    return true;
}

std::string fmtUsd(long long nanos) { return formatUsdNanosSigned(nanos, false); }

std::string formatQtyNanos(long long nanos) {
    if (nanos < 0) nanos = -nanos;
    const long long whole = nanos / NANOS_PER_UNIT;
    long long frac = nanos % NANOS_PER_UNIT;
    std::string out = formatThousands(static_cast<uint64_t>(whole));
    if (frac == 0) return out;
    std::string f = std::to_string(frac);
    f.insert(0, 9 - f.size(), '0');
    while (!f.empty() && f.back() == '0') f.pop_back();
    return out + "." + f;
}

const char* dirMark(Lang lang) {
    return lang == Lang::AR ? "\u200F" : "";
}

std::string formatPriceNanos(long long nanos) {
    const bool neg = nanos < 0;
    if (neg) nanos = -nanos;

    int digits;
    if (nanos >= 1000LL * NANOS_PER_UNIT)      digits = 2;
    else if (nanos >= NANOS_PER_UNIT)          digits = 4;
    else if (nanos >= NANOS_PER_UNIT / 100)    digits = 6;
    else                                       digits = 9;

    long long scale = 1;
    for (int i = 0; i < 9 - digits; i++) scale *= 10;
    long long units = nanos / NANOS_PER_UNIT;
    long long frac = (nanos % NANOS_PER_UNIT + scale / 2) / scale;

    long long fracLimit = 1;
    for (int i = 0; i < digits; i++) fracLimit *= 10;
    if (frac >= fracLimit) { units++; frac = 0; }

    std::string fracStr = std::to_string(frac);
    fracStr.insert(0, static_cast<size_t>(digits) - fracStr.size(), '0');
    while (fracStr.size() > 2 && fracStr.back() == '0') fracStr.pop_back();

    return std::string(neg ? "-$" : "$") +
           formatThousands(static_cast<uint64_t>(units)) + "." + fracStr;
}

std::mutex g_coinsMutex;
std::set<std::string> g_perpCoins;
std::vector<std::string> g_perpDexes{""};

bool perpCoinsLoaded() {
    std::lock_guard<std::mutex> l(g_coinsMutex);
    return !g_perpCoins.empty();
}

bool isPerpCoin(const std::string& coin) {
    if (coin.find(':') != std::string::npos) return true; // HIP-3: xyz:TSLA
    std::lock_guard<std::mutex> l(g_coinsMutex);
    if (g_perpCoins.empty()) return true;
    return g_perpCoins.count(coin) > 0;
}

void setPerpCoins(const std::vector<std::string>& coins) {
    std::set<std::string> fresh(coins.begin(), coins.end());
    std::lock_guard<std::mutex> l(g_coinsMutex);
    g_perpCoins.swap(fresh);
}

std::vector<std::string> perpDexNames() {
    std::lock_guard<std::mutex> l(g_coinsMutex);
    if (g_perpDexes.empty()) return {""};
    return g_perpDexes;
}

using AddressSet = std::set<std::string>;
std::mutex g_walletsMutex;
std::shared_ptr<const AddressSet> g_watched = std::make_shared<const AddressSet>();
std::shared_ptr<const AddressSet> g_banned  = std::make_shared<const AddressSet>();

std::shared_ptr<const AddressSet> watchedSnapshot() {
    std::lock_guard<std::mutex> l(g_walletsMutex);
    return g_watched;
}
std::shared_ptr<const AddressSet> bannedSnapshot() {
    std::lock_guard<std::mutex> l(g_walletsMutex);
    return g_banned;
}

bool isBanned(const std::string& addr) {
    return bannedSnapshot()->count(addr) > 0;
}

std::set<std::string> loadBannedWallets() {
    std::set<std::string> out;
    {
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (g_hlDb) {
            sqlite3_stmt* s = nullptr;
            if (prepareOrLog(g_hlDb, &s, "SELECT wallet FROM hl_banned")) {
                std::set<std::string> part;
                int rc = SQLITE_DONE;
                while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
                    std::string w = safeColumnText(s, 0);
                    if (!w.empty()) part.insert(toLower(w));
                }
                sqlite3_finalize(s);
                if (rc == SQLITE_DONE) out.swap(part);
            }
        }
    }
    {
        std::lock_guard<std::mutex> l(dbMutex);
        if (db) {
            sqlite3_stmt* s = nullptr;
            if (prepareOrLog(db, &s,
                    "SELECT wallet FROM ignored_wallets WHERE permanent=1")) {
                std::set<std::string> part;
                int rc = SQLITE_DONE;
                while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
                    std::string w = safeColumnText(s, 0);
                    if (!w.empty()) part.insert(toLower(w));
                }
                sqlite3_finalize(s);
                if (rc == SQLITE_DONE)
                    out.insert(part.begin(), part.end());
            }
        }
    }
    return out;
}

void reloadWatchedWallets() {
    std::vector<std::string> src = hlWatchedAddresses();
    std::set<std::string> fresh(src.begin(), src.end());
    std::set<std::string> banned = loadBannedWallets();
    auto freshPtr  = std::make_shared<const AddressSet>(std::move(fresh));
    auto bannedPtr = std::make_shared<const AddressSet>(std::move(banned));
    std::lock_guard<std::mutex> l(g_walletsMutex);
    g_watched = freshPtr;
    g_banned = bannedPtr;
}

size_t watchedCount() { return watchedSnapshot()->size(); }

std::atomic<unsigned long long> g_tradesSeen{0};
std::atomic<unsigned long long> g_hits{0};
std::atomic<unsigned long long> g_enriched{0};
std::atomic<unsigned long long> g_alertsSent{0};
std::atomic<unsigned long long> g_budgetSkips{0};
std::atomic<unsigned long long> g_reconnects{0};
std::atomic<unsigned long long> g_botsBanned{0};
std::atomic<int> g_subscribedCoins{0};
std::atomic<long long> g_lastMsgTs{0};
std::atomic<bool> g_connected{false};

std::mutex g_budgetMutex;
long long g_budgetWindow = 0;
int g_budgetSpent = 0;

bool spendBudget(int weight, bool = false) {
    std::lock_guard<std::mutex> l(g_budgetMutex);
    long long minute = nowSec() / 60;
    if (minute != g_budgetWindow) { g_budgetWindow = minute; g_budgetSpent = 0; }
    if (g_budgetSpent + weight > HL_BUDGET_PER_MINUTE) return false;
    g_budgetSpent += weight;
    return true;
}

int budgetLeft() {
    std::lock_guard<std::mutex> l(g_budgetMutex);
    long long minute = nowSec() / 60;
    if (minute != g_budgetWindow) { g_budgetWindow = minute; g_budgetSpent = 0; }
    return HL_BUDGET_PER_MINUTE - g_budgetSpent;
}

json infoPost(const json& body, int weight, bool slow) {
    if (!spendBudget(weight, slow)) {
        g_budgetSkips.fetch_add(1, std::memory_order_relaxed);
        return json();
    }
    std::string resp = http(HL_INFO_URL, body.dump(), 15);
    if (resp.empty()) return json();
    json j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) return json();
    return j;
}

json infoPost(const json& body, int weight) {
    return infoPost(body, weight, false);
}

std::mutex g_queueMutex;
std::set<std::string> g_enrichQueue;
std::set<std::string> g_urgentQueue;
std::unordered_map<std::string, long long> g_nextEnrichAt;

void queueWallet(const std::string& addr) {
    std::lock_guard<std::mutex> l(g_queueMutex);
    if (g_urgentQueue.count(addr)) return;
    g_enrichQueue.insert(addr);
}

void queueUrgent(const std::string& addr) {
    std::lock_guard<std::mutex> l(g_queueMutex);
    g_enrichQueue.erase(addr);
    g_urgentQueue.insert(addr);
}

bool readyToEnrich(const std::string& addr) {
    std::lock_guard<std::mutex> l(g_queueMutex);
    auto it = g_nextEnrichAt.find(addr);
    return it == g_nextEnrichAt.end() || nowSec() >= it->second;
}

void setNextEnrich(const std::string& addr, long long at) {
    std::lock_guard<std::mutex> l(g_queueMutex);
    g_nextEnrichAt[addr] = at;
}

std::atomic<bool> g_hlRunning{false};
std::thread g_feedThread;
std::thread g_enrichThread;

bool keepGoing() {
    return g_hlRunning.load(std::memory_order_relaxed) &&
           running.load(std::memory_order_relaxed);
}

void interruptibleSleep(int seconds) {
    for (int i = 0; i < seconds * 10 && keepGoing(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

long long hourFloorSec(long long sec) {
    if (sec < 0) return 0;
    return (sec / 3600LL) * 3600LL;
}

struct CtxSnap {
    std::string coin;
    long long rate = 0;
    long long mark = 0;
    long long oi = 0;
    long long dayVlm = 0;
};

void saveFundingRates(long long hourTs, const std::vector<CtxSnap>& rows) {
    if (rows.empty() || hourTs <= 0) return;
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return;
    if (sqlite3_exec(g_hlDb, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "INSERT INTO hl_funding_rate(coin,hour_ts,rate_nanos,mark_nanos,oi_nanos,day_vlm_nanos) "
            "VALUES(?,?,?,?,?,?) "
            "ON CONFLICT(coin,hour_ts) DO UPDATE SET "
            "rate_nanos=excluded.rate_nanos,"
            "mark_nanos=CASE WHEN excluded.mark_nanos>0 THEN excluded.mark_nanos ELSE hl_funding_rate.mark_nanos END,"
            "oi_nanos=CASE WHEN excluded.oi_nanos>0 THEN excluded.oi_nanos ELSE hl_funding_rate.oi_nanos END,"
            "day_vlm_nanos=CASE WHEN excluded.day_vlm_nanos>0 THEN excluded.day_vlm_nanos ELSE hl_funding_rate.day_vlm_nanos END")) {
        sqlite3_exec(g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
        return;
    }
    for (const auto& row : rows) {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        sqlite3_bind_text(s, 1, row.coin.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, hourTs);
        sqlite3_bind_int64(s, 3, row.rate);
        sqlite3_bind_int64(s, 4, row.mark);
        sqlite3_bind_int64(s, 5, row.oi);
        sqlite3_bind_int64(s, 6, row.dayVlm);
        if (sqlite3_step(s) != SQLITE_DONE) {
            sqlite3_finalize(s);
            sqlite3_exec(g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
            return;
        }
    }
    sqlite3_finalize(s);
    if (sqlite3_exec(g_hlDb, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[HL] saveFundingRates COMMIT: " << sqlite3_errmsg(g_hlDb) << std::endl;
        sqlite3_exec(g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

void ingestUniverse(const json& universe, const json* ctxs, const std::string& dex,
                    std::vector<std::string>& subscribe, std::vector<std::string>& all,
                    std::vector<CtxSnap>& rates) {
    if (!universe.is_array()) return;
    for (size_t i = 0; i < universe.size(); i++) {
        const auto& u = universe[i];
        if (!u.is_object() || !u.contains("name") || !u["name"].is_string()) continue;
        std::string name = u["name"].get<std::string>();
        if (name.empty()) continue;
        std::string coin = name;
        if (!dex.empty() && name.find(':') == std::string::npos)
            coin = dex + ":" + name;
        all.push_back(coin);
        if (!u.value("isDelisted", false)) subscribe.push_back(coin);
        if (!ctxs || i >= ctxs->size() || !(*ctxs)[i].is_object()) continue;
        const json& ctx = (*ctxs)[i];
        CtxSnap snap;
        snap.coin = coin;
        parseDecimalToNanos(jstr(ctx, "funding", "0"), snap.rate);
        parseDecimalToNanos(jstr(ctx, "markPx", "0"), snap.mark);
        parseDecimalToNanos(jstr(ctx, "dayNtlVlm", "0"), snap.dayVlm);
        if (snap.dayVlm < 0) snap.dayVlm = 0;
        long long oiCoins = 0;
        parseDecimalToNanos(jstr(ctx, "openInterest", "0"), oiCoins);
        if (oiCoins > 0 && snap.mark > 0) {
            const __int128 x = static_cast<__int128>(oiCoins) * snap.mark / NANOS_PER_UNIT;
            if (x > 0 && x <= 9000000000000000000LL) snap.oi = static_cast<long long>(x);
        }
        rates.push_back(std::move(snap));
    }
}

bool fetchDexUniverse(const std::string& dex, std::vector<std::string>& subscribe,
                      std::vector<std::string>& all) {
    json body;
    body["type"] = "metaAndAssetCtxs";
    if (!dex.empty()) body["dex"] = dex;
    json j = infoPost(body, HL_WEIGHT_META);
    std::vector<CtxSnap> rates;
    if (j.is_array() && j.size() >= 2 && j[0].is_object() &&
        j[0].contains("universe") && j[0]["universe"].is_array() && j[1].is_array()) {
        ingestUniverse(j[0]["universe"], &j[1], dex, subscribe, all, rates);
        saveFundingRates(hourFloorSec(nowSec()), rates);
        return true;
    }

    json meta;
    meta["type"] = "meta";
    if (!dex.empty()) meta["dex"] = dex;
    json m = infoPost(meta, HL_WEIGHT_META);
    if (!m.is_object() || !m.contains("universe") || !m["universe"].is_array())
        return false;
    ingestUniverse(m["universe"], nullptr, dex, subscribe, all, rates);
    return true;
}

bool fetchPerpCoins(std::vector<std::string>& subscribe, std::vector<std::string>& all) {
    subscribe.clear();
    all.clear();
    if (!fetchDexUniverse("", subscribe, all) || all.empty()) {
        std::cerr << "[HL] meta: неожиданный ответ" << std::endl;
        return false;
    }
    json dexs = infoPost(json{{"type", "perpDexs"}}, HL_WEIGHT_META);
    if (!dexs.is_array()) return true;
    int extra = 0;
    for (const auto& d : dexs) {
        std::string name;
        if (d.is_null()) continue;
        if (d.is_string()) name = d.get<std::string>();
        else if (d.is_object()) name = d.value("name", "");
        if (name.empty()) continue;
        const size_t before = all.size();
        if (!fetchDexUniverse(name, subscribe, all)) continue;
        extra += static_cast<int>(all.size() - before);
    }
    if (extra > 0)
        std::cout << "[HL] HIP-3 рынков: " << extra << ", всего: " << all.size() << std::endl;
    {
        std::vector<std::string> dexes{""};
        for (const auto& d : dexs) {
            std::string name;
            if (d.is_null()) continue;
            if (d.is_string()) name = d.get<std::string>();
            else if (d.is_object()) name = d.value("name", "");
            if (!name.empty()) dexes.push_back(name);
        }
        std::lock_guard<std::mutex> l(g_coinsMutex);
        g_perpDexes.swap(dexes);
    }
    return !all.empty();
}

struct PositionInfo {
    bool known = false;
    long long snapshotAt = 0;
    bool stillOpen = false;
    int leverage = 0;
    bool isolated = false;
    long long marginUsedNanos = 0;
    long long positionValueNanos = 0;
    long long liquidationPxNanos = 0;
};

std::mutex g_posMutex;
std::unordered_map<std::string, PositionInfo> g_lastPositions;
std::unordered_map<std::string, long long> g_accountValue;

std::string jstr(const json& j, const char* key, const char* def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

std::string posKey(const std::string& wallet, const std::string& coin) {
    return wallet + "|" + coin;
}

void fetchAccountState(const std::string& wallet) {
    std::set<std::string> openNow;
    std::vector<std::pair<std::string, json>> posRows;
    long long accountValue = 0;

    for (const std::string& dex : perpDexNames()) {
        json body;
        body["type"] = "clearinghouseState";
        body["user"] = wallet;
        if (!dex.empty()) body["dex"] = dex;
        json j = infoPost(body, HL_WEIGHT_CLEARINGHOUSE);
        if (!j.is_object()) continue;
        if (j.contains("marginSummary") && j["marginSummary"].is_object()) {
            long long v = 0;
            parseDecimalToNanos(jstr(j["marginSummary"], "accountValue", "0"), v);
            if (v > 0) accountValue += v;
        }
        if (!j.contains("assetPositions") || !j["assetPositions"].is_array()) continue;
        for (const auto& ap : j["assetPositions"]) {
            if (!ap.is_object() || !ap.contains("position") || !ap["position"].is_object()) continue;
            json p = ap["position"];
            std::string coin = jstr(p, "coin");
            if (coin.empty()) continue;
            if (!dex.empty() && coin.find(':') == std::string::npos)
                coin = dex + ":" + coin;
            openNow.insert(coin);
            posRows.emplace_back(std::move(coin), std::move(p));
        }
    }

    std::lock_guard<std::mutex> l(g_posMutex);
    if (accountValue > 0) g_accountValue[wallet] = accountValue;

    const std::string prefix = posKey(wallet, "");
    for (auto it = g_lastPositions.begin(); it != g_lastPositions.end(); ) {
        if (it->first.rfind(prefix, 0) == 0 &&
            openNow.find(it->first.substr(prefix.size())) == openNow.end())
            it = g_lastPositions.erase(it);
        else
            ++it;
    }

    if (g_lastPositions.size() > HL_POSITION_CACHE_CAP) {
        g_lastPositions.clear();
        std::cout << "[HL] кэш позиций сброшен по достижении предела" << std::endl;
    }

    for (auto& row : posRows) {
        const json& p = row.second;
        PositionInfo info;
        info.known = true;
        info.snapshotAt = nowSec();
        if (p.contains("leverage") && p["leverage"].is_object()) {
            const json& lev = p["leverage"];
            if (lev.contains("value") && lev["value"].is_number())
                info.leverage = lev["value"].get<int>();
            info.isolated = jstr(lev, "type") == "isolated";
        }
        parseDecimalToNanos(jstr(p, "marginUsed", "0"), info.marginUsedNanos);
        parseDecimalToNanos(jstr(p, "positionValue", "0"), info.positionValueNanos);
        parseDecimalToNanos(jstr(p, "liquidationPx", "0"), info.liquidationPxNanos);
        g_lastPositions[posKey(wallet, row.first)] = info;
    }
}

long long lastKnownAccountValue(const std::string& wallet) {
    std::lock_guard<std::mutex> l(g_posMutex);
    auto it = g_accountValue.find(wallet);
    return it == g_accountValue.end() ? 0 : it->second;
}

PositionInfo lastKnownPosition(const std::string& wallet, const std::string& coin) {
    std::lock_guard<std::mutex> l(g_posMutex);
    auto it = g_lastPositions.find(posKey(wallet, coin));
    return it == g_lastPositions.end() ? PositionInfo{} : it->second;
}

struct WalletState {
    bool seeded = false;
    bool backfilled30d = false;
    long long lastFillTs = 0;
    long long lastEnriched = 0;
    long long debounce = HL_USER_DEBOUNCE_SEC;
};

bool loadWalletState(const std::string& wallet, WalletState& out) {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return false;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT seeded, last_fill_ts, last_enriched, debounce_sec, backfilled_30d "
            "FROM hl_wallet_state WHERE wallet=?"))
        return false;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        out.seeded = sqlite3_column_int(s, 0) != 0;
        out.lastFillTs = sqlite3_column_int64(s, 1);
        out.lastEnriched = sqlite3_column_int64(s, 2);
        out.debounce = sqlite3_column_int64(s, 3);
        if (out.debounce != HL_HYPERACTIVE_DEBOUNCE_SEC &&
            out.debounce != HL_USER_DEBOUNCE_SEC &&
            out.debounce != HL_SERVICE_DEBOUNCE_SEC)
            out.debounce = HL_SERVICE_DEBOUNCE_SEC;
        out.backfilled30d = sqlite3_column_int(s, 4) != 0;
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

void saveWalletState(const std::string& wallet, const WalletState& st) {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "INSERT INTO hl_wallet_state(wallet,seeded,last_fill_ts,last_enriched,debounce_sec,backfilled_30d) "
            "VALUES(?,?,?,?,?,?) ON CONFLICT(wallet) DO UPDATE SET "
            "seeded=excluded.seeded, last_fill_ts=excluded.last_fill_ts, "
            "last_enriched=excluded.last_enriched, debounce_sec=excluded.debounce_sec, "
            "backfilled_30d=excluded.backfilled_30d"))
        return;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, st.seeded ? 1 : 0);
    sqlite3_bind_int64(s, 3, st.lastFillTs);
    sqlite3_bind_int64(s, 4, st.lastEnriched);
    sqlite3_bind_int64(s, 5, st.debounce);
    sqlite3_bind_int(s, 6, st.backfilled30d ? 1 : 0);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void rollbackIfOpen() {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (g_hlDb && sqlite3_get_autocommit(g_hlDb) == 0)
        sqlite3_exec(g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
}

void purgeNonPerpFills() {
    std::vector<std::string> distinctCoins;
    {
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (!g_hlDb) return;
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(g_hlDb, &s, "SELECT DISTINCT coin FROM hl_fills")) return;
        while (sqlite3_step(s) == SQLITE_ROW) distinctCoins.push_back(safeColumnText(s, 0));
        sqlite3_finalize(s);
    }

    std::vector<std::string> alien;
    for (const std::string& c : distinctCoins)
        if (!c.empty() && !isPerpCoin(c)) alien.push_back(c);
    if (alien.empty()) return;

    if (alien.size() == distinctCoins.size()) {
        std::cerr << "[HL] уборка ОТМЕНЕНА: НИ ОДИН рынок из " << distinctCoins.size()
                  << " не опознан как фьючерсный - сломалось сопоставление имён. "
                  << "Примеры: ";
        for (size_t i = 0; i < alien.size() && i < 5; i++)
            std::cerr << alien[i] << (i + 1 < std::min<size_t>(alien.size(), 5) ? ", " : "");
        std::cerr << std::endl;
        return;
    }

    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return;
    int purged = 0;
    for (const std::string& c : alien) {
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(g_hlDb, &s, "DELETE FROM hl_fills WHERE coin=?")) continue;
        sqlite3_bind_text(s, 1, c.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_DONE) purged += sqlite3_changes(g_hlDb);
        sqlite3_finalize(s);
    }
    if (purged > 0)
        std::cout << "[HL] удалено нефьючерсных строк: " << purged
                  << " по " << alien.size() << " рынкам" << std::endl;
}

void cleanupOldFills() {
    const long long cutoffMs = (nowSec() - HL_FILL_TTL_SEC) * 1000LL;
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return;
    sqlite3_stmt* s = nullptr;
    if (prepareOrLog(g_hlDb, &s, "DELETE FROM hl_fills WHERE ts < ?")) {
        sqlite3_bind_int64(s, 1, cutoffMs);
        const int deleted = (sqlite3_step(s) == SQLITE_DONE) ? sqlite3_changes(g_hlDb) : 0;
        sqlite3_finalize(s);
        if (deleted > 0) std::cout << "[HL] удалено сделок старше срока: " << deleted << std::endl;
    }
    if (prepareOrLog(g_hlDb, &s, "DELETE FROM hl_wallet_state WHERE last_enriched < ?")) {
        sqlite3_bind_int64(s, 1, nowSec() - HL_FILL_TTL_SEC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

void forgetUnwatchedAccounts() {
    auto watched = watchedSnapshot();
    if (!watched || watched->empty()) return;
    std::lock_guard<std::mutex> l(g_posMutex);
    size_t before = g_accountValue.size();
    for (auto it = g_accountValue.begin(); it != g_accountValue.end(); ) {
        if (watched->count(it->first)) ++it;
        else it = g_accountValue.erase(it);
    }
    const size_t removed = before - g_accountValue.size();
    if (removed > 0)
        std::cout << "[HL] забыто депозитов неотслеживаемых кошельков: " << removed << std::endl;
}

int countFillsInWindow(const std::string& wallet) {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return 0;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT COUNT(*) FROM hl_fills"
            " WHERE wallet=? AND ts >= ? AND flat=1")) return 0;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (nowSec() - HL_RANK_WINDOW_SEC) * 1000LL);
    int n = 0;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

json fetchFillsPage(const std::string& wallet, long long startMs) {
    json body;
    body["type"] = "userFillsByTime";
    body["user"] = wallet;
    body["startTime"] = startMs;
    body["aggregateByTime"] = true;
    return infoPost(body, HL_WEIGHT_USER_FILLS);
}

bool fillStartEnd(const json& f, long long& start, long long& end) {
    start = 0;
    long long sz = 0;
    parseDecimalToNanos(jstr(f, "startPosition", "0"), start);
    parseDecimalToNanos(jstr(f, "sz", "0"), sz);
    if (sz < 0) sz = -sz;
    const std::string side = jstr(f, "side");
    const long long signedSz = (side == "B") ? sz : -sz;
    end = start + signedSz;
    return true;
}

bool fillIsRoundTrip(const json& f, int code) {
    if (code >= DIR_FLIP) return true;
    if (code != DIR_CLOSE_LONG && code != DIR_CLOSE_SHORT) return false;
    long long start = 0, end = 0;
    fillStartEnd(f, start, end);
    return std::llabs(end) <= NANOS_PER_UNIT / 1000;
}

bool banWallet(const std::string& wallet, int trades) {
    const std::string addr = toLower(wallet);
    bool saved = false;
    {
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (!g_hlDb) return false;
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(g_hlDb, &s,
                "INSERT OR IGNORE INTO hl_banned(wallet,banned_at,trades) VALUES(?,?,?)"))
            return false;
        sqlite3_bind_text(s, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, nowSec());
        sqlite3_bind_int(s, 3, trades);
        if (sqlite3_step(s) == SQLITE_DONE) saved = sqlite3_changes(g_hlDb) > 0;
        sqlite3_finalize(s);
    }
    if (!saved) return false;

    {
        std::lock_guard<std::mutex> l(g_walletsMutex);
        auto updated = std::make_shared<AddressSet>(*g_banned);
        updated->insert(addr);
        g_banned = updated;
    }
    invalidateRankCache();

    if (!isPermanentlyBanned(addr)) {
        std::lock_guard<std::mutex> l(dbMutex);
        if (db) {
            sqlite3_stmt* s = nullptr;
            if (prepareOrLog(db, &s,
                    "INSERT OR REPLACE INTO ignored_wallets(wallet, ignored_until, permanent) "
                    "VALUES(?, ?, 1)")) {
                sqlite3_bind_text(s, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(s, 2, static_cast<sqlite3_int64>(nowSec()));
                if (sqlite3_step(s) != SQLITE_DONE)
                    std::cerr << "[HL] mirror spot ban failed: " << sqlite3_errmsg(db) << std::endl;
                sqlite3_finalize(s);
            }
        }
    }
    untrackWalletFromService(addr);
    std::cout << "[HL] bot banned on perps+spot: " << addr << " (" << trades << " fills/30d)" << std::endl;
    return true;
}

bool saveFillLocked(const std::string& wallet, const json& f, long long closedPnlNanos,
                    long long feeNanos, long long notionalNanosVal, const PositionInfo& pos,
                    long long accountValueNanos) {
    long long tid = f.value("tid", 0LL);
    if (tid == 0) return false;
    if (!g_hlDb) return false;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "INSERT OR IGNORE INTO hl_fills"
            "(tid,wallet,coin,dir,side,px,sz,closed_pnl_nanos,fee_nanos,"
            "notional_nanos,margin_nanos,leverage,oid,account_value_nanos,ts,hash,dir_code,flat,start_pos_nanos) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
        return false;
    sqlite3_bind_int64(s, 1, tid);
    sqlite3_bind_text(s, 2, wallet.c_str(), -1, SQLITE_TRANSIENT);
    std::string coin = jstr(f, "coin");
    std::string dir  = jstr(f, "dir");
    std::string side = jstr(f, "side");
    std::string px   = jstr(f, "px");
    std::string sz   = jstr(f, "sz");
    std::string hash = jstr(f, "hash");
    const int code = dirCode(dir);
    sqlite3_bind_text(s, 3, coin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, dir.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, side.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 6, px.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 7, sz.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 8, closedPnlNanos);
    sqlite3_bind_int64(s, 9, feeNanos);
    sqlite3_bind_int64(s, 10, notionalNanosVal);
    long long margin = 0;
    int lev = 0;
    if (pos.known) {
        margin = pos.marginUsedNanos;
        lev = pos.leverage;
    }
    if (margin <= 0 && lev > 0 && notionalNanosVal > 0)
        margin = notionalNanosVal / lev;
    sqlite3_bind_int64(s, 11, margin);
    sqlite3_bind_int(s,   12, lev);
    sqlite3_bind_int64(s, 13, f.value("oid", 0LL));
    sqlite3_bind_int64(s, 14, accountValueNanos);
    sqlite3_bind_int64(s, 15, f.value("time", 0LL));
    sqlite3_bind_text(s,  16, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 17, code);
    sqlite3_bind_int(s, 18, fillIsRoundTrip(f, code) ? 1 : 0);
    long long startPos = 0;
    parseDecimalToNanos(jstr(f, "startPosition", "0"), startPos);
    sqlite3_bind_int64(s, 19, startPos);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    int changed = sqlite3_changes(g_hlDb);
    sqlite3_finalize(s);
    return ok && changed > 0;
}

std::string dirKey(const json& f) {
    const int code = dirCode(jstr(f, "dir"));
    long long start = 0, end = 0;
    fillStartEnd(f, start, end);
    const long long eps = NANOS_PER_UNIT / 1000;
    switch (code) {
        case DIR_OPEN_LONG:
            return std::llabs(start) > eps ? "hl_add_long" : "hl_open_long";
        case DIR_OPEN_SHORT:
            return std::llabs(start) > eps ? "hl_add_short" : "hl_open_short";
        case DIR_CLOSE_LONG:
            return std::llabs(end) > eps ? "hl_partial_long" : "hl_close_long";
        case DIR_CLOSE_SHORT:
            return std::llabs(end) > eps ? "hl_partial_short" : "hl_close_short";
        case DIR_FLIP:        return "hl_flip";
        case DIR_LIQ_LONG:    return "hl_liq_long";
        case DIR_LIQ_SHORT:   return "hl_liq_short";
        case DIR_LIQ_OTHER:   return "hl_liquidated";
        default:              return "hl_trade";
    }
}

std::string dirEmoji(const std::string& key) {
    if (key == "hl_open_long" || key == "hl_add_long" ||
        key == "hl_close_short" || key == "hl_partial_short") return "\U0001F7E2";
    if (key == "hl_close_long" || key == "hl_partial_long" ||
        key == "hl_open_short" || key == "hl_add_short") return "\U0001F534";
    if (key == "hl_liquidated" || key == "hl_liq_long" || key == "hl_liq_short")
        return "\U0001F4A5";
    return "\u26A1";
}

std::string dirTitle(Lang lang, const std::string& key) {
    std::string t = tr(lang, key);
    if (t != key) return t;
    const bool ru = (lang == Lang::RU);
    if (key == "hl_add_long")      return ru ? "Добор лонга" : "Added to long";
    if (key == "hl_add_short")     return ru ? "Добор шорта" : "Added to short";
    if (key == "hl_partial_long")  return ru ? "Частичное закрытие лонга" : "Partial close long";
    if (key == "hl_partial_short") return ru ? "Частичное закрытие шорта" : "Partial close short";
    return t;
}

struct HlAlertData {
    std::string coin;
    std::string dirKey;
    int fillCount = 0;
    long long notionalNanos = 0;
    long long closedPnlNanos = 0;
    long long qtyNanos = 0;
    long long avgPxNanos = 0;
    PositionInfo pos;
    long long accountValueNanos = 0;
};

std::string buildHlAlert(const std::string& label, const HlAlertData& a, Lang lang) {
    const std::string coin = safeString(a.coin.empty() ? "?" : a.coin, 32);
    const std::string& key = a.dirKey;
    const long long closedPnlNanos = a.closedPnlNanos;
    const long long notionalNanosVal = a.notionalNanos;
    const PositionInfo& pos = a.pos;
    const long long accountValueNanos = a.accountValueNanos;

    const char* const dm = dirMark(lang);

    std::stringstream m;
    m << dm << "\U0001F4BC <b>" << safeString(label) << "</b>\n\n";
    m << dm << dirEmoji(key) << " <b>" << dirTitle(lang, key) << " " << coin << "</b>"
      << " \u00B7 <b>" << fmtUsd(notionalNanosVal) << "</b>\n";

    if (pos.known && pos.leverage > 0) {
        m << dm << pos.leverage << "\u00D7 " << tr(lang, pos.isolated ? "hl_isolated" : "hl_cross");
        if (a.fillCount > 1)
            m << " \u00B7 " << a.fillCount << " " << tr(lang, "hl_fills_in_series");
        m << "\n";
    } else if (a.fillCount > 1) {
        m << dm << tr(lang, "hl_fills_in_series") << ": " << a.fillCount << "\n";
    }

    if (a.avgPxNanos > 0)
        m << dm << "\U0001F4CD " << tr(lang, "hl_price") << ": <b>"
          << formatPriceNanos(a.avgPxNanos) << "</b>\n";
    if (a.qtyNanos > 0)
        m << dm << "\U0001F4E6 " << tr(lang, "hl_qty") << ": <b>"
          << formatQtyNanos(a.qtyNanos) << " " << coin << "</b>\n";

    if (pos.stillOpen && pos.positionValueNanos > 0) {
        const bool leftover = a.dirKey == "hl_partial_long" || a.dirKey == "hl_partial_short";
        m << dm << "\U0001F4CA " << tr(lang, leftover ? "hl_position_left" : "hl_position_size")
          << ": <b>" << fmtUsd(pos.positionValueNanos) << "</b>\n";
    }

    if (pos.stillOpen && pos.marginUsedNanos > 0) {
        m << dm << "\U0001F4B5 " << tr(lang, "hl_collateral") << ": <b>"
          << fmtUsd(pos.marginUsedNanos) << "</b>";
        if (accountValueNanos > 0) {
            const double share = 100.0 * static_cast<double>(pos.marginUsedNanos)
                                        / static_cast<double>(accountValueNanos);
            m << " \u00B7 " << formatPercent(share, false) << " " << tr(lang, "hl_of_account");
        }
        m << "\n";
    }

    if (closedPnlNanos != 0) {
        m << dm << (closedPnlNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ")
          << tr(lang, "hl_pnl") << ": <b>"
          << formatUsdNanosSigned(closedPnlNanos, true) << "</b>\n";
    }

    if (pos.stillOpen && pos.liquidationPxNanos > 0) {
        m << dm << "\u2620\uFE0F " << tr(lang, "hl_liq") << ": <b>"
          << formatPriceNanos(pos.liquidationPxNanos) << "</b>\n";
    } else if (pos.known && !pos.stillOpen) {
        m << dm << "\u2705 " << tr(lang, "hl_position_closed") << "\n";
    }
    if (accountValueNanos > 0) {
        m << dm << "\U0001F3E6 " << tr(lang, "hl_account") << ": <b>"
          << fmtUsd(accountValueNanos) << "</b>";
    }

    return m.str();
}

constexpr long long HL_ALERT_IDLE_SEC = 12;
constexpr long long HL_ALERT_AGGREGATION_SEC = 60;

struct PendingHlAlert {
    std::string wallet;
    HlAlertData data;
    long long firstSeen = 0;
    long long lastSeen = 0;
};

std::mutex g_hlAlertMutex;
std::map<std::string, PendingHlAlert> g_hlPendingAlerts;

struct AlertCycle {
    int gen = 0;
    std::string lastGroup;
};
std::map<std::string, AlertCycle> g_hlAlertCycle;

std::string alertGroupOf(const std::string& dirKey) {
    if (dirKey == "hl_partial_long"  || dirKey == "hl_close_long")  return "close_long";
    if (dirKey == "hl_partial_short" || dirKey == "hl_close_short") return "close_short";
    if (dirKey == "hl_add_long"  || dirKey == "hl_open_long")  return "open_long";
    if (dirKey == "hl_add_short" || dirKey == "hl_open_short") return "open_short";
    return dirKey;
}

bool isOpenGroup(const std::string& g) {
    return g == "open_long" || g == "open_short";
}

bool isCloseGroup(const std::string& g) {
    return g == "close_long" || g == "close_short"
        || g == "hl_flip" || g == "hl_liq_long" || g == "hl_liq_short"
        || g == "hl_liquidated";
}

int alertTitleRank(const std::string& k) {
    if (k == "hl_liq_long" || k == "hl_liq_short" || k == "hl_liquidated") return 6;
    if (k == "hl_flip") return 5;
    if (k == "hl_close_long" || k == "hl_close_short") return 4;
    if (k == "hl_partial_long" || k == "hl_partial_short") return 3;
    if (k == "hl_open_long" || k == "hl_open_short") return 2;
    if (k == "hl_add_long" || k == "hl_add_short") return 1;
    return 0;
}

void dispatchHlAlert(const std::string& wallet, const HlAlertData& a) {
    const long long notionalNanosVal = a.notionalNanos;
    if (notionalNanosVal <= 0) return;
    std::vector<HlRecipient> recipients = hlWatchersFor(wallet);
    if (recipients.empty()) return;

    std::map<std::pair<std::string, Lang>, std::vector<std::string>> byLabelLang;
    for (const HlRecipient& r : recipients) {
        if (r.chatId == SERVICE_CHAT_ID) continue;
        if (!isPremium(r.chatId)) continue;
        if (static_cast<uint64_t>(notionalNanosVal) < r.thresholdNanos) continue;
        Lang lang = langFromCode(getUserLanguage(r.chatId));
        byLabelLang[{r.label, lang}].push_back(r.chatId);
    }
    if (byLabelLang.empty()) return;

    for (auto& entry : byLabelLang) {
        std::string msg = buildHlAlert(entry.first.first, a, entry.first.second);
        if (g_msgQueue.enqueueToRecipients(msg, entry.second))
            g_alertsSent.fetch_add(1, std::memory_order_relaxed);
    }
}

void bufferHlAlert(const std::string& wallet, const HlAlertData& a) {
    const std::string group = alertGroupOf(a.dirKey);
    const std::string cycleKey = wallet + "|" + a.coin;
    std::lock_guard<std::mutex> l(g_hlAlertMutex);
    AlertCycle& c = g_hlAlertCycle[cycleKey];
    if (isOpenGroup(group) && isCloseGroup(c.lastGroup)) c.gen++;
    c.lastGroup = group;
    const std::string key = cycleKey + "|" + group + "|" + std::to_string(c.gen);
    const long long now = nowSec();
    auto it = g_hlPendingAlerts.find(key);
    if (it == g_hlPendingAlerts.end()) {
        g_hlPendingAlerts.emplace(key, PendingHlAlert{wallet, a, now, now});
        return;
    }
    HlAlertData& acc = it->second.data;
    if (alertTitleRank(a.dirKey) >= alertTitleRank(acc.dirKey))
        acc.dirKey = a.dirKey;
    acc.fillCount      += a.fillCount;
    acc.notionalNanos  += a.notionalNanos;
    acc.closedPnlNanos += a.closedPnlNanos;
    acc.qtyNanos       += a.qtyNanos;
    acc.pos = a.pos;
    acc.accountValueNanos = a.accountValueNanos;
    it->second.lastSeen = now;
}

void flushHlAlerts(bool force) {
    std::vector<PendingHlAlert> ready;
    {
        std::lock_guard<std::mutex> l(g_hlAlertMutex);
        const long long now = nowSec();
        for (auto it = g_hlPendingAlerts.begin(); it != g_hlPendingAlerts.end(); ) {
            if (force
                || now - it->second.lastSeen >= HL_ALERT_IDLE_SEC
                || now - it->second.firstSeen >= HL_ALERT_AGGREGATION_SEC) {
                ready.push_back(std::move(it->second));
                it = g_hlPendingAlerts.erase(it);
            } else ++it;
        }
    }
    // Чистим память циклов: запись нужна только пока по монете идёт серия.
    // Без этого карта растёт с каждым новым кошельком и монетой навсегда.
    {
        std::lock_guard<std::mutex> l(g_hlAlertMutex);
        if (g_hlPendingAlerts.empty() && g_hlAlertCycle.size() > 20000)
            g_hlAlertCycle.clear();
    }

    for (PendingHlAlert& p : ready) {
        HlAlertData& a = p.data;
        if (a.qtyNanos > 0) {
            const __int128 num = static_cast<__int128>(a.notionalNanos) * NANOS_PER_UNIT;
            a.avgPxNanos = static_cast<long long>(num / a.qtyNanos);
        }
        dispatchHlAlert(p.wallet, a);
    }
}

void enrichWallet(const std::string& wallet) {
    if (isBanned(wallet)) return;

    WalletState st;
    const bool known = loadWalletState(wallet, st);
    const long long now = nowSec();

    bool userWatch = false;
    for (const HlRecipient& r : hlWatchersFor(wallet)) {
        if (r.chatId != SERVICE_CHAT_ID && isPremium(r.chatId)) { userWatch = true; break; }
    }

    const long long startMs = (known && st.seeded)
        ? st.lastFillTs + 1
        : nowMs() - 120000LL;

    json fills = fetchFillsPage(wallet, startMs);
    if (!fills.is_array()) {
        if (userWatch) queueUrgent(wallet);
        else queueWallet(wallet);
        return;
    }

    g_enriched.fetch_add(1, std::memory_order_relaxed);

    const bool wasSeeded = known && st.seeded;
    const bool liveAlerts = wasSeeded || userWatch;
    const long long stateAt = nowSec();

    struct Prepared {
        const json* fill;
        long long closedPnl;
        long long fee;
        long long notional;
        PositionInfo pos;
    };
    std::vector<Prepared> prepared;
    prepared.reserve(fills.size());
    size_t skippedNonPerp = 0;
    long long maxTs = st.lastFillTs;

    for (const auto& f : fills) {
        if (!f.is_object()) continue;
        const long long ts = f.value("time", 0LL);
        if (ts > maxTs) maxTs = ts;

        if (!isPerpCoin(jstr(f, "coin"))) { skippedNonPerp++; continue; }

        Prepared p;
        p.fill = &f;
        p.closedPnl = 0;
        p.fee = 0;
        parseDecimalToNanos(jstr(f, "closedPnl", "0"), p.closedPnl);
        parseDecimalToNanos(jstr(f, "fee", "0"), p.fee);
        const std::string feeToken = jstr(f, "feeToken", "USDC");
        if (!feeToken.empty() && feeToken != "USDC") {
            long long px = 0;
            parseDecimalToNanos(jstr(f, "px", "0"), px);
            if (px > 0) {
                const __int128 usd = static_cast<__int128>(p.fee) * px / NANOS_PER_UNIT;
                p.fee = static_cast<long long>(usd);
            }
        }
        p.notional = 0;
        notionalNanos(jstr(f, "px", "0"), jstr(f, "sz", "0"), p.notional);
        p.pos = lastKnownPosition(wallet, jstr(f, "coin"));
        prepared.push_back(std::move(p));
    }

    if (!fills.empty() && liveAlerts) fetchAccountState(wallet);

    const long long accountValue = lastKnownAccountValue(wallet);
    for (Prepared& p : prepared) {
        PositionInfo now = lastKnownPosition(wallet, jstr(*p.fill, "coin"));
        if (!p.pos.known) p.pos = now;
        p.pos.stillOpen = now.known && now.snapshotAt >= stateAt;
    }

    std::vector<size_t> freshRows;
    size_t stored = 0;
    {
        const bool bulk = prepared.size() > 20;
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (g_hlDb) {
            if (bulk) sqlite3_exec(g_hlDb, "BEGIN", nullptr, nullptr, nullptr);
            for (size_t i = 0; i < prepared.size(); i++) {
                const Prepared& p = prepared[i];
                if (saveFillLocked(wallet, *p.fill, p.closedPnl, p.fee, p.notional, p.pos,
                                   accountValue)) {
                    stored++;
                    if (p.notional > 0) freshRows.push_back(i);
                }
            }
            if (bulk && sqlite3_exec(g_hlDb, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
                std::cerr << "[HL] COMMIT сделок не прошёл: " << sqlite3_errmsg(g_hlDb) << std::endl;
                sqlite3_exec(g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
                stored = 0;
                freshRows.clear();
            }
        }
    }

    if (liveAlerts && !freshRows.empty()) {
        std::map<std::string, HlAlertData> series;
        for (size_t i : freshRows) {
            const Prepared& p = prepared[i];
            const std::string coin = jstr(*p.fill, "coin");
            const std::string dk = dirKey(*p.fill);

            HlAlertData& a = series[coin + "|" + dk];
            if (a.fillCount == 0) {
                a.coin = coin;
                a.dirKey = dk;
            }
            a.fillCount++;
            a.notionalNanos += p.notional;
            a.closedPnlNanos += p.closedPnl;
            long long sz = 0;
            if (parseDecimalToNanos(jstr(*p.fill, "sz", "0"), sz)) {
                if (sz < 0) sz = -sz;
                a.qtyNanos += sz;
            }
            a.pos = p.pos;
            a.accountValueNanos = accountValue;
        }

        for (auto& kv : series) bufferHlAlert(wallet, kv.second);
    }

    if (!fills.empty() && skippedNonPerp == fills.size()) {
        std::cerr << "[HL] ВНИМАНИЕ: у " << wallet << " отсеяны ВСЕ " << fills.size()
                  << " сделок как нефьючерсные. Первая монета: "
                  << jstr(fills[0], "coin", "?")
                  << " - сверь с именами рынков из meta." << std::endl;
    } else if (stored > 0 && !wasSeeded) {
        std::cout << "[HL] первичное наполнение " << wallet << ": " << stored
                  << " сделок записано молча" << std::endl;
    }

    if (liveAlerts && fills.size() >= static_cast<size_t>(HL_HYPERACTIVE_FILLS)) {
        if (st.debounce < HL_HYPERACTIVE_DEBOUNCE_SEC) {
            st.debounce = HL_HYPERACTIVE_DEBOUNCE_SEC;
            std::cout << "[HL] " << wallet << " торгует слишком часто, опрос раз в "
                      << HL_HYPERACTIVE_DEBOUNCE_SEC << "с" << std::endl;
        }
    }
    if (maxTs < startMs) maxTs = startMs - 1;
    st.seeded = true;
    st.backfilled30d = true;
    st.lastFillTs = maxTs;
    st.lastEnriched = now;
    if (st.debounce != HL_HYPERACTIVE_DEBOUNCE_SEC)
        st.debounce = userWatch ? HL_USER_DEBOUNCE_SEC : HL_SERVICE_DEBOUNCE_SEC;
    saveWalletState(wallet, st);
    setNextEnrich(wallet, now + st.debounce);

    const int total = countFillsInWindow(wallet);
    if (total > HL_MAX_BOT_TRADES && banWallet(wallet, total)) {
        g_botsBanned.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[HL] бот на фьючерсах: " << wallet << " - " << total
                  << " ордеров за 30 дней, исключён из перп-рейтинга" << std::endl;
    }
}

void backfillFlatFlags() {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return;
    sqlite3_stmt* c = nullptr;
    bool have = false;
    if (prepareOrLog(g_hlDb, &c, "SELECT 1 FROM hl_fills WHERE flat=1 LIMIT 1")) {
        have = sqlite3_step(c) == SQLITE_ROW;
        sqlite3_finalize(c);
    }
    if (have) return;

    sqlite3_exec(g_hlDb,
        "UPDATE hl_fills SET flat=1 WHERE dir_code >= 5 AND flat=0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(g_hlDb,
        "UPDATE hl_fills SET flat=CASE"
        " WHEN NOT EXISTS ("
        "   SELECT 1 FROM hl_fills n"
        "   WHERE n.wallet=hl_fills.wallet AND n.coin=hl_fills.coin"
        "     AND n.ts>hl_fills.ts AND n.dir_code >= 3"
        "     AND NOT EXISTS ("
        "       SELECT 1 FROM hl_fills m"
        "       WHERE m.wallet=hl_fills.wallet AND m.coin=hl_fills.coin"
        "         AND m.ts>hl_fills.ts AND m.ts<n.ts"
        "         AND m.dir_code IN (1,2,5)))"
        " THEN 1 ELSE 0 END"
        " WHERE dir_code IN (3,4)",
        nullptr, nullptr, nullptr);
    int nflat = 0;
    if (prepareOrLog(g_hlDb, &c, "SELECT COUNT(*) FROM hl_fills WHERE flat=1")) {
        if (sqlite3_step(c) == SQLITE_ROW) nflat = sqlite3_column_int(c, 0);
        sqlite3_finalize(c);
    }
    std::cout << "[HL] сделок-кругов в базе (flat): " << nflat << std::endl;
}

void dripFundingHistory() {
    if (budgetLeft() < HL_WEIGHT_FUNDING_HIST + 40) return;
    std::vector<std::string> coins;
    {
        std::lock_guard<std::mutex> l(g_coinsMutex);
        coins.assign(g_perpCoins.begin(), g_perpCoins.end());
    }
    if (coins.empty()) return;

    std::string pick;
    {
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (!g_hlDb) return;
        sqlite3_stmt* s = nullptr;
        std::set<std::string> done;
        if (prepareOrLog(g_hlDb, &s, "SELECT coin FROM hl_funding_backfill WHERE done=1")) {
            while (sqlite3_step(s) == SQLITE_ROW) done.insert(safeColumnText(s, 0));
            sqlite3_finalize(s);
        }
        for (const auto& c : coins) {
            if (!done.count(c)) { pick = c; break; }
        }
    }
    if (pick.empty()) return;

    json body;
    body["type"] = "fundingHistory";
    body["coin"] = pick;
    body["startTime"] = (nowSec() - 30LL * 86400LL) * 1000LL;
    json j = infoPost(body, HL_WEIGHT_FUNDING_HIST);
    if (!j.is_array()) return;

    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return;
    if (sqlite3_exec(g_hlDb, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) return;
    sqlite3_stmt* ins = nullptr;
    if (!prepareOrLog(g_hlDb, &ins,
            "INSERT INTO hl_funding_rate(coin,hour_ts,rate_nanos,mark_nanos,oi_nanos,day_vlm_nanos) "
            "VALUES(?,?,?,?,0,0) "
            "ON CONFLICT(coin,hour_ts) DO UPDATE SET "
            "rate_nanos=excluded.rate_nanos,"
            "mark_nanos=CASE WHEN excluded.mark_nanos>0 THEN excluded.mark_nanos ELSE hl_funding_rate.mark_nanos END")) {
        sqlite3_exec(g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
        return;
    }
    int n = 0;
    for (const auto& row : j) {
        if (!row.is_object()) continue;
        std::string coin = jstr(row, "coin", pick.c_str());
        if (coin.empty()) coin = pick;
        long long rate = 0;
        parseDecimalToNanos(jstr(row, "fundingRate", "0"), rate);
        long long tms = row.value("time", 0LL);
        if (tms <= 0) continue;
        const long long hour = hourFloorSec(tms / 1000LL);
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_text(ins, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins, 2, hour);
        sqlite3_bind_int64(ins, 3, rate);
        sqlite3_bind_int64(ins, 4, 0);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            sqlite3_finalize(ins);
            sqlite3_exec(g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
            return;
        }
        n++;
    }
    sqlite3_finalize(ins);
    sqlite3_stmt* d = nullptr;
    if (prepareOrLog(g_hlDb, &d, "INSERT OR REPLACE INTO hl_funding_backfill(coin,done) VALUES(?,1)")) {
        sqlite3_bind_text(d, 1, pick.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(d);
        sqlite3_finalize(d);
    }
    if (sqlite3_exec(g_hlDb, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[HL] dripFundingHistory COMMIT: " << sqlite3_errmsg(g_hlDb) << std::endl;
        sqlite3_exec(g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
        return;
    }
    std::cout << "[HL] фандинг " << pick << ": " << n << " часов" << std::endl;
}

void enricherLoop() {
    long long lastCleanup = 0;
    long long lastRank = 0;
    try {
        backfillFlatFlags();
        rebuildRankCache();
        lastRank = nowSec();
    } catch (const std::exception& e) {
        std::cerr << "[HL] сбой пересборки рейтинга: " << e.what() << std::endl;
    }
    while (keepGoing()) {
        std::set<std::string> batch;
        bool urgent = false;
        {
            std::lock_guard<std::mutex> l(g_queueMutex);
            if (!g_urgentQueue.empty()) {
                batch.swap(g_urgentQueue);
                urgent = true;
            } else {
                auto it = g_enrichQueue.begin();
                for (int i = 0; i < 5 && it != g_enrichQueue.end(); ) {
                    batch.insert(*it);
                    it = g_enrichQueue.erase(it);
                    ++i;
                }
            }
        }
        for (const std::string& w : batch) {
            if (!keepGoing()) break;
            if (!readyToEnrich(w)) {
                if (urgent) queueUrgent(w);
                else queueWallet(w);
                continue;
            }
            try {
                enrichWallet(w);
            } catch (const std::exception& e) {
                std::cerr << "[HL] сбой дозагрузки " << w << ": " << e.what() << std::endl;
                rollbackIfOpen();
            } catch (...) {
                std::cerr << "[HL] неизвестный сбой дозагрузки " << w << std::endl;
                rollbackIfOpen();
            }
        }
        flushHlAlerts(false);
        if (!urgent) dripFundingHistory();
        if (perpCoinsLoaded() &&
            (lastCleanup == 0 || nowSec() - lastCleanup >= HL_CLEANUP_INTERVAL_SEC)) {
            purgeNonPerpFills();
            cleanupOldFills();
            forgetUnwatchedAccounts();
            lastCleanup = nowSec();
        }

        if (lastRank == 0 || nowSec() - lastRank >= HL_RANK_REBUILD_SEC) {
            try { rebuildRankCache(); }
            catch (const std::exception& e) {
                std::cerr << "[HL] сбой пересборки рейтинга: " << e.what() << std::endl;
            }
            lastRank = nowSec();
        }

        for (int i = 0; i < 20 && keepGoing(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    flushHlAlerts(true);
    std::cout << "[HL] дозагрузчик остановлен" << std::endl;
}

bool wsSendText(CURL* c, const std::string& payload) {
    size_t sent = 0;
    CURLcode rc = curl_ws_send(c, payload.data(), payload.size(), &sent, 0, CURLWS_TEXT);
    if (rc != CURLE_OK || sent != payload.size()) {
        if (rc != CURLE_OK)
            std::cerr << "[HL] отправка не удалась: " << curl_easy_strerror(rc) << std::endl;
        return false;
    }
    return true;
}

bool wsSubscribeTrades(CURL* c, const std::string& coin) {
    json sub;
    sub["method"] = "subscribe";
    sub["subscription"]["type"] = "trades";
    sub["subscription"]["coin"] = coin;
    return wsSendText(c, sub.dump());
}

void waitReadable(CURL* c, int timeoutMs) {
    curl_socket_t sock = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK || sock == CURL_SOCKET_BAD) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return;
    }
    struct pollfd pfd;
    pfd.fd = static_cast<int>(sock);
    pfd.events = POLLIN;
    pfd.revents = 0;
    poll(&pfd, 1, timeoutMs);
}

void handleTrades(const json& data) {
    if (!data.is_array()) return;

    const auto watchedPtr = watchedSnapshot();
    const auto bannedPtr = bannedSnapshot();
    const AddressSet& watched = *watchedPtr;
    const AddressSet& banned = *bannedPtr;
    if (watched.empty()) {
        g_tradesSeen.fetch_add(data.size(), std::memory_order_relaxed);
        return;
    }

    for (const auto& t : data) {
        if (!t.is_object()) continue;
        g_tradesSeen.fetch_add(1, std::memory_order_relaxed);
        if (!t.contains("users") || !t["users"].is_array()) continue;

        for (const auto& u : t["users"]) {
            if (!u.is_string()) continue;
            const std::string addr = toLower(u.get<std::string>());
            if (!watched.count(addr)) continue;
            if (banned.count(addr)) continue;

            g_hits.fetch_add(1, std::memory_order_relaxed);

            long long notional = 0;
            if (!notionalNanos(jstr(t, "px", "0"),
                               jstr(t, "sz", "0"), notional)) continue;

            bool serviceWatched = false;
            uint64_t minThreshold = 0;
            bool haveUser = false;
            for (const HlRecipient& r : hlWatchersFor(addr)) {
                if (r.chatId == SERVICE_CHAT_ID) { serviceWatched = true; continue; }
                if (!isPremium(r.chatId)) continue;
                if (!haveUser || r.thresholdNanos < minThreshold) {
                    minThreshold = r.thresholdNanos;
                    haveUser = true;
                }
            }
            if (!serviceWatched && !haveUser) continue;
            if (!serviceWatched && static_cast<uint64_t>(notional) < minThreshold) continue;

            if (haveUser) queueUrgent(addr);
            else queueWallet(addr);
        }
    }
}

void handleWsMessage(const std::string& raw) {
    g_lastMsgTs.store(nowSec(), std::memory_order_relaxed);

    json j = json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    const std::string channel = jstr(j, "channel");
    if (channel == "trades") {
        try {
            if (j.contains("data")) handleTrades(j["data"]);
        } catch (const std::exception& e) {
            std::cerr << "[HL] сбой разбора сделок: " << e.what() << std::endl;
        }
    } else if (channel == "subscriptionResponse") {
        g_subscribedCoins.fetch_add(1, std::memory_order_relaxed);
    } else if (channel == "error") {
        std::cerr << "[HL] ошибка площадки: " << safeString(raw, 300) << std::endl;
    }
}

bool runSession(const std::vector<std::string>& coins) {
    CURL* c = curl_easy_init();
    if (!c) return false;

    curl_easy_setopt(c, CURLOPT_URL, HL_WS_URL);
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        if (rc == CURLE_UNSUPPORTED_PROTOCOL)
            logCritical("[HL] libcurl собран без поддержки WebSocket - перпы работать не будут");
        else
            std::cerr << "[HL] соединение не установлено: " << curl_easy_strerror(rc) << std::endl;
        curl_easy_cleanup(c);
        return false;
    }

    g_connected.store(true, std::memory_order_relaxed);
    g_subscribedCoins.store(0, std::memory_order_relaxed);
    g_lastMsgTs.store(nowSec(), std::memory_order_relaxed);
    std::cout << "[HL] соединение установлено, рынков к подписке: " << coins.size() << std::endl;

    bool alive = true;
    for (const std::string& coin : coins) {
        if (!keepGoing()) { alive = false; break; }
        if (!wsSubscribeTrades(c, coin)) { alive = false; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(HL_SUBSCRIBE_PACE_MS));
    }

    long long lastPing = nowSec();
    std::string message;

    while (alive && keepGoing()) {
        char buf[16384];
        size_t rlen = 0;
        const struct curl_ws_frame* meta = nullptr;

        CURLcode r = curl_ws_recv(c, buf, sizeof(buf), &rlen, &meta);

        if (r == CURLE_AGAIN) {
            waitReadable(c, 500);
        } else if (r != CURLE_OK) {
            std::cerr << "[HL] соединение потеряно: " << curl_easy_strerror(r) << std::endl;
            alive = false;
        } else if (meta && (meta->flags & CURLWS_CLOSE)) {
            std::cerr << "[HL] площадка закрыла соединение" << std::endl;
            alive = false;
        } else if (meta && (meta->flags & (CURLWS_PING | CURLWS_PONG))) {
        } else if (meta) {
            message.append(buf, rlen);
            if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
                handleWsMessage(message);
                message.clear();
            }
        }

        if (!alive) break;

        const long long now = nowSec();
        if (now - lastPing >= HL_PING_INTERVAL_SEC) {
            if (!wsSendText(c, "{\"method\":\"ping\"}")) break;
            lastPing = now;
        }
        if (now - g_lastMsgTs.load(std::memory_order_relaxed) > HL_SILENCE_TIMEOUT_SEC) {
            std::cerr << "[HL] тишина в канале, переподключаюсь" << std::endl;
            break;
        }
    }

    g_connected.store(false, std::memory_order_relaxed);
    curl_easy_cleanup(c);
    return true;
}

void feedLoop() {
    std::vector<std::string> coins;
    long long coinsFetchedAt = 0;
    long long walletsLoadedAt = 0;
    int backoff = HL_RECONNECT_MIN_SEC;

    while (keepGoing()) {
        const long long now = nowSec();

        if (coins.empty() || now - coinsFetchedAt >= HL_COINS_REFRESH_SEC) {
            std::vector<std::string> fresh, allPerps;
            if (fetchPerpCoins(fresh, allPerps)) {
                coins.swap(fresh);
                setPerpCoins(allPerps);
                coinsFetchedAt = now;
            } else if (coins.empty()) {
                std::cerr << "[HL] список рынков получить не удалось, повтор через "
                          << backoff << "с" << std::endl;
                interruptibleSleep(backoff);
                backoff = std::min(backoff * 2, HL_RECONNECT_MAX_SEC);
                continue;
            }
        }

        if (now - walletsLoadedAt >= HL_WALLETS_REFRESH_SEC) {
            reloadWatchedWallets();
            walletsLoadedAt = now;
        }

        const bool connected = runSession(coins);
        if (!keepGoing()) break;

        g_reconnects.fetch_add(1, std::memory_order_relaxed);
        backoff = connected ? HL_RECONNECT_MIN_SEC : std::min(backoff * 2, HL_RECONNECT_MAX_SEC);
        interruptibleSleep(backoff);
    }

    std::cout << "[HL] поток фида остановлен" << std::endl;
}

}

using namespace hl;

bool initHyperliquid() {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (g_hlDb) return true;

    const std::string file = hlDbFile();
    if (sqlite3_open(file.c_str(), &g_hlDb) != SQLITE_OK) {
        std::cerr << "[HL] не открыть базу " << file << ": "
                  << (g_hlDb ? sqlite3_errmsg(g_hlDb) : "нет памяти") << std::endl;
        if (g_hlDb) { sqlite3_close(g_hlDb); g_hlDb = nullptr; }
        return false;
    }

    const char* schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS hl_fills ("
        "  tid INTEGER PRIMARY KEY,"
        "  wallet TEXT NOT NULL,"
        "  coin TEXT NOT NULL DEFAULT '',"
        "  dir TEXT NOT NULL DEFAULT '',"
        "  side TEXT NOT NULL DEFAULT '',"
        "  dir_code INTEGER NOT NULL DEFAULT 0,"
        "  px TEXT NOT NULL DEFAULT '',"
        "  sz TEXT NOT NULL DEFAULT '',"
        "  closed_pnl_nanos INTEGER NOT NULL DEFAULT 0,"
        "  fee_nanos INTEGER NOT NULL DEFAULT 0,"
        "  notional_nanos INTEGER NOT NULL DEFAULT 0,"
        "  margin_nanos INTEGER NOT NULL DEFAULT 0,"
        "  leverage INTEGER NOT NULL DEFAULT 0,"
        "  oid INTEGER NOT NULL DEFAULT 0,"
        "  account_value_nanos INTEGER NOT NULL DEFAULT 0,"
        "  ts INTEGER NOT NULL DEFAULT 0,"
        "  hash TEXT NOT NULL DEFAULT '',"
        "  flat INTEGER NOT NULL DEFAULT 0,"
        "  start_pos_nanos INTEGER);"
        "CREATE INDEX IF NOT EXISTS idx_hl_fills_wallet_ts ON hl_fills(wallet, ts);"
        "CREATE INDEX IF NOT EXISTS idx_hl_fills_ts ON hl_fills(ts);"
        "CREATE TABLE IF NOT EXISTS hl_wallet_state ("
        "  wallet TEXT PRIMARY KEY,"
        "  seeded INTEGER NOT NULL DEFAULT 0,"
        "  last_fill_ts INTEGER NOT NULL DEFAULT 0,"
        "  last_enriched INTEGER NOT NULL DEFAULT 0,"
        "  debounce_sec INTEGER NOT NULL DEFAULT 30,"
        "  backfilled_30d INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS hl_banned ("
        "  wallet TEXT PRIMARY KEY,"
        "  banned_at INTEGER NOT NULL DEFAULT 0,"
        "  trades INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS hl_funding_rate ("
        "  coin TEXT NOT NULL,"
        "  hour_ts INTEGER NOT NULL,"
        "  rate_nanos INTEGER NOT NULL DEFAULT 0,"
        "  mark_nanos INTEGER NOT NULL DEFAULT 0,"
        "  oi_nanos INTEGER NOT NULL DEFAULT 0,"
        "  day_vlm_nanos INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (coin, hour_ts));"
        "CREATE TABLE IF NOT EXISTS hl_funding_backfill ("
        "  coin TEXT PRIMARY KEY,"
        "  done INTEGER NOT NULL DEFAULT 0);";

    char* err = nullptr;
    if (sqlite3_exec(g_hlDb, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[HL] схема не создана: " << (err ? err : "?") << std::endl;
        if (err) sqlite3_free(err);
        sqlite3_close(g_hlDb);
        g_hlDb = nullptr;
        return false;
    }

    const char* const migrations[] = {
        "ALTER TABLE hl_fills ADD COLUMN dir_code INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN notional_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN margin_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN leverage INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN oid INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN account_value_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_wallet_state ADD COLUMN backfilled_30d INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN flat INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN start_pos_nanos INTEGER",
        "ALTER TABLE hl_funding_rate ADD COLUMN oi_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_funding_rate ADD COLUMN day_vlm_nanos INTEGER NOT NULL DEFAULT 0",
    };
    for (const char* sql : migrations) {
        char* mErr = nullptr;
        if (sqlite3_exec(g_hlDb, sql, nullptr, nullptr, &mErr) == SQLITE_OK)
            std::cout << "[HL] база обновлена: " << sql << std::endl;
        if (mErr) sqlite3_free(mErr);
    }
    sqlite3_exec(g_hlDb,
        "UPDATE hl_fills SET account_value_nanos=0"
        " WHERE account_value_nanos > 0 AND account_value_nanos <= 100000000000",
        nullptr, nullptr, nullptr);

    {
        char* iErr = nullptr;
        sqlite3_exec(g_hlDb,
            "CREATE INDEX IF NOT EXISTS idx_hl_fills_code_ts ON hl_fills(dir_code, ts)",
            nullptr, nullptr, &iErr);
        if (iErr) sqlite3_free(iErr);
    }

    {
        const char* const backfill =
            "UPDATE hl_fills SET dir_code = CASE"
            "  WHEN (dir LIKE '%Liquidat%' OR dir LIKE '%ADL%') AND dir LIKE '%Short%' THEN 7"
            "  WHEN (dir LIKE '%Liquidat%' OR dir LIKE '%ADL%') AND dir LIKE '%Long%'  THEN 6"
            "  WHEN  dir LIKE '%Liquidat%' OR dir LIKE '%ADL%'                         THEN 8"
            "  WHEN dir LIKE '%Long > Short%' OR dir LIKE '%Short > Long%' THEN 5"
            "  WHEN dir LIKE '%Open Long%'   THEN 1"
            "  WHEN dir LIKE '%Open Short%'  THEN 2"
            "  WHEN dir LIKE '%Close Long%'  THEN 3"
            "  WHEN dir LIKE '%Close Short%' THEN 4"
            "  ELSE 0 END "
            "WHERE dir <> '' AND dir_code <> CASE"
            "  WHEN (dir LIKE '%Liquidat%' OR dir LIKE '%ADL%') AND dir LIKE '%Short%' THEN 7"
            "  WHEN (dir LIKE '%Liquidat%' OR dir LIKE '%ADL%') AND dir LIKE '%Long%'  THEN 6"
            "  WHEN  dir LIKE '%Liquidat%' OR dir LIKE '%ADL%'                         THEN 8"
            "  WHEN dir LIKE '%Long > Short%' OR dir LIKE '%Short > Long%' THEN 5"
            "  WHEN dir LIKE '%Open Long%'   THEN 1"
            "  WHEN dir LIKE '%Open Short%'  THEN 2"
            "  WHEN dir LIKE '%Close Long%'  THEN 3"
            "  WHEN dir LIKE '%Close Short%' THEN 4"
            "  ELSE 0 END";
        char* bErr = nullptr;
        if (sqlite3_exec(g_hlDb, backfill, nullptr, nullptr, &bErr) == SQLITE_OK) {
            const int n = sqlite3_changes(g_hlDb);
            if (n > 0) std::cout << "[HL] признак направления проставлен: " << n << std::endl;
        }
        if (bErr) sqlite3_free(bErr);
    }
    sqlite3_exec(g_hlDb,
        "CREATE INDEX IF NOT EXISTS idx_hl_fills_wallet_coin_ts ON hl_fills(wallet, coin, ts)",
        nullptr, nullptr, nullptr);

    sqlite3_stmt* probe = nullptr;
    if (sqlite3_prepare_v2(g_hlDb,
            "SELECT tid,notional_nanos,margin_nanos,leverage,oid,account_value_nanos,flat"
            " FROM hl_fills LIMIT 1",
            -1, &probe, nullptr) != SQLITE_OK) {
        logCritical(std::string("[HL] таблица сделок непригодна для записи: ")
                    + sqlite3_errmsg(g_hlDb) + " - перпы работать не будут");
        if (probe) sqlite3_finalize(probe);
        sqlite3_close(g_hlDb);
        g_hlDb = nullptr;
        return false;
    }
    sqlite3_finalize(probe);
    return true;
}

void startHyperliquidLoop() {
    if (g_hlRunning.exchange(true)) return;
    reloadWatchedWallets();
    g_feedThread = std::thread(feedLoop);
    g_enrichThread = std::thread(enricherLoop);
}

void stopHyperliquid() {
    g_hlRunning.store(false);
    if (g_feedThread.joinable()) g_feedThread.join();
    if (g_enrichThread.joinable()) g_enrichThread.join();
    flushHlAlerts(true);
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (g_hlDb) {
        sqlite3_close(g_hlDb);
        g_hlDb = nullptr;
    }
}
