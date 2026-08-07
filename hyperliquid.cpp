#include "hyperliquid.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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
std::string http(const std::string& url, const std::string& post, int timeout);
void logCritical(const std::string& msg);
std::string getUserLanguage(const std::string& chatId);
void rememberView(const std::string& chatId, const std::string& data);
std::string shortAddress(const std::string& a);

namespace {

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
constexpr int HL_WEIGHT_CLEARINGHOUSE = 2;
constexpr int HL_WEIGHT_META = 20;

constexpr long long HL_ENRICH_DEBOUNCE_SEC = 60;

constexpr long long HL_SEED_LOOKBACK_MS = 3600LL * 1000LL;

constexpr int HL_HYPERACTIVE_FILLS = 200;
constexpr long long HL_HYPERACTIVE_DEBOUNCE_SEC = 600;

constexpr long long NANOS_PER_UNIT = 1000000000LL;

constexpr long long HL_CLEANUP_INTERVAL_SEC = 3600;
constexpr size_t HL_POSITION_CACHE_CAP = 20000;
constexpr int HL_PICKER_PER_PAGE = 5;

constexpr long long HL_RANK_WINDOW_SEC = 30LL * 86400LL;
constexpr long long HL_FILL_TTL_SEC = HL_RANK_WINDOW_SEC;

constexpr int HL_MAX_BOT_TRADES = 1000;

std::mutex g_rankMutex;
long long g_rankBuiltAt = 0;

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

bool perpCoinsLoaded() {
    std::lock_guard<std::mutex> l(g_coinsMutex);
    return !g_perpCoins.empty();
}

bool isPerpCoin(const std::string& coin) {
    std::lock_guard<std::mutex> l(g_coinsMutex);
    if (g_perpCoins.empty()) return true;
    return g_perpCoins.count(coin) > 0;
}

void setPerpCoins(const std::vector<std::string>& coins) {
    std::set<std::string> fresh(coins.begin(), coins.end());
    std::lock_guard<std::mutex> l(g_coinsMutex);
    g_perpCoins.swap(fresh);
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
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return out;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s, "SELECT wallet FROM hl_banned")) return out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string w = safeColumnText(s, 0);
        if (!w.empty()) out.insert(w);
    }
    sqlite3_finalize(s);
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

bool spendBudget(int weight) {
    std::lock_guard<std::mutex> l(g_budgetMutex);
    long long minute = nowSec() / 60;
    if (minute != g_budgetWindow) { g_budgetWindow = minute; g_budgetSpent = 0; }
    if (g_budgetSpent + weight > HL_BUDGET_PER_MINUTE) return false;
    g_budgetSpent += weight;
    return true;
}

json infoPost(const json& body, int weight) {
    if (!spendBudget(weight)) {
        g_budgetSkips.fetch_add(1, std::memory_order_relaxed);
        return json();
    }
    std::string resp = http(HL_INFO_URL, body.dump(), 15);
    if (resp.empty()) return json();
    json j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) return json();
    return j;
}

std::mutex g_queueMutex;
std::set<std::string> g_enrichQueue;
std::unordered_map<std::string, long long> g_nextEnrichAt;

void queueWallet(const std::string& addr) {
    std::lock_guard<std::mutex> l(g_queueMutex);
    g_enrichQueue.insert(addr);
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

bool fetchPerpCoins(std::vector<std::string>& subscribe, std::vector<std::string>& all) {
    subscribe.clear();
    all.clear();
    json body;
    body["type"] = "meta";
    body["dex"] = "";
    json j = infoPost(body, HL_WEIGHT_META);
    if (!j.is_object() || !j.contains("universe") || !j["universe"].is_array()) {
        std::cerr << "[HL] meta: неожиданный ответ" << std::endl;
        return false;
    }
    for (const auto& u : j["universe"]) {
        if (!u.is_object() || !u.contains("name") || !u["name"].is_string()) continue;
        std::string name = u["name"].get<std::string>();
        if (name.empty()) continue;
        all.push_back(name);
        if (!u.value("isDelisted", false)) subscribe.push_back(name);
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

// Безопасное чтение строкового поля. json::value() бросает исключение, если
// ключ ЕСТЬ, но лежит в нём null - а площадка присылает null в liquidationPx у
// позиций без цены ликвидации и в некоторых других полях. Исключение из потока
// телеграма никто не ловит, и процесс падал целиком.
std::string jstr(const json& j, const char* key, const char* def = "") {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

std::string posKey(const std::string& wallet, const std::string& coin) {
    return wallet + "|" + coin;
}

void fetchAccountState(const std::string& wallet) {
    json body;
    body["type"] = "clearinghouseState";
    body["user"] = wallet;
    body["dex"] = "";
    json j = infoPost(body, HL_WEIGHT_CLEARINGHOUSE);
    if (!j.is_object()) return;

    long long accountValue = 0;
    if (j.contains("marginSummary") && j["marginSummary"].is_object())
        parseDecimalToNanos(jstr(j["marginSummary"], "accountValue", "0"), accountValue);

    {
        std::lock_guard<std::mutex> l(g_posMutex);
        if (accountValue > 0) g_accountValue[wallet] = accountValue;
    }

    if (!j.contains("assetPositions") || !j["assetPositions"].is_array()) return;

    // Закрытые позиции в ответе не приходят вовсе, поэтому старые записи о них
    // остались бы в кэше навсегда. Собираем монеты, которые сейчас открыты, и
    // всё остальное по этому кошельку удаляем.
    std::set<std::string> openNow;
    for (const auto& ap : j["assetPositions"]) {
        if (!ap.is_object() || !ap.contains("position") || !ap["position"].is_object()) continue;
        const std::string c = jstr(ap["position"], "coin");
        if (!c.empty()) openNow.insert(c);
    }

    std::lock_guard<std::mutex> l(g_posMutex);

    const std::string prefix = posKey(wallet, "");
    for (auto it = g_lastPositions.begin(); it != g_lastPositions.end(); ) {
        if (it->first.rfind(prefix, 0) == 0 &&
            openNow.find(it->first.substr(prefix.size())) == openNow.end())
            it = g_lastPositions.erase(it);
        else
            ++it;
    }

    // Предохранитель на случай, если кошельков станет очень много. Депозиты
    // счетов НЕ трогаем: они нужны для ROI и заново придут не скоро.
    if (g_lastPositions.size() > HL_POSITION_CACHE_CAP) {
        g_lastPositions.clear();
        std::cout << "[HL] кэш позиций сброшен по достижении предела" << std::endl;
    }

    for (const auto& ap : j["assetPositions"]) {
        if (!ap.is_object() || !ap.contains("position") || !ap["position"].is_object()) continue;
        const json& p = ap["position"];
        std::string coin = jstr(p, "coin");
        if (coin.empty()) continue;

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

        g_lastPositions[posKey(wallet, coin)] = info;
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
    long long lastFillTs = 0;
    long long lastEnriched = 0;
    long long debounce = HL_ENRICH_DEBOUNCE_SEC;
};

bool loadWalletState(const std::string& wallet, WalletState& out) {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return false;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT seeded, last_fill_ts, last_enriched, debounce_sec FROM hl_wallet_state WHERE wallet=?"))
        return false;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        out.seeded = sqlite3_column_int(s, 0) != 0;
        out.lastFillTs = sqlite3_column_int64(s, 1);
        out.lastEnriched = sqlite3_column_int64(s, 2);
        out.debounce = sqlite3_column_int64(s, 3);
        if (out.debounce < HL_ENRICH_DEBOUNCE_SEC) out.debounce = HL_ENRICH_DEBOUNCE_SEC;
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
            "INSERT INTO hl_wallet_state(wallet,seeded,last_fill_ts,last_enriched,debounce_sec) "
            "VALUES(?,?,?,?,?) ON CONFLICT(wallet) DO UPDATE SET "
            "seeded=excluded.seeded, last_fill_ts=excluded.last_fill_ts, "
            "last_enriched=excluded.last_enriched, debounce_sec=excluded.debounce_sec"))
        return;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, st.seeded ? 1 : 0);
    sqlite3_bind_int64(s, 3, st.lastFillTs);
    sqlite3_bind_int64(s, 4, st.lastEnriched);
    sqlite3_bind_int64(s, 5, st.debounce);
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

int countFillsInWindow(const std::string& wallet) {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return 0;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT COUNT(*) FROM hl_fills"
            " WHERE wallet=? AND ts >= ? AND closed_pnl_nanos != 0")) return 0;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (nowSec() - HL_RANK_WINDOW_SEC) * 1000LL);
    int n = 0;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

bool banWallet(const std::string& wallet, int trades) {
    bool saved = false;
    {
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (!g_hlDb) return false;
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(g_hlDb, &s,
                "INSERT OR IGNORE INTO hl_banned(wallet,banned_at,trades) VALUES(?,?,?)"))
            return false;
        sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, nowSec());
        sqlite3_bind_int(s, 3, trades);
        if (sqlite3_step(s) == SQLITE_DONE) saved = sqlite3_changes(g_hlDb) > 0;
        sqlite3_finalize(s);
    }
    if (!saved) return false;

    {
        std::lock_guard<std::mutex> l(g_walletsMutex);
        auto updated = std::make_shared<AddressSet>(*g_banned);
        updated->insert(wallet);
        g_banned = updated;
    }
    { std::lock_guard<std::mutex> l(g_rankMutex); g_rankBuiltAt = 0; }
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
            "notional_nanos,margin_nanos,leverage,oid,account_value_nanos,ts,hash) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
        return false;
    sqlite3_bind_int64(s, 1, tid);
    sqlite3_bind_text(s, 2, wallet.c_str(), -1, SQLITE_TRANSIENT);
    std::string coin = jstr(f, "coin");
    std::string dir  = jstr(f, "dir");
    std::string side = jstr(f, "side");
    std::string px   = jstr(f, "px");
    std::string sz   = jstr(f, "sz");
    std::string hash = jstr(f, "hash");
    sqlite3_bind_text(s, 3, coin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, dir.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, side.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 6, px.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 7, sz.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 8, closedPnlNanos);
    sqlite3_bind_int64(s, 9, feeNanos);
    sqlite3_bind_int64(s, 10, notionalNanosVal);
    sqlite3_bind_int64(s, 11, pos.known ? pos.marginUsedNanos : 0);
    sqlite3_bind_int(s,   12, pos.known ? pos.leverage : 0);
    sqlite3_bind_int64(s, 13, f.value("oid", 0LL));
    sqlite3_bind_int64(s, 14, accountValueNanos);
    sqlite3_bind_int64(s, 15, f.value("time", 0LL));
    sqlite3_bind_text(s,  16, hash.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    int changed = sqlite3_changes(g_hlDb);
    sqlite3_finalize(s);
    return ok && changed > 0;
}

std::string dirKey(const std::string& dir) {
    if (dir.find("Open Long")   != std::string::npos) return "hl_open_long";
    if (dir.find("Close Long")  != std::string::npos) return "hl_close_long";
    if (dir.find("Open Short")  != std::string::npos) return "hl_open_short";
    if (dir.find("Close Short") != std::string::npos) return "hl_close_short";
    if (dir.find("Long > Short") != std::string::npos ||
        dir.find("Short > Long") != std::string::npos) return "hl_flip";
    if (dir.find("Liquidat") != std::string::npos) {
        if (dir.find("Long")  != std::string::npos) return "hl_liq_long";
        if (dir.find("Short") != std::string::npos) return "hl_liq_short";
        return "hl_liquidated";
    }
    return "hl_trade";
}

std::string dirEmoji(const std::string& key) {
    if (key == "hl_open_long" || key == "hl_close_short") return "\U0001F7E2";
    if (key == "hl_close_long" || key == "hl_open_short") return "\U0001F534";
    if (key == "hl_liquidated" || key == "hl_liq_long" || key == "hl_liq_short")
        return "\U0001F4A5";
    return "\u26A1";
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
    m << dm << dirEmoji(key) << " <b>" << tr(lang, key) << " " << coin << "</b>\n";

    m << dm << "\U0001F4B0 " << tr(lang, "hl_trade_size") << ": <b>" << fmtUsd(notionalNanosVal) << "</b>\n";

    if (a.fillCount > 1) {
        m << dm << "\U0001F522 " << tr(lang, "hl_fills_in_series") << ": <b>"
          << a.fillCount << "</b>\n";
    }

    if (pos.stillOpen && pos.positionValueNanos > 0) {
        m << dm << "\U0001F4CA " << tr(lang, "hl_position_size") << ": <b>"
          << fmtUsd(pos.positionValueNanos) << "</b>\n";
    }

    if (pos.known && pos.leverage > 0) {
        m << dm << "\u2699\uFE0F " << tr(lang, "hl_leverage") << ": <b>" << pos.leverage << "\u00D7 "
          << tr(lang, pos.isolated ? "hl_isolated" : "hl_cross") << "</b>\n";
    }

    if (pos.stillOpen && pos.marginUsedNanos > 0) {
        m << dm << "\U0001F4B5 " << tr(lang, "hl_collateral") << ": <b>" << fmtUsd(pos.marginUsedNanos) << "</b>";
        if (accountValueNanos > 0) {
            const double share = 100.0 * static_cast<double>(pos.marginUsedNanos)
                                       / static_cast<double>(accountValueNanos);
            m << " <i>(" << formatPercent(share, false) << " " << tr(lang, "hl_of_account") << ")</i>";
        }
        m << "\n";
    }

    if (a.avgPxNanos > 0)
        m << dm << "\U0001F4CD " << tr(lang, "hl_price") << ": <b>"
          << formatPriceNanos(a.avgPxNanos) << "</b>\n";

    if (a.qtyNanos > 0)
        m << dm << "\U0001F4E6 " << tr(lang, "hl_qty") << ": <b>"
          << formatQtyNanos(a.qtyNanos) << " " << coin << "</b>\n";

    if (closedPnlNanos != 0) {
        m << dm << (closedPnlNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ") << tr(lang, "hl_pnl") << ": <b>"
          << formatUsdNanosSigned(closedPnlNanos, true) << "</b>\n";
    }

    if (pos.stillOpen && pos.liquidationPxNanos > 0) {
        m << dm << "\u2620\uFE0F " << tr(lang, "hl_liq") << ": <b>"
          << formatPriceNanos(pos.liquidationPxNanos) << "</b>\n";
    } else if (pos.known && !pos.stillOpen && closedPnlNanos != 0) {
        m << dm << "\u2705 " << tr(lang, "hl_position_closed") << "\n";
    }
    if (accountValueNanos > 0) {
        m << dm << "\U0001F3E6 " << tr(lang, "hl_account") << ": <b>" << fmtUsd(accountValueNanos) << "</b>\n";
    }

    m << "\n" << dm << HL_CARD_SEPARATOR << "\n";
    m << dm << "\U0001F535 " << tr(lang, "hl_venue") << ": <b>Hyperliquid</b>";
    return m.str();
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

void enrichWallet(const std::string& wallet) {
    if (isBanned(wallet)) return;

    WalletState st;
    const bool known = loadWalletState(wallet, st);
    const long long now = nowSec();

    if (known && now - st.lastEnriched < st.debounce) {
        setNextEnrich(wallet, st.lastEnriched + st.debounce);
        queueWallet(wallet);
        return;
    }

    const long long startMs = (known && st.seeded)
        ? st.lastFillTs + 1
        : nowMs() - HL_SEED_LOOKBACK_MS;

    json body;
    body["type"] = "userFillsByTime";
    body["user"] = wallet;
    body["startTime"] = startMs;
    body["aggregateByTime"] = true;
    json fills = infoPost(body, HL_WEIGHT_USER_FILLS);
    if (!fills.is_array()) {
        queueWallet(wallet);
        return;
    }

    g_enriched.fetch_add(1, std::memory_order_relaxed);

    const bool wasSeeded = known && st.seeded;
    const long long stateAt = nowSec();
    if (!fills.empty()) fetchAccountState(wallet);

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
        p.notional = 0;
        notionalNanos(jstr(f, "px", "0"), jstr(f, "sz", "0"), p.notional);
        p.pos = lastKnownPosition(wallet, jstr(f, "coin"));
        p.pos.stillOpen = p.pos.known && p.pos.snapshotAt >= stateAt;
        prepared.push_back(std::move(p));
    }

    const long long accountValue = lastKnownAccountValue(wallet);

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
            if (bulk) sqlite3_exec(g_hlDb, "COMMIT", nullptr, nullptr, nullptr);
        }
    }

    if (wasSeeded && !freshRows.empty()) {
        std::map<std::string, HlAlertData> series;
        for (size_t i : freshRows) {
            const Prepared& p = prepared[i];
            const std::string coin = jstr(*p.fill, "coin");
            const std::string dk = dirKey(jstr(*p.fill, "dir"));

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

        for (auto& kv : series) {
            HlAlertData& a = kv.second;
            if (a.qtyNanos > 0) {
                const __int128 num = static_cast<__int128>(a.notionalNanos) * NANOS_PER_UNIT;
                a.avgPxNanos = static_cast<long long>(num / a.qtyNanos);
            }
            dispatchHlAlert(wallet, a);
        }
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

    if (fills.size() >= static_cast<size_t>(HL_HYPERACTIVE_FILLS)) {
        if (st.debounce < HL_HYPERACTIVE_DEBOUNCE_SEC) {
            st.debounce = HL_HYPERACTIVE_DEBOUNCE_SEC;
            std::cout << "[HL] " << wallet << " торгует слишком часто, опрос раз в "
                      << HL_HYPERACTIVE_DEBOUNCE_SEC << "с" << std::endl;
        }
    }
    if (maxTs < startMs) maxTs = startMs - 1;
    st.seeded = true;
    st.lastFillTs = maxTs;
    st.lastEnriched = now;
    saveWalletState(wallet, st);
    setNextEnrich(wallet, now + st.debounce);

    const int total = countFillsInWindow(wallet);
    if (total > HL_MAX_BOT_TRADES && banWallet(wallet, total)) {
        g_botsBanned.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[HL] бот на фьючерсах: " << wallet << " - " << total
                  << " ордеров за 30 дней, исключён из перп-рейтинга" << std::endl;
    }
}

void enricherLoop() {
    long long lastCleanup = 0;
    while (keepGoing()) {
        std::set<std::string> batch;
        {
            std::lock_guard<std::mutex> l(g_queueMutex);
            batch.swap(g_enrichQueue);
        }
        for (const std::string& w : batch) {
            if (!keepGoing()) break;
            if (!readyToEnrich(w)) { queueWallet(w); continue; }
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
        if (perpCoinsLoaded() &&
            (lastCleanup == 0 || nowSec() - lastCleanup >= HL_CLEANUP_INTERVAL_SEC)) {
            purgeNonPerpFills();
            cleanupOldFills();
            lastCleanup = nowSec();
        }

        for (int i = 0; i < 20 && keepGoing(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
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

            queueWallet(addr);
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

constexpr int HL_MIN_CLOSED_TRADES = 5;
constexpr int HL_PER_PAGE = 5;
constexpr long long HL_RANK_CACHE_SEC = 300;

struct PerpRow {
    std::string wallet;
    long long pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int closedTrades = 0;
    double avgLeverage = 0.0;
    bool roiKnown = false;
    long long volumeNanos = 0;
    long long lastTs = 0;
};

std::vector<PerpRow> g_rankCache;

std::vector<PerpRow> computeRanking() {
    std::vector<PerpRow> rows;
    const long long sinceMs = (nowSec() - HL_RANK_WINDOW_SEC) * 1000LL;

    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return rows;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT wallet,"
            " SUM(closed_pnl_nanos),"
            " SUM(CASE WHEN closed_pnl_nanos != 0 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN closed_pnl_nanos > 0 THEN 1 ELSE 0 END),"
            " SUM(notional_nanos),"
            " AVG(CASE WHEN leverage > 0 THEN leverage END),"
            " AVG(CASE WHEN account_value_nanos > 0 THEN account_value_nanos END),"
            " MAX(ts)"
            " FROM hl_fills f WHERE f.ts >= ?"
            " AND NOT EXISTS (SELECT 1 FROM hl_banned b WHERE b.wallet = f.wallet)"
            " GROUP BY wallet"))
        return rows;
    sqlite3_bind_int64(s, 1, sinceMs);

    while (sqlite3_step(s) == SQLITE_ROW) {
        PerpRow r;
        r.wallet = safeColumnText(s, 0);
        r.pnlNanos = sqlite3_column_int64(s, 1);
        r.closedTrades = sqlite3_column_int(s, 2);
        const int wins = sqlite3_column_int(s, 3);
        r.volumeNanos = sqlite3_column_int64(s, 4);
        r.avgLeverage = sqlite3_column_double(s, 5);
        const double avgAccount = sqlite3_column_double(s, 6);
        r.lastTs = sqlite3_column_int64(s, 7);

        if (r.closedTrades < HL_MIN_CLOSED_TRADES) continue;
        r.winRatePercent = static_cast<int>((100LL * wins) / r.closedTrades);
        if (avgAccount > 0.0) {
            r.roiPercent = 100.0 * static_cast<double>(r.pnlNanos) / avgAccount;
            r.roiKnown = true;
        }
        rows.push_back(r);
    }
    sqlite3_finalize(s);
    return rows;
}

void rankingSnapshot(std::vector<PerpRow>& localCopy) {
    bool needRebuild;
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        needRebuild = g_rankCache.empty() || nowSec() - g_rankBuiltAt >= HL_RANK_CACHE_SEC;
        if (needRebuild) g_rankBuiltAt = nowSec();
    }

    if (needRebuild) {
        std::vector<PerpRow> fresh = computeRanking();
        std::lock_guard<std::mutex> l(g_rankMutex);
        g_rankCache.swap(fresh);
        localCopy = g_rankCache;
        return;
    }
    std::lock_guard<std::mutex> l(g_rankMutex);
    localCopy = g_rankCache;
}

enum class PerpKind { PNL, ROI, WINRATE, ACTIVE };

bool parsePerpKind(const std::string& v, PerpKind& out) {
    if (v == "pnl")     { out = PerpKind::PNL;     return true; }
    if (v == "roi")     { out = PerpKind::ROI;     return true; }
    if (v == "winrate") { out = PerpKind::WINRATE; return true; }
    if (v == "active")  { out = PerpKind::ACTIVE;  return true; }
    return false;
}

std::string perpKindStr(PerpKind k) {
    switch (k) {
        case PerpKind::ROI:     return "roi";
        case PerpKind::WINRATE: return "winrate";
        case PerpKind::ACTIVE:  return "active";
        default:                return "pnl";
    }
}

std::string perpTitle(PerpKind k, Lang lang) {
    switch (k) {
        case PerpKind::ROI:     return "\U0001F4C8 " + tr(lang, "rk_top_roi_30d");
        case PerpKind::WINRATE: return "\U0001F3AF " + tr(lang, "rk_top_winrate_30d");
        case PerpKind::ACTIVE:  return "\U0001F504 " + tr(lang, "rk_most_active_30d");
        default:                return "\U0001F4B5 " + tr(lang, "rk_top_pnl_30d");
    }
}

void sortByKind(std::vector<PerpRow>& rows, PerpKind kind) {
    switch (kind) {
        case PerpKind::ROI:
            std::sort(rows.begin(), rows.end(), [](const PerpRow& a, const PerpRow& b) {
                if (a.roiKnown != b.roiKnown) return a.roiKnown;
                return a.roiPercent > b.roiPercent;
            });
            break;
        case PerpKind::WINRATE:
            std::sort(rows.begin(), rows.end(), [](const PerpRow& a, const PerpRow& b) {
                if (a.winRatePercent != b.winRatePercent) return a.winRatePercent > b.winRatePercent;
                return a.closedTrades > b.closedTrades;
            });
            break;
        case PerpKind::ACTIVE:
            std::sort(rows.begin(), rows.end(),
                      [](const PerpRow& a, const PerpRow& b) { return a.closedTrades > b.closedTrades; });
            break;
        default:
            std::sort(rows.begin(), rows.end(),
                      [](const PerpRow& a, const PerpRow& b) { return a.pnlNanos > b.pnlNanos; });
    }
}

std::string rankLabel(int rank) {
    switch (rank) {
        case 1: return "\U0001F947 #1";
        case 2: return "\U0001F948 #2";
        case 3: return "\U0001F949 #3";
        default: return "#" + std::to_string(rank);
    }
}

HlMessage renderPerpPage(const std::string& chatId, PerpKind kind, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    int maxRank = premiumTopTradersLimit(chatId);
    if (maxRank < 1) maxRank = 1;
    const bool showUpgrade = !isPremium(chatId);

    std::vector<PerpRow> rows;
    rankingSnapshot(rows);
    sortByKind(rows, kind);

    const int visible = std::min(static_cast<int>(rows.size()), maxRank);
    const int totalPages = std::max(1, (visible + HL_PER_PAGE - 1) / HL_PER_PAGE);
    page = std::max(1, std::min(page, totalPages));
    const int startIdx = (page - 1) * HL_PER_PAGE;
    const int endIdx = std::min(visible, startIdx + HL_PER_PAGE);

    const char* const dm = dirMark(lang);

    std::stringstream text;
    text << dm << "\U0001F535 <b>Hyperliquid \u2014 " << perpTitle(kind, lang) << "</b>\n\n";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    if (visible == 0) {
        text << dm << "\U0001F4CA " << tr(lang, "hl_rk_empty");
    } else {
        for (int i = startIdx; i < endIdx; i++) {
            const PerpRow& r = rows[static_cast<size_t>(i)];
            const int rank = i + 1;
            text << dm << rankLabel(rank) << "\n";
            text << dm << "<code>" << safeString(r.wallet, 42) << "</code>\n\n";
            text << dm << "\U0001F4B5 <b>PnL:</b> " << formatUsdNanosSigned(r.pnlNanos, true) << "\n";
            if (r.roiKnown)
                text << dm << "\U0001F4C8 <b>" << tr(lang, "hl_rk_roi_account") << ":</b> " << formatPercent(r.roiPercent, true) << "\n";
            text << dm << "\U0001F3AF <b>" << tr(lang, "ws_winrate") << ":</b> " << r.winRatePercent << "%\n";
            text << dm << "\U0001F504 <b>" << tr(lang, "rk_trades") << ":</b> " << r.closedTrades << "\n";
            if (r.avgLeverage > 0.0) {
                text << dm << "\u2699\uFE0F <b>" << tr(lang, "hl_rk_leverage") << ":</b> "
                     << static_cast<int>(r.avgLeverage + 0.5) << "\u00D7\n";
            }
            if (i + 1 < endIdx) text << "\n" << dm << HL_CARD_SEPARATOR << "\n\n";

            keyboard["inline_keyboard"].push_back(json::array({
                {{"text", tr(lang, "rk_track") + " #" + std::to_string(rank)},
                 {"callback_data", "tt_track:" + r.wallet}}
            }));
        }
    }

    if (showUpgrade && visible > 0) {
        text << "\n" << dm << HL_CARD_SEPARATOR << "\n";
        text << dm << tr(lang, "rk_unlock_top100");
        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
        }));
    }

    const std::string kindParam = perpKindStr(kind);
    json navRow = json::array();
    if (page > 1)
        navRow.push_back({{"text", "\u2B05\uFE0F"}, {"callback_data", "hl_page:" + kindParam + ":" + std::to_string(page - 1)}});
    navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "tt_noop"}});
    if (page < totalPages)
        navRow.push_back({{"text", "\u27A1\uFE0F"}, {"callback_data", "hl_page:" + kindParam + ":" + std::to_string(page + 1)}});
    keyboard["inline_keyboard"].push_back(navRow);
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    return {text.str(), keyboard.dump()};
}

}

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
        "  hash TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS idx_hl_fills_wallet_ts ON hl_fills(wallet, ts);"
        "CREATE INDEX IF NOT EXISTS idx_hl_fills_ts ON hl_fills(ts);"
        "CREATE TABLE IF NOT EXISTS hl_wallet_state ("
        "  wallet TEXT PRIMARY KEY,"
        "  seeded INTEGER NOT NULL DEFAULT 0,"
        "  last_fill_ts INTEGER NOT NULL DEFAULT 0,"
        "  last_enriched INTEGER NOT NULL DEFAULT 0,"
        "  debounce_sec INTEGER NOT NULL DEFAULT 30);"
        "CREATE TABLE IF NOT EXISTS hl_banned ("
        "  wallet TEXT PRIMARY KEY,"
        "  banned_at INTEGER NOT NULL DEFAULT 0,"
        "  trades INTEGER NOT NULL DEFAULT 0);";

    char* err = nullptr;
    if (sqlite3_exec(g_hlDb, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[HL] схема не создана: " << (err ? err : "?") << std::endl;
        if (err) sqlite3_free(err);
        sqlite3_close(g_hlDb);
        g_hlDb = nullptr;
        return false;
    }

    const char* const migrations[] = {
        "ALTER TABLE hl_fills ADD COLUMN notional_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN margin_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN leverage INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN oid INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN account_value_nanos INTEGER NOT NULL DEFAULT 0",
    };
    for (const char* sql : migrations) {
        char* mErr = nullptr;
        if (sqlite3_exec(g_hlDb, sql, nullptr, nullptr, &mErr) == SQLITE_OK)
            std::cout << "[HL] база обновлена: " << sql << std::endl;
        if (mErr) sqlite3_free(mErr);
    }

    sqlite3_stmt* probe = nullptr;
    if (sqlite3_prepare_v2(g_hlDb,
            "SELECT tid,notional_nanos,margin_nanos,leverage,oid,account_value_nanos"
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
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (g_hlDb) {
        sqlite3_close(g_hlDb);
        g_hlDb = nullptr;
    }
}

std::string hyperliquidStatsLine() {
    std::stringstream ss;
    const long long last = g_lastMsgTs.load(std::memory_order_relaxed);
    size_t queued;
    { std::lock_guard<std::mutex> l(g_queueMutex); queued = g_enrichQueue.size(); }

    ss << "\n\n\U0001F535 <b>Hyperliquid</b>\n"
       << (g_connected.load(std::memory_order_relaxed) ? "\u2705 на связи" : "\u274C нет связи")
       << ", рынков: " << g_subscribedCoins.load(std::memory_order_relaxed)
       << "\n\U0001F440 кошельков: " << watchedCount()
       << "\n\U0001F4CA сделок в фиде: " << formatThousands(g_tradesSeen.load(std::memory_order_relaxed))
       << "\n\U0001F3AF попаданий: " << g_hits.load(std::memory_order_relaxed)
       << "\n\U0001F504 дозагрузок: " << g_enriched.load(std::memory_order_relaxed)
       << " (в очереди " << queued << ")"
       << "\n\U0001F4E8 алертов: " << g_alertsSent.load(std::memory_order_relaxed)
       << "\n\U0001F6D1 пропусков по бюджету: " << g_budgetSkips.load(std::memory_order_relaxed)
       << "\n\U0001F916 ботов исключено: " << g_botsBanned.load(std::memory_order_relaxed)
       << "\n\u267B\uFE0F переподключений: " << g_reconnects.load(std::memory_order_relaxed);
    if (last > 0) ss << "\n\u23F1 тишина: " << (nowSec() - last) << "с";
    return ss.str();
}

HlMessage buildVenueMenu(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "hl_venue_spot")}, {"callback_data", "menu:toptrader_spot"}}
    }));
    const std::string perpLabel = tr(lang, "hl_venue_perp")
                                + (isPremium(chatId) ? "" : "  \U0001F512");
    kb["inline_keyboard"].push_back(json::array({
        {{"text", perpLabel}, {"callback_data", "hl_menu"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));
    return {"\U0001F3C6 <b>" + tr(lang, "hl_venue_title") + "</b>\n\n"
            + tr(lang, "hl_venue_choose"), kb.dump()};
}

HlMessage buildPerpTopMenu(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    json kb;
    kb["inline_keyboard"] = json::array();
    const char* kinds[4] = {"pnl", "roi", "winrate", "active"};
    const char* keys[4]  = {"rk_btn_top_pnl", "rk_btn_top_roi",
                            "rk_btn_top_winrate", "rk_btn_most_active"};
    for (int i = 0; i < 4; i++) {
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, keys[i])}, {"callback_data", std::string("hl_open:") + kinds[i]}}
        }));
    }
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));
    return {"\U0001F535 <b>Hyperliquid \u2014 " + tr(lang, "rk_top_traders_30d") + "</b>\n\n"
            + tr(lang, "hl_rk_choose"), kb.dump()};
}

HlMessage buildPerpLocked(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    std::stringstream t;
    t << "\U0001F535 <b>Hyperliquid \u2014 " << tr(lang, "rk_top_traders_30d") << "</b>\n";
    t << HL_CARD_SEPARATOR << "\n\n";
    t << "\U0001F512 " << tr(lang, "hl_locked_body") << "\n";

    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));
    return {t.str(), kb.dump()};
}

struct OpenPosition {
    std::string wallet;
    std::string label;
    std::string coin;
    bool isLong = false;
    long long sizeNanos = 0;
    long long entryPxNanos = 0;
    long long liqPxNanos = 0;
    long long marginNanos = 0;
    long long unrealizedNanos = 0;
    double roePercent = 0.0;
    int leverage = 0;
    bool isolated = false;
};

bool fetchOpenPositions(const std::string& wallet, std::vector<OpenPosition>& out) {
    json body;
    body["type"] = "clearinghouseState";
    body["user"] = wallet;
    body["dex"] = "";
    json j = infoPost(body, HL_WEIGHT_CLEARINGHOUSE);
    if (!j.is_object() || !j.contains("assetPositions") || !j["assetPositions"].is_array())
        return false;

    for (const auto& ap : j["assetPositions"]) {
        if (!ap.is_object() || !ap.contains("position") || !ap["position"].is_object()) continue;
        const json& p = ap["position"];

        OpenPosition op;
        op.wallet = wallet;
        op.coin = jstr(p, "coin");
        if (op.coin.empty()) continue;

        // szi отрицателен у шорта - знак и есть сторона позиции.
        long long szi = 0;
        parseDecimalToNanos(jstr(p, "szi", "0"), szi);
        if (szi == 0) continue;
        op.isLong = szi > 0;
        op.sizeNanos = szi < 0 ? -szi : szi;

        parseDecimalToNanos(jstr(p, "entryPx", "0"), op.entryPxNanos);
        parseDecimalToNanos(jstr(p, "liquidationPx", "0"), op.liqPxNanos);
        parseDecimalToNanos(jstr(p, "marginUsed", "0"), op.marginNanos);
        parseDecimalToNanos(jstr(p, "unrealizedPnl", "0"), op.unrealizedNanos);

        // returnOnEquity приходит долей, а не процентом.
        long long roe = 0;
        if (parseDecimalToNanos(jstr(p, "returnOnEquity", "0"), roe))
            op.roePercent = 100.0 * static_cast<double>(roe) / static_cast<double>(NANOS_PER_UNIT);

        if (p.contains("leverage") && p["leverage"].is_object()) {
            const json& lev = p["leverage"];
            if (lev.contains("value") && lev["value"].is_number())
                op.leverage = lev["value"].get<int>();
            op.isolated = jstr(lev, "type") == "isolated";
        }
        out.push_back(std::move(op));
    }
    return true;
}

HlMessage buildPositionsLocked(Lang lang) {
    std::stringstream t;
    const char* const dm = dirMark(lang);
    t << dm << "\U0001F4C8 <b>" << tr(lang, "hl_open_positions") << "</b>\n"
      << HL_CARD_SEPARATOR << "\n\n"
      << dm << "\U0001F512 " << tr(lang, "hl_locked_body");
    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));
    return {t.str(), kb.dump()};
}

// Выбор кошелька. Опрашивать все сразу незачем: человека интересует конкретный,
// а каждый лишний запрос - это ожидание на его же экране.
HlMessage buildPositionsPicker(const std::string& chatId, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    if (!isPremium(chatId)) return buildPositionsLocked(lang);

    const char* const dm = dirMark(lang);
    const std::vector<HlUserWallet> wallets = hlUserWallets(chatId);

    // Постраничность обязательна: у премиума до 50 кошельков, а у сервисного
    // аккаунта - тысячи. Без неё экран превращался в простыню из кнопок.
    const int total = static_cast<int>(wallets.size());
    const int pages = total > 0 ? (total + HL_PICKER_PER_PAGE - 1) / HL_PICKER_PER_PAGE : 1;
    if (page < 1) page = 1;
    if (page > pages) page = pages;
    const int from = (page - 1) * HL_PICKER_PER_PAGE;
    const int to = std::min(from + HL_PICKER_PER_PAGE, total);

    json kb;
    kb["inline_keyboard"] = json::array();
    for (int i = from; i < to; i++) {
        const HlUserWallet& w = wallets[static_cast<size_t>(i)];
        const std::string label = w.label.empty() ? shortAddress(w.address) : safeString(w.label, 32);
        kb["inline_keyboard"].push_back(json::array({
            {{"text", "\U0001F4BC " + label}, {"callback_data", "hl_pos:" + w.address}}
        }));
    }

    if (pages > 1) {
        json nav = json::array();
        if (page > 1)
            nav.push_back({{"text", "\u2B05\uFE0F"},
                           {"callback_data", "hl_pospage:" + std::to_string(page - 1)}});
        nav.push_back({{"text", std::to_string(page) + "/" + std::to_string(pages)},
                       {"callback_data", "hl_posnoop"}});
        if (page < pages)
            nav.push_back({{"text", "\u27A1\uFE0F"},
                           {"callback_data", "hl_pospage:" + std::to_string(page + 1)}});
        kb["inline_keyboard"].push_back(nav);
    }

    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    std::stringstream t;
    t << dm << "\U0001F4C8 <b>" << tr(lang, "hl_open_positions") << "</b>\n"
      << HL_CARD_SEPARATOR << "\n\n";
    t << dm << (wallets.empty() ? tr(lang, "mw_no_wallets") : tr(lang, "hl_positions_choose"));
    return {t.str(), kb.dump()};
}

HlMessage buildWalletPositions(const std::string& chatId, const std::string& address) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    if (!isPremium(chatId)) return buildPositionsLocked(lang);

    const char* const dm = dirMark(lang);
    const std::string addr = toLower(address);

    json kb;
    kb["inline_keyboard"] = json::array();
    // Именно "back", а не "hl_positions": второе - это НОВЫЙ переход, он кладёт
    // экран в историю поверх текущего, и стек начинает ходить по кругу
    // positions -> pos -> positions. Шаг назад по истории приведёт туда же,
    // но не сломает навигацию.
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    // Метку берём из списка пользователя: показывать голый адрес там, где у
    // человека есть своё имя для кошелька, значит терять узнаваемость.
    std::string label = shortAddress(addr);
    for (const auto& w : hlUserWallets(chatId))
        if (w.address == addr && !w.label.empty()) { label = safeString(w.label, 32); break; }

    std::stringstream t;
    t << dm << "\U0001F4BC <b>" << label << "</b>\n";
    t << dm << "<code>" << safeString(addr, 42) << "</code>\n";
    t << HL_CARD_SEPARATOR << "\n\n";

    std::vector<OpenPosition> pos;
    if (!fetchOpenPositions(addr, pos)) {
        t << dm << tr(lang, "generic_error_retry");
        return {t.str(), kb.dump()};
    }
    if (pos.empty()) {
        t << dm << tr(lang, "hl_no_open_positions");
        return {t.str(), kb.dump()};
    }

    // Крупные позиции выше: если их несколько, сверху должно быть главное.
    std::sort(pos.begin(), pos.end(), [](const OpenPosition& a, const OpenPosition& b) {
        return a.marginNanos > b.marginNanos;
    });

    for (size_t i = 0; i < pos.size(); i++) {
        const OpenPosition& p = pos[i];
        if (i) t << "\n" << dm << HL_CARD_SEPARATOR << "\n\n";

        t << dm << (p.isLong ? "\U0001F7E2" : "\U0001F534") << " <b>"
          << safeString(p.coin, 32) << "</b>\n";
        t << dm << tr(lang, p.isLong ? "hl_side_long" : "hl_side_short") << ": <b>"
          << fmtUsd(p.marginNanos * (p.leverage > 0 ? p.leverage : 1)) << "</b>";
        if (p.leverage > 0)
            t << " \u00B7 " << p.leverage << "\u00D7 "
              << tr(lang, p.isolated ? "hl_isolated" : "hl_cross");
        t << "\n";
        if (p.marginNanos > 0)
            t << dm << "\U0001F4B5 " << tr(lang, "hl_collateral") << ": <b>"
              << fmtUsd(p.marginNanos) << "</b>\n";
        if (p.entryPxNanos > 0)
            t << dm << "\U0001F4CD " << tr(lang, "hl_entry_price") << ": <b>"
              << formatPriceNanos(p.entryPxNanos) << "</b>\n";
        t << dm << (p.unrealizedNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ")
          << tr(lang, "hl_unrealized") << ": <b>"
          << formatUsdNanosSigned(p.unrealizedNanos, true) << "</b>";
        if (p.roePercent != 0.0) t << " (" << formatPercent(p.roePercent, true) << ")";
        t << "\n";
        if (p.liqPxNanos > 0)
            t << dm << "\u2620\uFE0F " << tr(lang, "hl_liq") << ": <b>"
              << formatPriceNanos(p.liqPxNanos) << "</b>\n";
    }
    return {t.str(), kb.dump()};
}

bool renderHyperliquidView(const std::string& chatId, const std::string& action,
                           const std::string& param, HlMessage& out) {
    if ((action == "hl_menu" || action == "hl_open" || action == "hl_page") &&
        !isPremium(chatId)) {
        out = buildPerpLocked(chatId);
        return true;
    }
    if (action == "hl_positions") { out = buildPositionsPicker(chatId, 1); return true; }
    if (action == "hl_pospage") {
        int page = 1;
        try { page = std::stoi(param); } catch (...) {}
        out = buildPositionsPicker(chatId, page);
        return true;
    }
    if (action == "hl_posnoop") { out = buildPositionsPicker(chatId, 1); return true; }
    if (action == "hl_pos") { out = buildWalletPositions(chatId, param); return true; }
    if (action == "hl_menu") { out = buildPerpTopMenu(chatId); return true; }
    if (action == "hl_open") {
        PerpKind kind;
        if (!parsePerpKind(param, kind)) return false;
        out = renderPerpPage(chatId, kind, 1);
        return true;
    }
    if (action == "hl_page") {
        const size_t sep = param.find(':');
        if (sep == std::string::npos) return false;
        PerpKind kind;
        if (!parsePerpKind(param.substr(0, sep), kind)) return false;
        int page = 1;
        try { page = std::stoi(param.substr(sep + 1)); } catch (...) {}
        out = renderPerpPage(chatId, kind, page);
        return true;
    }
    return false;
}

bool handleHyperliquidCallback(const std::string& chatId, const std::string& action,
                               const std::string& param, const std::string& data,
                               long long messageId, const std::string& callbackQueryId) {
    HlMessage msg;
    // Ответ площадки разбирается прямо здесь, в потоке телеграма. Любое
    // неожиданное поле - и исключение уронило бы весь процесс, а не один экран.
    // Ловим его: человек увидит ошибку и сможет нажать ещё раз.
    try {
        if (!renderHyperliquidView(chatId, action, param, msg)) return false;
    } catch (const std::exception& e) {
        std::cerr << "[HL] ошибка при построении экрана " << action << ": " << e.what() << std::endl;
        const Lang lang = langFromCode(getUserLanguage(chatId));
        msg.text = tr(lang, "generic_error_retry");
        json kb;
        kb["inline_keyboard"] = json::array();
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
        }));
        msg.keyboard = kb.dump();
    }
    rememberView(chatId, data);
    replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    return true;
}
