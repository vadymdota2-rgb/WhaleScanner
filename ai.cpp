#include "ai.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>
#include "json.hpp"
#include "alert_settings.h"
#include "hyperliquid_internal.h"
#include "ru.h"
#include "token_prices.h"
#include "utils.h"

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;
extern const std::string OWNER_CHAT_ID;

std::string getUserLanguage(const std::string& chatId);
void rememberView(const std::string& chatId, const std::string& data);

namespace {

constexpr long long AI_CACHE_TTL_SEC[] = { 120, 600, 1800 };
int cacheSlot(int days) { return days == 30 ? 2 : (days == 7 ? 1 : 0); }

struct AiCacheEntry {
    time_t at = 0;
    std::string text;
};
std::mutex g_aiCacheMutex;
std::map<int, AiCacheEntry> g_aiCache;

constexpr int AI_MIN_WALLETS = 3;
constexpr int AI_TOP_N = 5;
constexpr double AI_MAX_ONE_SHARE = 0.70;
constexpr int AI_NF = 9;
constexpr int AI_MIN_TRAIN = 80;
constexpr long long AI_HORIZON_6H = 6LL * 3600;
constexpr long long AI_HORIZON_24H = 86400;
constexpr long long AI_EVENT_TTL_SEC = 90LL * 86400LL;

int clampDays(int days) {
    if (days == 7 || days == 30) return days;
    return 1;
}

std::string compactUsd(long long nanos) {
    const long long a = nanos < 0 ? -nanos : nanos;
    const double usd = static_cast<double>(a) / 1000000000.0;
    char buf[32];
    if (usd >= 1000000000.0) std::snprintf(buf, sizeof(buf), "$%.1fB", usd / 1000000000.0);
    else if (usd >= 1000000.0) std::snprintf(buf, sizeof(buf), "$%.1fM", usd / 1000000.0);
    else if (usd >= 10000.0) std::snprintf(buf, sizeof(buf), "$%.0fK", usd / 1000.0);
    else if (usd >= 1000.0) std::snprintf(buf, sizeof(buf), "$%.1fK", usd / 1000.0);
    else std::snprintf(buf, sizeof(buf), "$%.0f", usd);
    if (nanos < 0) return std::string("-") + buf;
    return buf;
}

std::string signedCompact(long long nanos) {
    if (nanos > 0) return "+" + compactUsd(nanos);
    return compactUsd(nanos);
}

struct Row {
    std::string id;
    std::string name;
    long long buy = 0;
    long long sell = 0;
    int nBuy = 0;
    int nSell = 0;
    int wallets = 0;
    double oneShare = 0;
    double score = 0;
    bool perp = false;
    long long net6h = 0;
    long long netPrior = 0;
    long long fundingNanos = 0;
    double liqUsd = 0;
};

std::mutex g_wMutex;
std::array<double, AI_NF> g_wSpot{};
std::array<double, AI_NF> g_wPerp{};
bool g_trainedSpot = false;
bool g_trainedPerp = false;
long long g_lastTrain = 0;
bool g_schemaOk = false;

double sigmoid(double z) {
    if (z > 20) return 1;
    if (z < -20) return 0;
    return 1.0 / (1.0 + std::exp(-z));
}

double accelOf(long long net6, long long netPrior) {
    const double a = static_cast<double>(net6);
    const double b = static_cast<double>(netPrior) / 3.0;
    const double d = std::fabs(a) + std::fabs(b);
    if (d < 1) return 0;
    double v = (a - b) / d;
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    return v;
}

void featuresOf(const Row& r, std::array<double, AI_NF>& f) {
    const long long vol = r.buy + r.sell;
    const double dir = vol > 0 ? static_cast<double>(r.buy - r.sell) / static_cast<double>(vol) : 0;
    const double usd = static_cast<double>(vol) / 1000000000.0;
    const double scaled = r.perp ? usd / 10.0 : usd;
    const int n = r.nBuy + r.nSell;
    f[0] = 1;
    f[1] = dir;
    f[2] = std::log1p(static_cast<double>(r.wallets)) / std::log1p(50.0);
    f[3] = 1.0 - r.oneShare;
    f[4] = std::log1p(scaled) / std::log1p(1000000.0);
    f[5] = n > 0 ? static_cast<double>(r.nBuy) / static_cast<double>(n) : 0.5;
    f[6] = accelOf(r.net6h, r.netPrior);
    f[7] = std::tanh(static_cast<double>(r.fundingNanos) / 10000000.0);
    f[8] = r.perp ? 0.5 : std::min(1.0, std::log1p(std::max(0.0, r.liqUsd)) / std::log1p(5000000.0));
}

double heuristicScore(const Row& r) {
    const long long vol = r.buy + r.sell;
    if (vol <= 0 || r.wallets < AI_MIN_WALLETS) return 0;
    if (r.oneShare > AI_MAX_ONE_SHARE) return 0;
    const long long net = r.buy - r.sell;
    const double dir = static_cast<double>(net) / static_cast<double>(vol);
    const double breadth = std::log1p(static_cast<double>(r.wallets));
    const double usd = static_cast<double>(vol) / 1000000000.0;
    const double scaled = r.perp ? usd / 10.0 : usd;
    return dir * breadth * (1.0 - r.oneShare) * std::log1p(scaled);
}

double rowScore(const Row& r) {
    const double h = heuristicScore(r);
    if (h == 0) return 0;
    std::array<double, AI_NF> w{};
    bool trained = false;
    {
        std::lock_guard<std::mutex> l(g_wMutex);
        if (r.perp) { trained = g_trainedPerp; if (trained) w = g_wPerp; }
        else { trained = g_trainedSpot; if (trained) w = g_wSpot; }
    }
    if (!trained) return h;
    std::array<double, AI_NF> f{};
    featuresOf(r, f);
    double z = 0;
    for (int i = 0; i < AI_NF; i++) z += w[static_cast<size_t>(i)] * f[static_cast<size_t>(i)];
    const double pUp = sigmoid(z);
    const double conf = h > 0 ? pUp : (1.0 - pUp);
    return (h > 0 ? 1.0 : -1.0) * conf * std::fabs(h);
}

void finish(Row& r) { r.score = rowScore(r); }

int wKey(bool perp, int i) { return perp ? 200 + i : i; }
int nKey(bool perp) { return perp ? 300 : 100; }
int tKey(bool perp) { return perp ? 301 : 101; }

void loadWeightSet(bool perp) {
    sqlite3_stmt* s = nullptr;
    std::array<double, AI_NF> w{};
    int got = 0;
    if (prepareOrLog(db, &s, "SELECT k,v FROM ai_weights WHERE k>=? AND k<?")) {
        sqlite3_bind_int(s, 1, wKey(perp, 0));
        sqlite3_bind_int(s, 2, wKey(perp, AI_NF));
        while (sqlite3_step(s) == SQLITE_ROW) {
            const int k = sqlite3_column_int(s, 0) - wKey(perp, 0);
            if (k >= 0 && k < AI_NF) {
                w[static_cast<size_t>(k)] = sqlite3_column_double(s, 1);
                got++;
            }
        }
        sqlite3_finalize(s);
    }
    long long nSamp = 0;
    if (prepareOrLog(db, &s, "SELECT v FROM ai_weights WHERE k=?")) {
        sqlite3_bind_int(s, 1, nKey(perp));
        if (sqlite3_step(s) == SQLITE_ROW) nSamp = static_cast<long long>(sqlite3_column_double(s, 0));
        sqlite3_finalize(s);
    }
    if (got == AI_NF && nSamp >= AI_MIN_TRAIN) {
        std::lock_guard<std::mutex> l(g_wMutex);
        if (perp) { g_wPerp = w; g_trainedPerp = true; }
        else { g_wSpot = w; g_trainedSpot = true; }
    }
}

void ensureSchema() {
    if (g_schemaOk) return;
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return;
    const char* sql =
        "CREATE TABLE IF NOT EXISTS ai_events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts INTEGER NOT NULL,"
        "  hour_slot INTEGER NOT NULL,"
        "  window_days INTEGER NOT NULL,"
        "  venue INTEGER NOT NULL,"
        "  token TEXT NOT NULL,"
        "  name TEXT,"
        "  buy_nanos INTEGER NOT NULL,"
        "  sell_nanos INTEGER NOT NULL,"
        "  n_buy INTEGER NOT NULL,"
        "  n_sell INTEGER NOT NULL,"
        "  wallets INTEGER NOT NULL,"
        "  one_share_bp INTEGER NOT NULL,"
        "  score REAL NOT NULL,"
        "  price_then INTEGER NOT NULL DEFAULT 0,"
        "  price_6h INTEGER NOT NULL DEFAULT 0,"
        "  price_24h INTEGER NOT NULL DEFAULT 0,"
        "  filled_6h INTEGER NOT NULL DEFAULT 0,"
        "  filled_at INTEGER NOT NULL DEFAULT 0,"
        "  net_6h INTEGER NOT NULL DEFAULT 0,"
        "  net_prior INTEGER NOT NULL DEFAULT 0,"
        "  funding_nanos INTEGER NOT NULL DEFAULT 0,"
        "  liq_nanos INTEGER NOT NULL DEFAULT 0,"
        "  UNIQUE(token, venue, window_days, hour_slot)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_ai_events_fill ON ai_events(filled_at, ts);"
        "CREATE TABLE IF NOT EXISTS ai_weights ("
        "  k INTEGER PRIMARY KEY,"
        "  v REAL NOT NULL"
        ");";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return;
    }
    const char* alts[] = {
        "ALTER TABLE ai_events ADD COLUMN price_6h INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN filled_6h INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN net_6h INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN net_prior INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN funding_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN liq_nanos INTEGER NOT NULL DEFAULT 0",
        nullptr
    };
    for (int i = 0; alts[i]; i++) {
        char* aerr = nullptr;
        sqlite3_exec(db, alts[i], nullptr, nullptr, &aerr);
        if (aerr) sqlite3_free(aerr);
    }
    loadWeightSet(false);
    loadWeightSet(true);
    g_schemaOk = true;
}

long long perpMark(const std::string& coin, long long beforeTs) {
    std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return 0;
    sqlite3_stmt* s = nullptr;
    long long mark = 0;
    if (beforeTs > 0) {
        if (!prepareOrLog(hl::g_hlDb, &s,
                "SELECT mark_nanos FROM hl_funding_rate WHERE coin=? AND hour_ts<=? "
                "ORDER BY hour_ts DESC LIMIT 1"))
            return 0;
        sqlite3_bind_text(s, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, beforeTs);
    } else {
        if (!prepareOrLog(hl::g_hlDb, &s,
                "SELECT mark_nanos FROM hl_funding_rate WHERE coin=? "
                "ORDER BY hour_ts DESC LIMIT 1"))
            return 0;
        sqlite3_bind_text(s, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(s) == SQLITE_ROW) mark = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    if (mark > 0) return mark;
    if (!prepareOrLog(hl::g_hlDb, &s,
            "SELECT px FROM hl_fills WHERE coin=? AND ts<=? AND px!='' "
            "ORDER BY ts DESC LIMIT 1"))
        return 0;
    sqlite3_bind_text(s, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, beforeTs > 0 ? beforeTs * 1000 : hl::nowSec() * 1000);
    if (sqlite3_step(s) == SQLITE_ROW) {
        long long px = 0;
        if (hl::parseDecimalToNanos(safeColumnText(s, 0), px) && px > 0) mark = px;
    }
    sqlite3_finalize(s);
    return mark;
}

long long perpFunding(const std::string& coin) {
    std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return 0;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(hl::g_hlDb, &s,
            "SELECT rate_nanos FROM hl_funding_rate WHERE coin=? "
            "ORDER BY hour_ts DESC LIMIT 1"))
        return 0;
    sqlite3_bind_text(s, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
    long long v = 0;
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

long long spotThen(const std::string& token, long long ts) {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return 0;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "SELECT price_nanos FROM token_price_history WHERE address=? AND ts<=? "
            "ORDER BY ts DESC LIMIT 1"))
        return 0;
    sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, ts);
    long long p = 0;
    if (sqlite3_step(s) == SQLITE_ROW) p = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return p;
}

long long priceNowOf(bool perp, const std::string& id) {
    if (perp) return perpMark(id, 0);
    const uint64_t n = getPriceNanos(id);
    return n > 0 ? static_cast<long long>(n) : 0;
}

std::unordered_set<std::string> bannedSpot() {
    std::unordered_set<std::string> out;
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return out;
    sqlite3_stmt* s = nullptr;
    const long long now = static_cast<long long>(time(nullptr));
    if (!prepareOrLog(db, &s,
            "SELECT wallet FROM ignored_wallets WHERE permanent=1 OR ignored_until>?"))
        return out;
    sqlite3_bind_int64(s, 1, now);
    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string w = toLower(safeColumnText(s, 0));
        if (!w.empty()) out.insert(std::move(w));
    }
    sqlite3_finalize(s);
    return out;
}

std::unordered_set<std::string> bannedHl() {
    std::unordered_set<std::string> out;
    std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return out;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(hl::g_hlDb, &s, "SELECT wallet FROM hl_banned")) return out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string w = toLower(safeColumnText(s, 0));
        if (!w.empty()) out.insert(std::move(w));
    }
    sqlite3_finalize(s);
    return out;
}

std::unordered_map<std::string, std::string> tokenSymbols() {
    std::unordered_map<std::string, std::string> out;
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return out;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s, "SELECT address,symbol FROM token_cache")) return out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string addr = toLower(safeColumnText(s, 0));
        std::string sym = safeColumnText(s, 1);
        if (!addr.empty() && !sym.empty()) out[addr] = std::move(sym);
    }
    sqlite3_finalize(s);
    return out;
}

struct Bucket {
    long long buy = 0;
    long long sell = 0;
    int nBuy = 0;
    int nSell = 0;
    long long net6h = 0;
    long long netPrior = 0;
    std::map<std::string, long long> vol;
};

void addVol(Bucket& b, const std::string& wallet, long long usd, bool isBuy,
            long long ts, long long t6, long long t24) {
    if (usd < 0) usd = -usd;
    const long long signedUsd = isBuy ? usd : -usd;
    if (isBuy) { b.buy += usd; b.nBuy++; }
    else { b.sell += usd; b.nSell++; }
    b.vol[wallet] += usd;
    if (ts >= t6) b.net6h += signedUsd;
    else if (ts >= t24) b.netPrior += signedUsd;
}

Row toRow(const std::string& id, const std::string& name, const Bucket& b, bool perp) {
    Row r;
    r.id = id;
    r.name = name;
    r.buy = b.buy;
    r.sell = b.sell;
    r.nBuy = b.nBuy;
    r.nSell = b.nSell;
    r.wallets = static_cast<int>(b.vol.size());
    r.perp = perp;
    r.net6h = b.net6h;
    r.netPrior = b.netPrior;
    long long mx = 0, tot = 0;
    for (const auto& p : b.vol) {
        tot += p.second;
        if (p.second > mx) mx = p.second;
    }
    r.oneShare = tot > 0 ? static_cast<double>(mx) / static_cast<double>(tot) : 1;
    if (!perp) r.liqUsd = getPoolLiquidityUsd(id);
    finish(r);
    return r;
}

std::vector<Row> loadSpot(long long since, const std::unordered_set<std::string>& ban) {
    std::map<std::string, Bucket> m;
    const long long now = hl::nowSec();
    const long long t6 = now - AI_HORIZON_6H;
    const long long t24 = now - AI_HORIZON_24H;
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) return {};
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(db, &s,
                "SELECT token,wallet,is_buy,usd_nanos,timestamp FROM trades WHERE timestamp>=?"))
            return {};
        sqlite3_bind_int64(s, 1, since);
        while (sqlite3_step(s) == SQLITE_ROW) {
            const std::string token = toLower(safeColumnText(s, 0));
            const std::string wallet = toLower(safeColumnText(s, 1));
            if (token.empty() || wallet.empty() || ban.count(wallet)) continue;
            addVol(m[token], wallet, sqlite3_column_int64(s, 3),
                   sqlite3_column_int(s, 2) != 0, sqlite3_column_int64(s, 4), t6, t24);
        }
        sqlite3_finalize(s);
    }
    const auto names = tokenSymbols();
    std::vector<Row> out;
    out.reserve(m.size());
    for (const auto& p : m) {
        std::string name = getSymbol(p.first);
        if (name.empty() || name == "UNKNOWN") {
            auto it = names.find(p.first);
            if (it != names.end()) name = it->second;
            else continue;
        }
        name = safeString(name, 16);
        Row r = toRow(p.first, name, p.second, false);
        if (r.score != 0) out.push_back(std::move(r));
    }
    return out;
}

std::vector<Row> loadPerp(long long since, const std::unordered_set<std::string>& ban) {
    std::map<std::string, Bucket> m;
    const long long nowMs = hl::nowSec() * 1000;
    const long long t6 = nowMs - AI_HORIZON_6H * 1000;
    const long long t24 = nowMs - AI_HORIZON_24H * 1000;
    const long long sinceMs = since * 1000;
    {
        std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
        if (!hl::g_hlDb) return {};
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(hl::g_hlDb, &s,
                "SELECT coin,wallet,dir_code,notional_nanos,ts FROM hl_fills "
                "WHERE ts>=? AND dir_code IN (1,2)"))
            return {};
        sqlite3_bind_int64(s, 1, sinceMs);
        while (sqlite3_step(s) == SQLITE_ROW) {
            const std::string coin = safeColumnText(s, 0);
            const std::string wallet = toLower(safeColumnText(s, 1));
            if (coin.empty() || wallet.empty() || ban.count(wallet)) continue;
            const int dir = sqlite3_column_int(s, 2);
            addVol(m[coin], wallet, sqlite3_column_int64(s, 3),
                   dir == DIR_OPEN_LONG, sqlite3_column_int64(s, 4), t6, t24);
        }
        sqlite3_finalize(s);
    }
    std::vector<Row> out;
    out.reserve(m.size());
    for (const auto& p : m) {
        Row r = toRow(p.first, p.first, p.second, true);
        if (r.score != 0) out.push_back(std::move(r));
    }
    return out;
}

void takeSides(std::vector<Row>& rows, std::vector<Row>& buys, std::vector<Row>& avoids) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return std::fabs(a.score) > std::fabs(b.score);
    });
    for (auto& r : rows) {
        if (r.score > 0 && static_cast<int>(buys.size()) < AI_TOP_N) buys.push_back(std::move(r));
        else if (r.score < 0 && static_cast<int>(avoids.size()) < AI_TOP_N) avoids.push_back(std::move(r));
    }
}

void enrichPerpFunding(std::vector<Row>& rows) {
    for (auto& r : rows) {
        if (r.perp) r.fundingNanos = perpFunding(r.id);
    }
}

void recordRows(int days, const std::vector<Row>& rows) {
    if (rows.empty()) return;
    ensureSchema();
    const long long now = hl::nowSec();
    const long long slot = now / 3600;
    std::vector<long long> px;
    px.reserve(rows.size());
    for (const auto& r : rows) px.push_back(priceNowOf(r.perp, r.id));
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "INSERT OR IGNORE INTO ai_events("
            "ts,hour_slot,window_days,venue,token,name,"
            "buy_nanos,sell_nanos,n_buy,n_sell,wallets,one_share_bp,score,price_then,"
            "net_6h,net_prior,funding_nanos,liq_nanos)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
        return;
    for (size_t i = 0; i < rows.size(); i++) {
        const auto& r = rows[i];
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        sqlite3_bind_int64(s, 1, now);
        sqlite3_bind_int64(s, 2, slot);
        sqlite3_bind_int(s, 3, days);
        sqlite3_bind_int(s, 4, r.perp ? 1 : 0);
        sqlite3_bind_text(s, 5, r.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 6, r.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 7, r.buy);
        sqlite3_bind_int64(s, 8, r.sell);
        sqlite3_bind_int(s, 9, r.nBuy);
        sqlite3_bind_int(s, 10, r.nSell);
        sqlite3_bind_int(s, 11, r.wallets);
        sqlite3_bind_int(s, 12, static_cast<int>(r.oneShare * 10000.0 + 0.5));
        sqlite3_bind_double(s, 13, r.score);
        sqlite3_bind_int64(s, 14, px[i]);
        sqlite3_bind_int64(s, 15, r.net6h);
        sqlite3_bind_int64(s, 16, r.netPrior);
        sqlite3_bind_int64(s, 17, r.fundingNanos);
        sqlite3_bind_int64(s, 18, static_cast<long long>(r.liqUsd * 1000000000.0));
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
}

void writeCard(std::ostringstream& t, int i, const Row& r) {
    const char* medal = i == 0 ? "🥇 " : (i == 1 ? "🥈 " : (i == 2 ? "🥉 " : ""));
    const std::string num = i >= 3 ? "#" + std::to_string(i + 1) + " " : "";
    t << medal << num << "<b>" << r.name << "</b>\n";
    t << "<b>" << signedCompact(r.buy - r.sell) << "</b>\n";
    if (r.buy > 0) t << "🟢 " << compactUsd(r.buy);
    if (r.buy > 0 && r.sell > 0) t << "   ";
    if (r.sell > 0) t << "🔴 " << compactUsd(r.sell);
    t << "\n";
    if (r.nBuy > 0) t << r.nBuy << " ↗";
    if (r.nBuy > 0 && r.nSell > 0) t << "   ";
    if (r.nSell > 0) t << r.nSell << " ↘";
    t << "   ·   " << r.wallets << "\n";
}

std::string windowLabel(int days, Lang lang) {
    if (days == 7) return tr(lang, "ai_w7");
    if (days == 30) return tr(lang, "ai_w30");
    return tr(lang, "ai_w24");
}

struct PendingFill {
    long long id = 0;
    std::string token;
    int venue = 0;
    long long ts = 0;
    long long priceThen = 0;
    int filled6 = 0;
    int filled24 = 0;
};

void fillOutcomes() {
    ensureSchema();
    const long long now = hl::nowSec();
    std::vector<PendingFill> batch;
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) return;
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(db, &s,
                "SELECT id,token,venue,ts,price_then,filled_6h,filled_at FROM ai_events "
                "WHERE (filled_6h=0 AND ts<=?) OR (filled_at=0 AND ts<=?) LIMIT 80"))
            return;
        sqlite3_bind_int64(s, 1, now - AI_HORIZON_6H);
        sqlite3_bind_int64(s, 2, now - AI_HORIZON_24H);
        while (sqlite3_step(s) == SQLITE_ROW) {
            PendingFill p;
            p.id = sqlite3_column_int64(s, 0);
            p.token = safeColumnText(s, 1);
            p.venue = sqlite3_column_int(s, 2);
            p.ts = sqlite3_column_int64(s, 3);
            p.priceThen = sqlite3_column_int64(s, 4);
            p.filled6 = sqlite3_column_int(s, 5);
            p.filled24 = sqlite3_column_int(s, 6) != 0 ? 1 : 0;
            batch.push_back(std::move(p));
        }
        sqlite3_finalize(s);
    }
    for (auto& p : batch) {
        long long then = p.priceThen;
        if (then <= 0)
            then = p.venue ? perpMark(p.token, p.ts) : spotThen(p.token, p.ts);
        long long nowPx = priceNowOf(p.venue != 0, p.token);
        if (then <= 0 || nowPx <= 0) continue;
        const bool due6 = p.filled6 == 0 && p.ts <= now - AI_HORIZON_6H;
        const bool due24 = p.filled24 == 0 && p.ts <= now - AI_HORIZON_24H;
        std::lock_guard<std::mutex> lock(dbMutex);
        sqlite3_stmt* s = nullptr;
        if (due6 && due24) {
            if (!prepareOrLog(db, &s,
                    "UPDATE ai_events SET price_then=?, price_6h=?, filled_6h=?, "
                    "price_24h=?, filled_at=? WHERE id=?"))
                return;
            sqlite3_bind_int64(s, 1, then);
            sqlite3_bind_int64(s, 2, nowPx);
            sqlite3_bind_int64(s, 3, now);
            sqlite3_bind_int64(s, 4, nowPx);
            sqlite3_bind_int64(s, 5, now);
            sqlite3_bind_int64(s, 6, p.id);
        } else if (due24) {
            if (!prepareOrLog(db, &s,
                    "UPDATE ai_events SET price_then=?, price_24h=?, filled_at=? WHERE id=?"))
                return;
            sqlite3_bind_int64(s, 1, then);
            sqlite3_bind_int64(s, 2, nowPx);
            sqlite3_bind_int64(s, 3, now);
            sqlite3_bind_int64(s, 4, p.id);
        } else {
            if (!prepareOrLog(db, &s,
                    "UPDATE ai_events SET price_then=?, price_6h=?, filled_6h=? WHERE id=?"))
                return;
            sqlite3_bind_int64(s, 1, then);
            sqlite3_bind_int64(s, 2, nowPx);
            sqlite3_bind_int64(s, 3, now);
            sqlite3_bind_int64(s, 4, p.id);
        }
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

void saveWeights(bool perp, const std::array<double, AI_NF>& w, long long nSamp) {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s, "INSERT OR REPLACE INTO ai_weights(k,v) VALUES(?,?)")) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return;
    }
    auto bindKV = [&](int k, double v) -> bool {
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, k);
        sqlite3_bind_double(s, 2, v);
        return sqlite3_step(s) == SQLITE_DONE;
    };
    bool ok = true;
    for (int i = 0; i < AI_NF && ok; i++) ok = bindKV(wKey(perp, i), w[static_cast<size_t>(i)]);
    if (ok) ok = bindKV(nKey(perp), static_cast<double>(nSamp));
    if (ok) ok = bindKV(tKey(perp), static_cast<double>(hl::nowSec()));
    sqlite3_finalize(s);
    if (!ok || sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
}

void trainOne(bool perp) {
    struct Sample {
        std::array<double, AI_NF> f{};
        double y = 0;
    };
    std::vector<Sample> xs;
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) return;
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(db, &s,
                "SELECT buy_nanos,sell_nanos,n_buy,n_sell,wallets,one_share_bp,"
                "net_6h,net_prior,funding_nanos,liq_nanos,price_then,price_24h "
                "FROM ai_events WHERE filled_at>0 AND price_then>0 AND price_24h>0 "
                "AND window_days=1 AND venue=?"))
            return;
        sqlite3_bind_int(s, 1, perp ? 1 : 0);
        while (sqlite3_step(s) == SQLITE_ROW) {
            Row r;
            r.buy = sqlite3_column_int64(s, 0);
            r.sell = sqlite3_column_int64(s, 1);
            r.nBuy = sqlite3_column_int(s, 2);
            r.nSell = sqlite3_column_int(s, 3);
            r.wallets = sqlite3_column_int(s, 4);
            r.oneShare = sqlite3_column_int(s, 5) / 10000.0;
            r.net6h = sqlite3_column_int64(s, 6);
            r.netPrior = sqlite3_column_int64(s, 7);
            r.fundingNanos = sqlite3_column_int64(s, 8);
            r.liqUsd = static_cast<double>(sqlite3_column_int64(s, 9)) / 1000000000.0;
            r.perp = perp;
            const long long then = sqlite3_column_int64(s, 10);
            const long long later = sqlite3_column_int64(s, 11);
            if (then <= 0 || later <= 0) continue;
            const double ret = static_cast<double>(later - then) / static_cast<double>(then);
            if (std::fabs(ret) < 0.02) continue;
            const long long net = r.buy - r.sell;
            if (net == 0) continue;
            Sample sm;
            featuresOf(r, sm.f);
            sm.y = ((net > 0) == (ret > 0)) ? 1.0 : 0.0;
            xs.push_back(sm);
        }
        sqlite3_finalize(s);
    }
    if (static_cast<int>(xs.size()) < AI_MIN_TRAIN) return;

    std::array<double, AI_NF> w{};
    const double lr = 0.05;
    const double l2 = 0.01;
    for (int ep = 0; ep < 80; ep++) {
        for (const auto& sm : xs) {
            double z = 0;
            for (int i = 0; i < AI_NF; i++) z += w[static_cast<size_t>(i)] * sm.f[static_cast<size_t>(i)];
            const double p = sigmoid(z);
            const double err = p - sm.y;
            for (int i = 0; i < AI_NF; i++) {
                double g = err * sm.f[static_cast<size_t>(i)];
                if (i > 0) g += l2 * w[static_cast<size_t>(i)];
                w[static_cast<size_t>(i)] -= lr * g;
            }
        }
    }
    for (double v : w) {
        if (!std::isfinite(v)) return;
    }
    saveWeights(perp, w, static_cast<long long>(xs.size()));
    {
        std::lock_guard<std::mutex> l(g_wMutex);
        if (perp) { g_wPerp = w; g_trainedPerp = true; }
        else { g_wSpot = w; g_trainedSpot = true; }
    }
    std::cout << "[AI] trained " << (perp ? "perp" : "spot")
              << " on " << xs.size() << " 24h outcomes" << std::endl;
}

void trainWeights() {
    if (hl::nowSec() - g_lastTrain < 86400) return;
    g_lastTrain = hl::nowSec();
    trainOne(false);
    trainOne(true);
}

void cleanupEvents() {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s, "DELETE FROM ai_events WHERE ts<?")) return;
    sqlite3_bind_int64(s, 1, hl::nowSec() - AI_EVENT_TTL_SEC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

std::vector<Row> collectPassing(int days) {
    const long long since = hl::nowSec() - static_cast<long long>(days) * 86400LL;
    auto spot = loadSpot(since, bannedSpot());
    auto perp = loadPerp(since, bannedHl());
    enrichPerpFunding(perp);
    for (auto& r : perp) finish(r);
    spot.insert(spot.end(), perp.begin(), perp.end());
    return spot;
}

void snapshotHour() {
    ensureSchema();
    static long long lastSlot = 0;
    const long long slot = hl::nowSec() / 3600;
    if (slot == lastSlot) return;
    lastSlot = slot;
    auto rows = collectPassing(1);
    if (rows.size() > 400) {
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return std::fabs(a.score) > std::fabs(b.score);
        });
        rows.resize(400);
    }
    recordRows(1, rows);
}

}

AiMessage buildAiSignals(const std::string& chatId, int days) {
    days = clampDays(days);
    ensureSchema();
    const Lang lang = langFromCode(getUserLanguage(chatId));
    const long long since = hl::nowSec() - static_cast<long long>(days) * 86400LL;

    json kbCached;
    kbCached["inline_keyboard"] = json::array();
    kbCached["inline_keyboard"].push_back(json::array({
        json{{"text", std::string(days == 1 ? "\u2022 " : "") + tr(lang, "ai_w24")}, {"callback_data", "ai_open:1"}},
        json{{"text", std::string(days == 7 ? "\u2022 " : "") + tr(lang, "ai_w7")},  {"callback_data", "ai_open:7"}},
        json{{"text", std::string(days == 30 ? "\u2022 " : "") + tr(lang, "ai_w30")}, {"callback_data", "ai_open:30"}}
    }));
    kbCached["inline_keyboard"].push_back(json::array({
        json{{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));

    const int slot = cacheSlot(days);
    const int key = slot * 100 + static_cast<int>(lang);
    {
        std::lock_guard<std::mutex> l(g_aiCacheMutex);
        auto it = g_aiCache.find(key);
        if (it != g_aiCache.end() &&
            time(nullptr) - it->second.at < AI_CACHE_TTL_SEC[slot])
            return {it->second.text, kbCached.dump()};
    }

    auto spot = loadSpot(since, bannedSpot());
    auto perp = loadPerp(since, bannedHl());
    enrichPerpFunding(perp);
    for (auto& r : perp) finish(r);
    if (days == 1) {
        std::vector<Row> logged;
        logged.reserve(spot.size() + perp.size());
        logged.insert(logged.end(), spot.begin(), spot.end());
        logged.insert(logged.end(), perp.begin(), perp.end());
        recordRows(1, logged);
    }
    std::vector<Row> spotBuy, spotAvoid, perpBuy, perpAvoid;
    takeSides(spot, spotBuy, spotAvoid);
    takeSides(perp, perpBuy, perpAvoid);

    std::ostringstream t;
    t << "\U0001F916 <b>" << tr(lang, "ai_title") << "</b> · " << windowLabel(days, lang) << "\n\n";
    t << "<i>" << tr(lang, "ai_hint") << "</i>\n";

    const bool empty = spotBuy.empty() && spotAvoid.empty() && perpBuy.empty() && perpAvoid.empty();
    if (empty) {
        t << "\n" << tr(lang, "ai_empty");
    } else {
        auto dump = [&](const char* headKey, const std::vector<Row>& buys, const std::vector<Row>& avoids) {
            if (buys.empty() && avoids.empty()) return;
            t << "\n━━━━━━━━━━━━━━━━━━━━\n<b>" << tr(lang, headKey) << "</b>\n";
            if (!buys.empty()) {
                t << "\n🟢 " << tr(lang, "ai_buy") << "\n";
                for (size_t i = 0; i < buys.size(); i++) {
                    if (i) t << "\n";
                    writeCard(t, static_cast<int>(i), buys[i]);
                }
            }
            if (!avoids.empty()) {
                t << "\n🔴 " << tr(lang, "ai_avoid") << "\n";
                for (size_t i = 0; i < avoids.size(); i++) {
                    if (i) t << "\n";
                    writeCard(t, static_cast<int>(i), avoids[i]);
                }
            }
        };
        dump("ai_spot", spotBuy, spotAvoid);
        dump("ai_perp", perpBuy, perpAvoid);
    }

    json kb;
    kb["inline_keyboard"] = json::array();
    const std::string m24 = std::string(days == 1 ? "• " : "") + tr(lang, "ai_w24");
    const std::string m7  = std::string(days == 7 ? "• " : "") + tr(lang, "ai_w7");
    const std::string m30 = std::string(days == 30 ? "• " : "") + tr(lang, "ai_w30");
    kb["inline_keyboard"].push_back(json::array({
        json{{"text", m24}, {"callback_data", "ai_open:1"}},
        json{{"text", m7},  {"callback_data", "ai_open:7"}},
        json{{"text", m30}, {"callback_data", "ai_open:30"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        json{{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));
    {
        std::lock_guard<std::mutex> l(g_aiCacheMutex);
        g_aiCache[key] = {time(nullptr), t.str()};
    }
    return {t.str(), kb.dump()};
}

bool handleAiCallback(const std::string& chatId, const std::string& action,
                      const std::string& param, const std::string& data,
                      long long messageId, const std::string& callbackQueryId) {
    if (action != "ai_open") return false;
    if (chatId != OWNER_CHAT_ID) return true;
    int days = 1;
    try { days = std::stoi(param); } catch (...) { days = 1; }
    days = clampDays(days);
    rememberView(chatId, "ai_open:" + std::to_string(days));
    auto msg = buildAiSignals(chatId, days);
    replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    (void)data;
    (void)callbackQueryId;
    return true;
}

void aiTick() {
    fillOutcomes();
    snapshotHour();
    trainWeights();
    static int n = 0;
    if (++n % 60 == 0) cleanupEvents();
}
