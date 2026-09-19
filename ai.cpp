#include "ai.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>
#include "json.hpp"
#include "alert_settings.h"
#include "hyperliquid.h"
#include "hyperliquid_internal.h"
#include "oracle.h"
#include "ranking.h"
#include "ru.h"
#include "token_prices.h"
#include "utils.h"
#include "rpc_client.h"
#include "chains.h"

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;
extern const std::string OWNER_CHAT_ID;

// Доступ к Aladdin: владелец плюс те, кому он открыл вручную.
// Список в базе, а не в памяти: переживает перезапуск.


std::string getUserLanguage(const std::string& chatId);
void rememberView(const std::string& chatId, const std::string& data);

namespace {

constexpr long long AI_CACHE_TTL_SEC[] = { 60, 60, 60 };
int cacheSlot(int hours) { return hours == 1 ? 0 : (hours == 6 ? 1 : 2); }

struct AiCacheEntry {
    time_t at = 0;
    std::string text;
};
std::mutex g_aiCacheMutex;
std::map<int, AiCacheEntry> g_aiCache;

constexpr int AI_MIN_WALLETS = 3;
constexpr int AI_TOP_N = 10;
constexpr int AI_TRADE_N = 5;
constexpr double AI_MAX_ONE_SHARE = 0.70;
constexpr int AI_NF = 11;
constexpr int AI_MIN_TRAIN = 400;
constexpr int AI_TOP_WALLETS = 100;
constexpr long long AI_HORIZON_6H = 6LL * 3600;
constexpr long long AI_HORIZON_24H = 86400;
constexpr long long AI_EVENT_TTL_SEC = 90LL * 86400LL;

// Окна в ЧАСАХ, не днях: модель предсказывает движение на 6 и 24 часа,
// значит и приток осмысленно смотреть на том же горизонте. Приток за
// месяц размазан - киты могли выйти три недели назад.
// Поле window_days в базе оставлено под тем же именем, хранит часы.
int clampDays(int hours) {
    if (hours == 1 || hours == 6) return hours;
    return 24;
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
    int levBp = 0;
    long long liqFillNanos = 0;
    long long liqLongNanos = 0;    // вынесло лонги
    long long liqShortNanos = 0;   // вынесло шорты
    int bothVenues = 0;
    double topShare = 0;
    double topDir = 0;         // -1 топ продаёт, +1 топ покупает
    double rsi = 0;
    long long oiNanos = 0;
    long long mktVolNanos = 0;
    double regime = 0;         // 0 спокойно, 1 всплеск волатильности
    long long asOf = 0;          // момент, на который посчитан сигнал
};

std::mutex g_wMutex;
std::array<double, AI_NF> g_wSpot{};
std::array<double, AI_NF> g_wPerp{};
bool g_trainedSpot = false;
bool g_trainedPerp = false;
double g_accSpot = 0.0;
double g_calSpot = 1.0;
std::array<double, AI_NF> g_prevSpot{};
std::array<double, AI_NF> g_prevPerp{};
std::array<bool, AI_NF> g_stableSpot{};
std::array<bool, AI_NF> g_stablePerp{};
double g_calPerp = 1.0;
long long g_trainAtSpot = 0;
long long g_trainAtPerp = 0;
long long g_trainNSpot = 0;
long long g_trainNPerp = 0;
double g_accPerp = 0.0;
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
    f[0] = 1;
    f[1] = dir;
    f[2] = std::log1p(static_cast<double>(r.wallets)) / std::log1p(50.0);
    f[3] = 1.0 - r.oneShare;
    f[4] = std::log1p(scaled) / std::log1p(1000000.0);
    f[5] = accelOf(r.net6h, r.netPrior);
    f[6] = std::tanh(static_cast<double>(r.fundingNanos) / 10000000.0);
    f[7] = r.perp
        ? std::min(1.0, static_cast<double>(r.levBp) / 5000.0)
        : std::min(1.0, std::log1p(std::max(0.0, r.liqUsd)) / std::log1p(5000000.0));
    f[8] = r.topShare;
    f[9] = r.rsi != 0.0 ? (r.rsi - 50.0) / 50.0 : 0;
    {
        const long long lq = r.liqLongNanos, sq = r.liqShortNanos;
        const long long tq = lq + sq;
        f[10] = tq > 0 ? static_cast<double>(sq - lq) / static_cast<double>(tq) : 0.0;
    }
}

/* Строка потока в том виде, в каком её ждёт оракул. Одно место перевода:
   разойдись поля — модель считала бы одно, а обучалась на другом. */
OracleInput oracleInputOf(const Row& r) {
    OracleInput in;
    in.perp = r.perp;
    in.id = r.id;
    in.buy = r.buy;
    in.sell = r.sell;
    in.nBuy = r.nBuy;
    in.nSell = r.nSell;
    in.wallets = r.wallets;
    in.oneShare = r.oneShare;
    in.net6h = r.net6h;
    in.netPrior = r.netPrior;
    in.funding = r.fundingNanos;
    in.liqUsd = r.liqUsd;
    in.levBp = r.levBp;
    in.liqLong = r.liqLongNanos;
    in.liqShort = r.liqShortNanos;
    in.topShare = r.topShare;
    in.topDir = r.topDir;
    in.bothVenues = r.bothVenues;
    return in;
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
    // Оракул старше линии: он видит рынок, а не только поток. Нет его —
    // остаётся прежняя логистическая модель, нет и её — голая формула.
    if (oracleReady(r.perp)) {
        const OracleVerdict v = oracleScore(oracleInputOf(r), 0);
        if (v.known) {
            const double conf = h > 0 ? v.pUp : (1.0 - v.pUp);
            return (h > 0 ? 1.0 : -1.0) * conf * std::fabs(h);
        }
    }
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

int wKey(bool perp, int i) { return perp ? 500 + i : 400 + i; }
int nKey(bool perp) { return perp ? 300 : 100; }
int tKey(bool perp) { return perp ? 301 : 101; }
int accKey(bool perp) { return perp ? 302 : 102; }
int calKey(bool perp) { return perp ? 303 : 103; }

double readWeightKV(int k, double def) {
    sqlite3_stmt* s = nullptr;
    double v = def;
    if (!prepareOrLog(db, &s, "SELECT v FROM ai_weights WHERE k=?")) return def;
    sqlite3_bind_int(s, 1, k);
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_double(s, 0);
    sqlite3_finalize(s);
    return v;
}

void loadWeightSet(bool perp) {
    sqlite3_stmt* s = nullptr;
    std::array<double, AI_NF> w{};
    int got = 0;
    if (prepareOrLog(db, &s, "SELECT k,v FROM ai_weights WHERE k>=? AND k<?")) {
        sqlite3_bind_int(s, 1, wKey(perp, 0));
        sqlite3_bind_int(s, 2, wKey(perp, AI_NF));
        int rc = SQLITE_DONE;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            const int k = sqlite3_column_int(s, 0) - wKey(perp, 0);
            if (k >= 0 && k < AI_NF) {
                w[static_cast<size_t>(k)] = sqlite3_column_double(s, 1);
                got++;
            }
        }
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE) got = 0;
    }
    long long nSamp = static_cast<long long>(readWeightKV(nKey(perp), 0));
    const double acc = readWeightKV(accKey(perp), 0);
    double cal = readWeightKV(calKey(perp), 1);
    if (cal < 0.5) cal = 0.5;
    if (cal > 1.5) cal = 1.5;
    const long long at = static_cast<long long>(readWeightKV(tKey(perp), 0));
    if (got == AI_NF && nSamp >= AI_MIN_TRAIN) {
        std::lock_guard<std::mutex> l(g_wMutex);
        if (perp) {
            g_wPerp = w; g_trainedPerp = true; g_trainNPerp = nSamp;
            g_accPerp = acc; g_calPerp = cal; g_trainAtPerp = at;
            g_prevPerp = w;
            g_stablePerp.fill(true);
        } else {
            g_wSpot = w; g_trainedSpot = true; g_trainNSpot = nSamp;
            g_accSpot = acc; g_calSpot = cal; g_trainAtSpot = at;
            g_prevSpot = w;
            g_stableSpot.fill(true);
        }
    }
}

void ensureSchema() {
    if (g_schemaOk) return;
    std::lock_guard<std::mutex> lock(dbMutex);
    if (g_schemaOk) return;
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
        "  lev_bp INTEGER NOT NULL DEFAULT 0,"
        "  liq_fill_nanos INTEGER NOT NULL DEFAULT 0,"
        "  both_venues INTEGER NOT NULL DEFAULT 0,"
        "  top_share_bp INTEGER NOT NULL DEFAULT 0,"
        "  rsi_bp INTEGER NOT NULL DEFAULT 0,"
        "  oi_nanos INTEGER NOT NULL DEFAULT 0,"
        "  mkt_vol_nanos INTEGER NOT NULL DEFAULT 0,"
        "  UNIQUE(token, venue, window_days, hour_slot)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_ai_events_fill ON ai_events(filled_at, ts);"
        "CREATE INDEX IF NOT EXISTS idx_ai_events_fill6 ON ai_events(filled_6h, ts);"
        "CREATE TABLE IF NOT EXISTS ai_weights ("
        "  k INTEGER PRIMARY KEY,"
        "  v REAL NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS ai_access("
        "  chat_id TEXT PRIMARY KEY,"
        "  granted_at INTEGER NOT NULL DEFAULT 0"
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
        "ALTER TABLE ai_events ADD COLUMN lev_bp INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN top_dir_bp INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN liq_long_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN liq_short_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN regime_bp INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN liq_fill_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN both_venues INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN top_share_bp INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN rsi_bp INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN oi_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE ai_events ADD COLUMN mkt_vol_nanos INTEGER NOT NULL DEFAULT 0",
        nullptr
    };
    for (int i = 0; alts[i]; i++) {
        char* aerr = nullptr;
        sqlite3_exec(db, alts[i], nullptr, nullptr, &aerr);
        if (aerr) sqlite3_free(aerr);
    }
    sqlite3_exec(db,
        "DELETE FROM ai_events WHERE window_days=1 AND EXISTS ("
        "  SELECT 1 FROM ai_events e2 WHERE e2.token=ai_events.token"
        "  AND e2.venue=ai_events.venue AND e2.hour_slot=ai_events.hour_slot"
        "  AND e2.window_days=24);"
        "UPDATE ai_events SET window_days=24 WHERE window_days=1;",
        nullptr, nullptr, nullptr);
    loadWeightSet(false);
    loadWeightSet(true);
    g_schemaOk = true;
}



long long perpMarkNow(const std::string& coin) {
    std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return 0;
    const long long now = hl::nowSec();
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(hl::g_hlDb, &s,
            "SELECT mark_nanos,hour_ts FROM hl_funding_rate WHERE coin=? "
            "ORDER BY hour_ts DESC LIMIT 1"))
        return 0;
    sqlite3_bind_text(s, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
    long long mark = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const long long at = sqlite3_column_int64(s, 1);
        const long long v = sqlite3_column_int64(s, 0);
        if (v > 0 && now - at >= 0 && now - at <= 7200) mark = v;
    }
    sqlite3_finalize(s);
    if (mark > 0) return mark;
    if (!prepareOrLog(hl::g_hlDb, &s,
            "SELECT px,ts FROM hl_fills WHERE coin=? AND px!='' ORDER BY ts DESC LIMIT 1"))
        return 0;
    sqlite3_bind_text(s, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const long long fts = sqlite3_column_int64(s, 1);
        long long px = 0;
        if (now * 1000 - fts >= 0 && now * 1000 - fts <= 7200LL * 1000 &&
            hl::parseDecimalToNanos(safeColumnText(s, 0), px) && px > 0)
            mark = px;
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

// Типичный ход цены за сутки: медиана часовых изменений по модулю.
// Медиана, а не среднее - один выброс не должен раздувать стоп.
double volatilityOf(const std::string& token, bool perp) {
    std::vector<double> steps;
    {
        std::lock_guard<std::mutex> lock(perp ? hl::g_hlDbMutex : dbMutex);
        sqlite3* h = perp ? hl::g_hlDb : db;
        if (!h) return 0;
        sqlite3_stmt* s = nullptr;
        const char* q = perp
            ? "SELECT px FROM hl_fills WHERE coin=? AND ts>=? ORDER BY ts ASC LIMIT 400"
            : "SELECT price_nanos FROM token_price_history WHERE address=? AND ts>=? ORDER BY ts ASC LIMIT 400";
        if (!prepareOrLog(h, &s, q)) return 0;
        sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, perp ? (hl::nowSec() - 86400LL) * 1000LL
                                      : (hl::nowSec() - 86400LL));
        long long prev = 0;
        while (sqlite3_step(s) == SQLITE_ROW) {
            long long p = 0;
            if (perp) {
                if (!hl::parseDecimalToNanos(safeColumnText(s, 0), p) || p <= 0) continue;
            } else {
                p = sqlite3_column_int64(s, 0);
                if (p <= 0) continue;
            }
            if (prev > 0) {
                const double d = std::fabs(static_cast<double>(p - prev)) / static_cast<double>(prev);
                if (d > 0 && d < 0.5) steps.push_back(d);
            }
            prev = p;
        }
        sqlite3_finalize(s);
    }
    if (steps.size() < 8) return 0;
    std::sort(steps.begin(), steps.end());
    return steps[steps.size() / 2];
}

// Всплеск волатильности: последний час против обычного за сутки. Линейная
// модель усредняет режимы - пусть хотя бы знает, что рынок разогнался.
double regimeOf(const std::string& token, bool perp) {
    std::vector<long long> px;
    {
        std::lock_guard<std::mutex> lock(perp ? hl::g_hlDbMutex : dbMutex);
        sqlite3* h = perp ? hl::g_hlDb : db;
        if (!h) return 0.0;
        sqlite3_stmt* s = nullptr;
        const char* q = perp
            ? "SELECT px FROM hl_fills WHERE coin=? AND ts>=? ORDER BY ts ASC LIMIT 400"
            : "SELECT price_nanos FROM token_price_history WHERE address=? AND ts>=? ORDER BY ts ASC LIMIT 400";
        if (!prepareOrLog(h, &s, q)) return 0.0;
        sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, perp ? (hl::nowSec() - 86400LL) * 1000LL
                                      : (hl::nowSec() - 86400LL));
        while (sqlite3_step(s) == SQLITE_ROW) {
            const long long p = sqlite3_column_int64(s, 0);
            if (p > 0) px.push_back(p);
        }
        sqlite3_finalize(s);
    }
    if (px.size() < 10) return 0.0;

    std::vector<double> steps;
    for (size_t i = 1; i < px.size(); i++) {
        const double d = std::fabs(static_cast<double>(px[i] - px[i - 1]))
                       / static_cast<double>(px[i - 1]);
        if (d > 0 && d < 0.5) steps.push_back(d);
    }
    if (steps.size() < 8) return 0.0;

    const double last = steps.back();
    std::vector<double> sorted = steps;
    std::sort(sorted.begin(), sorted.end());
    const double med = sorted[sorted.size() / 2];
    if (med <= 0) return 0.0;

    const double ratio = last / med;
    return std::min(1.0, std::max(0.0, (ratio - 1.0) / 3.0));
}

struct TradePlan {
    bool valid = false;
    bool isLong = true;
    double entry = 0;
    double stop = 0;
    double take1 = 0;
    double take2 = 0;
    int leverage = 1;
    double riskPct = 0;      // насколько далеко стоп, в процентах
    double rr = 0;           // отношение прибыли к риску до первой цели
    double riskShare = 0;    // доля депозита под риском, в процентах
    long long horizon = 0;   // на каком горизонте модель это посчитала
};

// Уровни считаются от типичного хода цены за сутки, а не от круглых
// чисел. Стоп за пределом обычного шума, цели кратно риску.
TradePlan planOf(double px, double vol, double prob, bool isLong, double acc,
                 bool isPerp, bool thinLiq) {
    TradePlan t;
    if (px <= 0 || vol <= 0) return t;

    t.isLong = isLong;
    t.entry = px;

    double stopPct = vol * 2.5;
    if (stopPct < 0.015) stopPct = 0.015;
    if (stopPct > 0.15)  stopPct = 0.15;

    t.stop  = isLong ? px * (1.0 - stopPct) : px * (1.0 + stopPct);
    t.take1 = isLong ? px * (1.0 + stopPct * 1.5) : px * (1.0 - stopPct * 1.5);
    t.take2 = isLong ? px * (1.0 + stopPct * 3.0) : px * (1.0 - stopPct * 3.0);
    t.riskPct = stopPct * 100.0;
    t.rr = 1.5;

    // Плечо подбирается так, чтобы потеря на стопе была заданной долей
    // депозита. Уверенность модели и её точность задают эту долю:
    // от 2% при слабом сигнале до 15% при сильном и проверенном.
    const double confidence = std::max(0.0, std::min(1.0, (prob - 0.5) * 2.5));
    const double quality = acc > 0 ? std::min(1.0, std::max(0.0, (acc - 0.5) * 4.0)) : 0.25;
    // Точность меряется по ЗНАКУ движения, а не по судьбе плана: сигнал
    // может быть прав по направлению и всё равно выбит стопом на откате.
    // Пока метки плана нет, верх риска зажат - сильнее там, где выход
    // дороже: спот и тонкая ликвидность.
    double cap = 0.15;
    if (!isPerp)      cap = 0.06;          // на споте нет плеча, риск = размер позиции
    if (thinLiq)      cap = std::min(cap, 0.04);
    const double riskBudget = std::min(cap, 0.02 + 0.13 * confidence * quality);

    double lev = riskBudget / stopPct;
    if (lev < 1) lev = 1;
    if (lev > 10) lev = 10;
    t.leverage = static_cast<int>(lev + 0.5);

    t.valid = true;
    return t;
}

/* Уровни, названные моделью.
 *
 * Стоп ставится за пределом отката, который модель ждёт против сигнала, цель
 * — там, докуда она ждёт хода. Вшитых множителей волатильности здесь нет:
 * у спокойной монеты на тихом рынке стоп будет узкий, у разогнанной — широкий,
 * и решает это не константа 2.5, а то, как далеко цена уходила в таких же
 * условиях.
 *
 * Небольшие поправки всё же есть, и они не про рынок, а про исполнение: стоп
 * чуть дальше ожидаемого отката, иначе его снимает первым же шумом, а цель
 * чуть ближе ожидаемого хода, иначе до неё не доходит. */
TradePlan planFromLevels(double px, bool isLong, double up, double dn, double prob,
                         bool isPerp, bool thinLiq) {
    TradePlan t;
    if (px <= 0 || up <= 0 || dn <= 0) return t;
    const double adverse = isLong ? dn : up;
    const double favour  = isLong ? up : dn;

    double stopPct = adverse * 1.1;
    if (stopPct < 0.01) stopPct = 0.01;
    if (stopPct > 0.15) stopPct = 0.15;
    double t1 = favour * 0.8;
    if (t1 > 0.5) t1 = 0.5;
    double t2 = favour * 1.4;
    if (t2 > 1.0) t2 = 1.0;
    if (t2 < t1 * 1.2) t2 = t1 * 1.2;
    // Цель ближе стопа — сделка не стоит риска, и придумывать её не надо.
    if (t1 < stopPct * 0.9) return t;

    t.isLong = isLong;
    t.entry = px;
    t.stop  = isLong ? px * (1.0 - stopPct) : px * (1.0 + stopPct);
    t.take1 = isLong ? px * (1.0 + t1) : px * (1.0 - t1);
    t.take2 = isLong ? px * (1.0 + t2) : px * (1.0 - t2);
    t.riskPct = stopPct * 100.0;
    t.rr = t1 / stopPct;

    /* Доля депозита — по критерию Келли от чисел самой модели.
     *
     * Ставка, растущая быстрее всех в длинной череде сделок, равна
     * (p·b − q·a) / (a·b), где b — ход до цели, a — до стопа, p —
     * вероятность модели. Прежде здесь была лесенка «от 2% при слабом
     * сигнале до 15% при сильном»: числа взяты с потолка и не связаны ни с
     * расстоянием до стопа, ни с тем, сколько можно взять.
     *
     * Берём четверть от Келли. Полная ставка растит капитал быстрее всех, но
     * только при верной вероятности; наша верна приблизительно, и просадки у
     * полной ставки такие, что до долгого счёта доживают немногие. */
    const double b = t1, a = stopPct;
    const double p = std::min(0.99, std::max(0.01, prob));
    const double kelly = (p * b - (1.0 - p) * a) / (a * b);
    double stake = 0.25 * kelly;                 // доля депозита в номинале
    if (stake <= 0) return t;                    // ставка невыгодна — плана нет

    double cap = 0.15;
    if (!isPerp) cap = 0.06;
    if (thinLiq) cap = std::min(cap, 0.04);
    // Потолок — на потерю при стопе, а не на номинал: он и есть то, чем
    // человек рискует.
    if (stake * a > cap) stake = cap / a;
    double lev = stake;
    if (lev < 1) lev = 1;
    if (lev > 10) lev = 10;
    if (!isPerp && lev > 1) lev = 1;             // на споте плеча нет
    t.leverage = static_cast<int>(lev + 0.5);
    t.riskShare = std::min(cap, stake * a) * 100.0;
    t.valid = true;
    return t;
}

// Возраст последней записи цены. Разметка по цене недельной давности -
// не разметка, а шум: событие лучше не писать вовсе.
long long priceAgeOf(const std::string& token, bool perp) {
    if (perp) return 0;                    // на перпах цена из отметки, всегда свежая
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return -1;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "SELECT ts FROM token_price_history WHERE address=? ORDER BY ts DESC LIMIT 1"))
        return -1;
    sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    long long ts = 0;
    if (sqlite3_step(s) == SQLITE_ROW) ts = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return ts > 0 ? (hl::nowSec() - ts) : -1;
}

long long priceNowOf(bool perp, const std::string& id) {
    if (perp) return perpMarkNow(id);
    const uint64_t n = getPriceNanos(id);
    return n > 0 ? static_cast<long long>(n) : 0;
}

long long usdToNanos(double usd) {
    if (!(usd > 0) || usd > 1e12) return 0;
    return static_cast<long long>(usd * 1000000000.0 + 0.5);
}

long long dexUsdByToken(const std::string& addr) {
    if (addr.empty()) return 0;
    const auto body = http("https://api.dexscreener.com/latest/dex/tokens/" + addr, "", 3);
    if (body.empty()) return 0;
    try {
        const auto j = json::parse(body);
        if (!j.contains("pairs") || !j["pairs"].is_array()) return 0;
        const std::string want = chainCtx().dexscreenerChainId;
        double bestLiq = -1, bestPx = 0;
        for (const auto& p : j["pairs"]) {
            if (!p.is_object()) continue;
            if (!want.empty() && p.value("chainId", std::string()) != want) continue;
            if (!p.contains("priceUsd") || !p["priceUsd"].is_string()) continue;
            double liq = 0;
            if (p.contains("liquidity") && p["liquidity"].is_object() &&
                p["liquidity"].contains("usd") && p["liquidity"]["usd"].is_number())
                liq = p["liquidity"]["usd"].get<double>();
            double px = 0;
            try { px = std::stod(p["priceUsd"].get<std::string>()); } catch (...) { continue; }
            if (!(px > 0) || !std::isfinite(px)) continue;
            if (liq > bestLiq) { bestLiq = liq; bestPx = px; }
        }
        return usdToNanos(bestPx);
    } catch (...) { return 0; }
}

std::mutex g_midsMutex;
std::map<std::string, long long> g_mids;
time_t g_midsAt = 0;

long long hlMidNow(const std::string& coin) {
    const time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> l(g_midsMutex);
        if (now - g_midsAt < 60 && !g_mids.empty()) {
            auto it = g_mids.find(coin);
            if (it != g_mids.end()) return it->second;
        }
    }
    const json j = hl::infoPost(json{{"type", "allMids"}}, 2);
    std::map<std::string, long long> fresh;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!it.value().is_string()) continue;
            long long px = 0;
            if (hl::parseDecimalToNanos(it.value().get<std::string>(), px) && px > 0)
                fresh[it.key()] = px;
        }
    }
    long long hit = 0;
    {
        std::lock_guard<std::mutex> l(g_midsMutex);
        if (!fresh.empty()) { g_mids.swap(fresh); g_midsAt = now; }
        auto it = g_mids.find(coin);
        if (it != g_mids.end()) hit = it->second;
    }
    return hit;
}

struct LivePx {
    long long nanos = 0;
    time_t at = 0;
};
std::mutex g_livePxMutex;
std::map<std::string, LivePx> g_livePx;

long long livePriceOf(const Row& r) {
    const std::string key = std::string(r.perp ? "p:" : "s:") + r.id;
    const time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> l(g_livePxMutex);
        auto it = g_livePx.find(key);
        if (it != g_livePx.end() && it->second.nanos > 0 && now - it->second.at < 60)
            return it->second.nanos;
    }
    long long px = 0;
    if (r.perp) {
        px = hlMidNow(r.id);
        if (px <= 0) px = perpMarkNow(r.id);
    } else {
        px = dexUsdByToken(r.id);
        if (px <= 0) px = priceNowOf(false, r.id);
    }
    if (px > 0) {
        std::lock_guard<std::mutex> l(g_livePxMutex);
        g_livePx[key] = {px, now};
        if (g_livePx.size() > 400) {
            for (auto it = g_livePx.begin(); it != g_livePx.end(); )
                if (now - it->second.at > 300) it = g_livePx.erase(it);
                else ++it;
        }
    }
    return px;
}

struct FrozenEntry {
    long long px = 0;
    long long slot = 0;
};
std::mutex g_freezeMutex;
std::map<std::string, FrozenEntry> g_freeze;

bool keepSignal(const Row& r, bool wantLong, long long live) {
    if (live <= 0) return false;
    const long long slot = hl::nowSec() / 3600;
    const std::string key = std::string(r.perp ? "p:" : "s:")
                          + (wantLong ? "l:" : "h:") + r.id;
    long long first = 0;
    {
        std::lock_guard<std::mutex> l(g_freezeMutex);
        auto& f = g_freeze[key];
        if (f.slot != slot || f.px <= 0) { f.px = live; f.slot = slot; }
        first = f.px;
        if (g_freeze.size() > 400) {
            for (auto it = g_freeze.begin(); it != g_freeze.end(); )
                if (it->second.slot < slot) it = g_freeze.erase(it);
                else ++it;
        }
    }
    if (first <= 0) return true;
    const double move = static_cast<double>(live - first) / static_cast<double>(first);
    if (std::fabs(move) >= 0.02) return false;
    if (wantLong && move <= -0.005) return false;
    if (!wantLong && move >= 0.005) return false;
    return true;
}

double rsi14(const std::vector<long long>& px) {
    if (px.size() < 15) return 0;
    double gain = 0, loss = 0;
    for (size_t i = px.size() - 14; i < px.size(); i++) {
        const double d = static_cast<double>(px[i] - px[i - 1]);
        if (d > 0) gain += d;
        else loss -= d;
    }
    gain /= 14.0;
    loss /= 14.0;
    if (gain + loss < 1) return 0;
    if (loss < 1) return 100;
    return 100.0 - 100.0 / (1.0 + gain / loss);
}

double spotRsi(const std::string& token, long long asOf) {
    std::vector<long long> px;
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return 0;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "SELECT price_nanos FROM token_price_history "
            "WHERE address=? AND ts<=? AND price_nanos>0 ORDER BY ts DESC LIMIT 15"))
        return 0;
    sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, asOf);
    int rc = SQLITE_DONE;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        const long long v = sqlite3_column_int64(s, 0);
        if (v > 0) px.push_back(v);
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return 0;
    std::reverse(px.begin(), px.end());
    return rsi14(px);
}

void fillPerpCtx(Row& r, long long asOf) {
    std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return;
    sqlite3_stmt* s = nullptr;
    if (prepareOrLog(hl::g_hlDb, &s,
            "SELECT mark_nanos FROM hl_funding_rate "
            "WHERE coin=? AND hour_ts<=? AND mark_nanos>0 ORDER BY hour_ts DESC LIMIT 15")) {
        sqlite3_bind_text(s, 1, r.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, asOf);
        std::vector<long long> px;
        int rc = SQLITE_DONE;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            const long long v = sqlite3_column_int64(s, 0);
            if (v > 0) px.push_back(v);
        }
        sqlite3_finalize(s);
        if (rc == SQLITE_DONE) {
            std::reverse(px.begin(), px.end());
            r.rsi = rsi14(px);
        }
    }
    if (prepareOrLog(hl::g_hlDb, &s,
            "SELECT oi_nanos FROM hl_funding_rate "
            "WHERE coin=? AND hour_ts<=? AND oi_nanos>0 "
            "ORDER BY hour_ts DESC LIMIT 1")) {
        sqlite3_bind_text(s, 1, r.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, asOf);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const long long oi = sqlite3_column_int64(s, 0);
            if (oi > 0) r.oiNanos = oi;
        }
        sqlite3_finalize(s);
    }
    if (prepareOrLog(hl::g_hlDb, &s,
            "SELECT day_vlm_nanos FROM hl_funding_rate "
            "WHERE coin=? AND hour_ts<=? AND day_vlm_nanos>0 "
            "ORDER BY hour_ts DESC LIMIT 1")) {
        sqlite3_bind_text(s, 1, r.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, asOf);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const long long vlm = sqlite3_column_int64(s, 0);
            if (vlm > 0) r.mktVolNanos = vlm;
        }
        sqlite3_finalize(s);
    }
}

void fillSpotCtx(Row& r, long long asOf) {
    if (r.mktVolNanos <= 0) r.mktVolNanos = (r.buy > 0 ? r.buy : 0) + (r.sell > 0 ? r.sell : 0);
    r.rsi = spotRsi(r.id, asOf);
    std::string coin = r.name;
    for (char& c : coin) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (coin.empty()) return;
    std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(hl::g_hlDb, &s,
            "SELECT oi_nanos FROM hl_funding_rate "
            "WHERE coin=? AND hour_ts<=? AND oi_nanos>0 "
            "ORDER BY hour_ts DESC LIMIT 1"))
        return;
    sqlite3_bind_text(s, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, asOf);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const long long oi = sqlite3_column_int64(s, 0);
        if (oi > 0) r.oiNanos = oi;
    }
    sqlite3_finalize(s);
}

void enrichIndicators(std::vector<Row>& rows, long long asOf) {
    for (auto& r : rows) {
        if (r.perp) fillPerpCtx(r, asOf);
        else fillSpotCtx(r, asOf);
        finish(r);
    }
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
    int rc = SQLITE_DONE;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        std::string w = toLower(safeColumnText(s, 0));
        if (!w.empty()) out.insert(std::move(w));
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) out.clear();
    return out;
}

std::unordered_set<std::string> bannedHl() {
    std::unordered_set<std::string> out;
    std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return out;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(hl::g_hlDb, &s, "SELECT wallet FROM hl_banned")) return out;
    int rc = SQLITE_DONE;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        std::string w = toLower(safeColumnText(s, 0));
        if (!w.empty()) out.insert(std::move(w));
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) out.clear();
    return out;
}

std::unordered_map<std::string, std::string> tokenSymbols() {
    std::unordered_map<std::string, std::string> out;
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return out;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s, "SELECT address,symbol FROM token_cache")) return out;
    int rc = SQLITE_DONE;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        std::string addr = toLower(safeColumnText(s, 0));
        std::string sym = safeColumnText(s, 1);
        if (!addr.empty() && !sym.empty()) out[addr] = std::move(sym);
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) out.clear();
    return out;
}

struct Bucket {
    long long buy = 0;
    long long sell = 0;
    int nBuy = 0;
    int nSell = 0;
    long long net6h = 0;
    long long netPrior = 0;
    long long levNotional = 0;
    long long levWeight = 0;
    long long liqFill = 0;
    long long liqLong = 0;
    long long liqShort = 0;
    long long topVol = 0;
    long long topNet = 0;      // знак потока топовых: покупают или продают
    std::map<std::string, long long> vol;
};

void addVol(Bucket& b, const std::string& wallet, long long usd, bool isBuy,
            long long ts, long long t6, long long t24, bool fromTop) {
    if (usd < 0) usd = -usd;
    const long long signedUsd = isBuy ? usd : -usd;
    if (isBuy) { b.buy += usd; b.nBuy++; }
    else { b.sell += usd; b.nSell++; }
    b.vol[wallet] += usd;
    if (fromTop) { b.topVol += usd; b.topNet += signedUsd; }
    if (ts >= t6) b.net6h += signedUsd;
    else if (ts >= t24) b.netPrior += signedUsd;
}

Row toRow(const std::string& id, const std::string& name, const Bucket& b, bool perp, bool fetchLiq) {
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
    r.liqFillNanos = b.liqFill;
    r.liqLongNanos = b.liqLong;
    r.liqShortNanos = b.liqShort;
    if (b.levWeight > 0)
        r.levBp = static_cast<int>((b.levNotional * 100) / b.levWeight);
    long long mx = 0, tot = 0;
    for (const auto& p : b.vol) {
        tot += p.second;
        if (p.second > mx) mx = p.second;
    }
    r.oneShare = tot > 0 ? static_cast<double>(mx) / static_cast<double>(tot) : 1;
    r.topShare = tot > 0 ? static_cast<double>(b.topVol) / static_cast<double>(tot) : 0;
    // Доля без знака ничего не говорит: топ в плюсе может и продавать.
    r.topDir = b.topVol > 0 ? static_cast<double>(b.topNet) / static_cast<double>(b.topVol) : 0;
    if (!perp && fetchLiq) r.liqUsd = getPoolLiquidityUsd(id);
    finish(r);
    return r;
}

std::vector<Row> loadSpot(long long asOf, long long since, const std::unordered_set<std::string>& ban,
                          const std::unordered_set<std::string>& top) {
    std::map<std::string, Bucket> m;
    const long long t6 = asOf - AI_HORIZON_6H;
    const long long t24 = asOf - AI_HORIZON_24H;
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) return {};
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(db, &s,
                "SELECT token,wallet,is_buy,usd_nanos,timestamp FROM trades "
                "WHERE timestamp>=? AND timestamp<=?"))
            return {};
        sqlite3_bind_int64(s, 1, since);
        sqlite3_bind_int64(s, 2, asOf);
        int rc = SQLITE_DONE;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            const std::string token = toLower(safeColumnText(s, 0));
            const std::string wallet = toLower(safeColumnText(s, 1));
            if (token.empty() || wallet.empty() || ban.count(wallet)) continue;
            addVol(m[token], wallet, sqlite3_column_int64(s, 3),
                   sqlite3_column_int(s, 2) != 0, sqlite3_column_int64(s, 4), t6, t24,
                   top.count(wallet) != 0);
        }
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE) return {};
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
        Row r = toRow(p.first, name, p.second, false, true);
        r.asOf = asOf;
        r.regime = regimeOf(r.id, r.perp);
        if (r.wallets >= AI_MIN_WALLETS && (r.buy + r.sell) > 0 && r.oneShare <= AI_MAX_ONE_SHARE)
            out.push_back(std::move(r));
    }
    return out;
}

std::vector<Row> loadPerp(long long asOf, long long since, const std::unordered_set<std::string>& ban,
                          const std::unordered_set<std::string>& top) {
    std::map<std::string, Bucket> m;
    const long long asOfMs = asOf * 1000;
    const long long t6 = asOfMs - AI_HORIZON_6H * 1000;
    const long long t24 = asOfMs - AI_HORIZON_24H * 1000;
    const long long sinceMs = since * 1000;
    {
        std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
        if (!hl::g_hlDb) return {};
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(hl::g_hlDb, &s,
                "SELECT coin,wallet,dir_code,notional_nanos,ts,leverage FROM hl_fills "
                "WHERE ts>=? AND ts<=? AND dir_code IN (1,2,6,7,8)"))
            return {};
        sqlite3_bind_int64(s, 1, sinceMs);
        sqlite3_bind_int64(s, 2, asOfMs);
        int rc = SQLITE_DONE;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            const std::string coin = safeColumnText(s, 0);
            const std::string wallet = toLower(safeColumnText(s, 1));
            if (coin.empty() || wallet.empty() || ban.count(wallet)) continue;
            const int dir = sqlite3_column_int(s, 2);
            const long long notional = sqlite3_column_int64(s, 3);
            const long long ts = sqlite3_column_int64(s, 4);
            if (dir == DIR_LIQ_LONG || dir == DIR_LIQ_SHORT || dir == DIR_LIQ_OTHER) {
                long long a = notional < 0 ? -notional : notional;
                m[coin].liqFill += a;
                // Каскад в одну сторону - другой режим, чем просто много
                // ликвидаций: вынесло лонги или вынесло шорты.
                if (dir == DIR_LIQ_LONG)       m[coin].liqLong += a;
                else if (dir == DIR_LIQ_SHORT) m[coin].liqShort += a;
                continue;
            }
            addVol(m[coin], wallet, notional, dir == DIR_OPEN_LONG, ts, t6, t24,
                   top.count(wallet) != 0);
            int lev = sqlite3_column_int(s, 5);
            if (lev > 0) {
                if (lev > 100) lev = 100;
                long long a = notional < 0 ? -notional : notional;
                const long long lim = 90000000000000000LL / lev;
                if (a > lim) a = lim;
                m[coin].levWeight += a;
                m[coin].levNotional += a * static_cast<long long>(lev);
            }
        }
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE) return {};
    }
    std::vector<Row> out;
    out.reserve(m.size());
    for (const auto& p : m) {
        Row r = toRow(p.first, p.first, p.second, true, false);
        r.asOf = asOf;
        r.regime = regimeOf(r.id, r.perp);
        if (r.wallets >= AI_MIN_WALLETS && (r.buy + r.sell) > 0 && r.oneShare <= AI_MAX_ONE_SHARE)
            out.push_back(std::move(r));
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

void markBothVenues(std::vector<Row>& spot, std::vector<Row>& perp) {
    std::unordered_set<std::string> sn, pn;
    for (const auto& r : spot) sn.insert(toLower(r.name));
    for (const auto& r : perp) pn.insert(toLower(r.name));
    for (auto& r : spot) if (pn.count(toLower(r.name))) r.bothVenues = 1;
    for (auto& r : perp) if (sn.count(toLower(r.name))) r.bothVenues = 1;
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
    // Возраст цены собираем ДО замка: функция читает базу и взяла бы его
    // повторно. Та же ловушка, что была в истории.
    std::vector<long long> age;
    age.reserve(rows.size());
    for (const auto& r : rows) age.push_back(priceAgeOf(r.id, r.perp));
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "INSERT OR IGNORE INTO ai_events("
            "ts,hour_slot,window_days,venue,token,name,"
            "buy_nanos,sell_nanos,n_buy,n_sell,wallets,one_share_bp,score,price_then,"
            "net_6h,net_prior,funding_nanos,liq_nanos,lev_bp,liq_fill_nanos,both_venues,top_share_bp,"
            "rsi_bp,oi_nanos,mkt_vol_nanos,top_dir_bp,liq_long_nanos,liq_short_nanos,regime_bp)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
        return;
    for (size_t i = 0; i < rows.size(); i++) {
        const auto& r = rows[i];
        // Цена старше часа делает разметку шумом: через сутки сравнивать
        // будет не с чем. Такие события в журнал не пишем.
        if (age[i] < 0 || age[i] > 3600) continue;
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
        sqlite3_bind_int64(s, 18, static_cast<long long>(std::min(r.liqUsd, 1000000000.0) * 1000000000.0));
        sqlite3_bind_int(s, 19, r.levBp);
        sqlite3_bind_int64(s, 20, r.liqFillNanos);
        sqlite3_bind_int(s, 21, r.bothVenues);
        sqlite3_bind_int(s, 22, static_cast<int>(r.topShare * 10000.0 + 0.5));
        sqlite3_bind_int(s, 23, static_cast<int>(r.rsi * 100.0 + 0.5));
        sqlite3_bind_int64(s, 24, r.oiNanos);
        sqlite3_bind_int64(s, 25, r.mktVolNanos);
        sqlite3_bind_int(s, 26, static_cast<int>(r.topDir * 10000.0));
        sqlite3_bind_int64(s, 27, r.liqLongNanos);
        sqlite3_bind_int64(s, 28, r.liqShortNanos);
        sqlite3_bind_int(s, 29, static_cast<int>(r.regime * 10000.0));
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
    sqlite3_stmt* h = nullptr;
    if (!prepareOrLog(db, &h,
            "INSERT OR IGNORE INTO token_price_history(address,ts,price_nanos) VALUES(?,?,?)"))
        return;
    const long long histTs = slot * 3600;
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].perp || px[i] <= 0) continue;
        sqlite3_reset(h);
        sqlite3_clear_bindings(h);
        sqlite3_bind_text(h, 1, rows[i].id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(h, 2, histTs);
        sqlite3_bind_int64(h, 3, px[i]);
        sqlite3_step(h);
    }
    sqlite3_finalize(h);
}

int confPct(const Row& r, bool trained, bool wantLong) {
    if (oracleReady(r.perp)) {
        const OracleVerdict v = oracleScore(oracleInputOf(r), 0);
        if (v.known) {
            const double c = wantLong ? v.pUp : (1.0 - v.pUp);
            return std::min(99, std::max(1, static_cast<int>(c * 100.0 + 0.5)));
        }
    }
    if (trained) {
        std::array<double, AI_NF> w{};
        double k = 1;
        {
            std::lock_guard<std::mutex> l(g_wMutex);
            w = r.perp ? g_wPerp : g_wSpot;
            k = r.perp ? g_calPerp : g_calSpot;
        }
        std::array<double, AI_NF> f{};
        featuresOf(r, f);
        double z = 0;
        for (int i = 0; i < AI_NF; i++) z += w[static_cast<size_t>(i)] * f[static_cast<size_t>(i)];
        double p = sigmoid(z) * k;
        if (p > 0.99) p = 0.99;
        if (p < 0.01) p = 0.01;
        const double c = wantLong ? p : (1.0 - p);
        int v = static_cast<int>(c * 100.0 + 0.5);
        if (v < 1) v = 1;
        if (v > 99) v = 99;
        return v;
    }
    const double a = std::fabs(heuristicScore(r));
    int v = 40 + static_cast<int>(std::min(40.0, a * 5.0));
    if (v < 35) v = 35;
    if (v > 80) v = 80;
    return v;
}

/* Выбор горизонта и плана — одно место на оба выхода.
 *
 * Оракул обучен и на шести часах, и на сутках. Для каждого обученного окна
 * строится свой план, считается ожидаемый исход `p·цель − (1−p)·стоп`, и
 * побеждает большее. Раньше горизонт был один на всё, хотя монете на
 * разогнанном рынке сутки держать незачем, а на тихом за шесть часов ничего
 * не происходит.
 *
 * Раздельные реализации для чата и для мини-аппа означали бы, что человек
 * видит в Telegram один план, а в приложении другой, — поэтому считает это
 * одна функция, а оба выхода только рисуют её ответ. */
struct Choice {
    TradePlan plan;
    int conf = 0;
    bool modelled = false;
    std::string why;          // «flow:412,RSI:-88» — имя и сдвиг в сотых процента
};

Choice choosePlan(const Row& r, bool wantLong, double live, long long asOf,
                  double acc, bool thin, int fallbackConf, bool withWhy) {
    Choice k;
    k.conf = fallbackConf;
    if (live <= 0) return k;
    double bestEv = 0;
    for (const OracleView& view : oracleViews(oracleInputOf(r), asOf)) {
        if (!view.known) continue;
        const double p = wantLong ? view.pUp : (1.0 - view.pUp);
        /* Модель обязана согласиться со стороной. Сторону выбирает поток
           китов, вероятность считает модель, и при p ниже половины это
           значит «поток говорит покупать, а модель ждёт падения». Ожидаемый
           исход при перекошенных уровнях и тогда выходит плюсовым — цель
           дальше стопа в полтора раза, — но на экране получалась «Покупка ·
           40% шанс роста», то есть совет купить то, что модель считает
           падающим. Такого сигнала не будет: пусть лучше не будет никакого. */
        if (p < 0.5) continue;
        TradePlan cand;
        if (view.levels)
            cand = planFromLevels(live, wantLong, view.up, view.dn, p, r.perp, thin);
        if (!cand.valid) {
            const double vol = volatilityOf(r.id, r.perp);
            cand = planOf(live, vol, p, wantLong, acc, r.perp, thin);
        }
        if (!cand.valid) continue;
        const double reward = std::fabs(cand.take1 - live) / live;
        const double risk = std::fabs(cand.stop - live) / live;
        // Меньше нуля — сигнала на этом горизонте нет; меньше прочих — есть
        // горизонт получше.
        const double ev = p * reward - (1.0 - p) * risk;
        if (ev <= 0 || ev <= bestEv) continue;
        bestEv = ev;
        cand.horizon = view.horizon;
        k.plan = cand;
        k.conf = std::min(99, std::max(1, static_cast<int>(p * 100.0 + 0.5)));
        k.modelled = true;
        if (!withWhy) continue;
        std::ostringstream w;
        for (const OracleReason& why : oracleWhy(oracleInputOf(r), asOf, view.horizon, 3)) {
            const double shift = wantLong ? why.shift : -why.shift;
            if (!w.str().empty()) w << ",";
            w << why.name << ":" << static_cast<long long>(std::llround(shift * 10000.0));
        }
        k.why = w.str();
    }
    if (!k.plan.valid) {
        // Модели нет вовсе — старый путь: формула от волатильности.
        const double vol = volatilityOf(r.id, r.perp);
        k.plan = planOf(live, vol, k.conf / 100.0, wantLong, acc, r.perp, thin);
        k.plan.horizon = AI_HORIZON_24H;
        k.modelled = false;
    }
    return k;
}

/* Горизонт словами: «6 ч», «24 ч». Он у каждого сигнала свой, и подставляется
   в строку перевода — в шестнадцати языках единица стоит по-разному. */
std::string horizonWords(long long horizon, Lang lang) {
    const long long h = std::max(1LL, (horizon > 0 ? horizon : AI_HORIZON_24H) / 3600);
    std::string s = tr(lang, "ai_hours");
    const std::string::size_type at = s.find("{n}");
    if (at == std::string::npos) return std::to_string(h) + s;
    return s.replace(at, 3, std::to_string(h));
}

void writeWhy(std::ostringstream& t, const Row& r, Lang lang, bool trained, bool wantLong) {
    std::vector<std::pair<double, const char*>> xs;
    if (trained) {
        std::array<double, AI_NF> w{};
        {
            std::lock_guard<std::mutex> l(g_wMutex);
            w = r.perp ? g_wPerp : g_wSpot;
        }
        std::array<double, AI_NF> f{};
        featuresOf(r, f);
        const double sign = wantLong ? 1.0 : -1.0;
        std::array<bool, AI_NF> stable{};
        {
            std::lock_guard<std::mutex> l(g_wMutex);
            stable = r.perp ? g_stablePerp : g_stableSpot;
        }
        const char* keys[AI_NF] = {};
        keys[1] = "ai_why_flow";
        keys[2] = "ai_why_breadth";
        keys[3] = "ai_why_share";
        keys[7] = r.perp ? "ai_why_lev" : "ai_why_liq";
        keys[8] = "ai_why_top";
        keys[9] = "ai_why_rsi";
        keys[10] = r.perp ? "ai_why_liqskew" : nullptr;
        for (int i = 1; i < AI_NF; i++) {
            if (!keys[i]) continue;
            // Признак с прыгающим знаком не объясняет ничего: сегодня
            // «широкий приток», завтра он же против.
            if (!stable[static_cast<size_t>(i)]) continue;
            const double c = w[static_cast<size_t>(i)] * f[static_cast<size_t>(i)] * sign;
            if (c > 0.02) xs.push_back({c, keys[i]});
        }
    } else {
        const long long vol = r.buy + r.sell;
        const double dir = vol > 0 ? static_cast<double>(r.buy - r.sell) / static_cast<double>(vol) : 0;
        if ((wantLong && dir > 0.15) || (!wantLong && dir < -0.15)) xs.push_back({std::fabs(dir), "ai_why_flow"});
        if (r.wallets >= 8) xs.push_back({static_cast<double>(r.wallets) / 50.0, "ai_why_breadth"});
        if (r.oneShare <= 0.45) xs.push_back({1.0 - r.oneShare, "ai_why_share"});
        if (r.topShare >= 0.12) xs.push_back({r.topShare, "ai_why_top"});
        if (r.rsi > 0) {
            if (wantLong && r.rsi <= 60) xs.push_back({(60.0 - r.rsi) / 60.0, "ai_why_rsi"});
            if (!wantLong && r.rsi >= 40) xs.push_back({(r.rsi - 40.0) / 60.0, "ai_why_rsi"});
        }
        if (r.perp && r.liqLongNanos + r.liqShortNanos > 0)
            xs.push_back({0.15, "ai_why_liqskew"});
    }
    std::sort(xs.begin(), xs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (xs.empty()) return;
    t << tr(lang, "ai_why") << ": ";
    const int n = std::min(static_cast<int>(xs.size()), 3);
    for (int i = 0; i < n; i++) {
        if (i) t << " · ";
        t << tr(lang, xs[static_cast<size_t>(i)].second);
    }
    t << "\n";
}

// Цена монеты может быть и 60000, и 0.0000012 - знаков нужно разное число.
std::string fmtPx(double v) {
    char b[48];
    const double a = v < 0 ? -v : v;
    if (a >= 1000)      std::snprintf(b, sizeof(b), "%.2f", v);
    else if (a >= 1)    std::snprintf(b, sizeof(b), "%.4f", v);
    else if (a >= 0.01) std::snprintf(b, sizeof(b), "%.6f", v);
    else                std::snprintf(b, sizeof(b), "%.9f", v);
    return b;
}

void writeTrade(std::ostringstream& t, int i, const Row& r, Lang lang, bool trained, bool wantLong,
                long long livePx) {
    const char* medal = i == 0 ? "🥇 " : (i == 1 ? "🥈 " : (i == 2 ? "🥉 " : ""));
    const std::string num = i >= 3 ? "#" + std::to_string(i + 1) + " " : "";
    t << medal << num;
    const char* sideKey = r.perp ? (wantLong ? "ai_long" : "ai_short")
                                : (wantLong ? "ai_buy"  : "ai_sell");
    t << (wantLong ? "🟢 " : "🔴 ") << tr(lang, sideKey);
    t << " · <b>" << r.name << "</b>\n";
    const double px = static_cast<double>(livePx) / 1000000000.0;
    double acc = 0;
    {
        std::lock_guard<std::mutex> l(g_wMutex);
        acc = r.perp ? g_accPerp : g_accSpot;
    }
    const bool thinLiq = !r.perp && r.liqUsd > 0 && r.liqUsd < 200000.0;
    /* Тот же выбор горизонта и тот же план, что уходят в мини-апп: два
       расчёта означали бы два разных ответа на один вопрос. */
    const Choice ch = choosePlan(r, wantLong, px, hl::nowSec(), acc, thinLiq,
                                 confPct(r, trained, wantLong), false);
    const TradePlan tp = ch.plan;
    t << horizonWords(tp.horizon, lang) << " · " << tr(lang, "ai_conf") << " "
      << ch.conf << "%\n";
    t << tr(lang, "ai_market") << "\n";
    t << "<b>" << signedCompact(r.buy - r.sell) << "</b>";
    t << " · " << r.wallets << "\n";

    if (tp.valid) {
        t << tr(lang, "ai_entry") << " <code>" << fmtPx(tp.entry) << "</code>";
        if (r.perp) {
            /* Доля депозита под риском — то, чем человек рискует, и её
               называет модель (четверть Келли). Прежде здесь стояло
               «расстояние до стопа × плечо»: число выходило похожим, но
               значило другое и ни на что не опиралось. */
            const double budget = tp.riskShare > 0 ? tp.riskShare
                                                   : tp.riskPct * tp.leverage;
            t << " \u00B7 " << tr(lang, "ai_risk") << " "
              << std::fixed << std::setprecision(1) << budget << "%"
              << " (" << tp.leverage << "x)";
            t.unsetf(std::ios::fixed);
        }
        t << "\n";
        t << tr(lang, "ai_stop") << " <code>" << fmtPx(tp.stop) << "</code>"
          << " (" << std::fixed << std::setprecision(1) << tp.riskPct << "%)\n";
        t << tr(lang, "ai_take") << " <code>" << fmtPx(tp.take1) << "</code>"
          << " / <code>" << fmtPx(tp.take2) << "</code>\n";
        t.unsetf(std::ios::fixed);
    }

    writeWhy(t, r, lang, trained, wantLong);
}

std::string windowLabel(int hours, Lang lang) {
    if (hours == 1) return tr(lang, "ai_w1h");
    if (hours == 6) return tr(lang, "ai_w6h");
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
        const bool due6 = p.filled6 == 0 && p.ts <= now - AI_HORIZON_6H;
        const bool due24 = p.filled24 == 0 && p.ts <= now - AI_HORIZON_24H;
        if (!due6 && !due24) continue;
        const bool stale6 = now - p.ts > AI_HORIZON_6H + 3600;
        const bool stale24 = now - p.ts > AI_HORIZON_24H + 3600;
        long long then = p.priceThen;
        long long px6 = 0;
        long long px24 = 0;
        if (due6 && !stale6) px6 = priceNowOf(p.venue != 0, p.token);
        if (due24 && !stale24) px24 = priceNowOf(p.venue != 0, p.token);
        if (due6 && px6 <= 0 && !stale6) continue;
        if (due24 && px24 <= 0 && !stale24) continue;
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) return;
        sqlite3_stmt* s = nullptr;
        if (due6 && due24) {
            if (!prepareOrLog(db, &s,
                    "UPDATE ai_events SET price_then=?, price_6h=?, filled_6h=?,"
                    " price_24h=?, filled_at=? WHERE id=?"))
                continue;
            sqlite3_bind_int64(s, 1, then);
            sqlite3_bind_int64(s, 2, px6);
            sqlite3_bind_int64(s, 3, now);
            sqlite3_bind_int64(s, 4, px24);
            sqlite3_bind_int64(s, 5, now);
            sqlite3_bind_int64(s, 6, p.id);
        } else if (due24) {
            if (!prepareOrLog(db, &s,
                    "UPDATE ai_events SET price_then=?, price_24h=?, filled_at=? WHERE id=?"))
                continue;
            sqlite3_bind_int64(s, 1, then);
            sqlite3_bind_int64(s, 2, px24);
            sqlite3_bind_int64(s, 3, now);
            sqlite3_bind_int64(s, 4, p.id);
        } else {
            if (!prepareOrLog(db, &s,
                    "UPDATE ai_events SET price_then=?, price_6h=?, filled_6h=? WHERE id=?"))
                continue;
            sqlite3_bind_int64(s, 1, then);
            sqlite3_bind_int64(s, 2, px6);
            sqlite3_bind_int64(s, 3, now);
            sqlite3_bind_int64(s, 4, p.id);
        }
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

void saveWeights(bool perp, const std::array<double, AI_NF>& w, long long nSamp, double acc, double cal) {
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
    if (ok) ok = bindKV(accKey(perp), acc);
    if (ok) ok = bindKV(calKey(perp), cal);
    sqlite3_finalize(s);
    if (!ok || sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
}

void trainOne(bool perp) {
    struct Sample {
    long long ts = 0;
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
                "net_6h,net_prior,funding_nanos,liq_nanos,price_then,price_24h,top_share_bp,"
                "rsi_bp,oi_nanos,mkt_vol_nanos,e.ts,lev_bp,"
                "top_dir_bp,liq_long_nanos,liq_short_nanos,regime_bp "
                "FROM ai_events e WHERE filled_at>0 AND price_then>0 AND price_24h>0 "
                "AND window_days=24 AND venue=? "
                "AND NOT EXISTS ("
                "  SELECT 1 FROM ai_events e2 WHERE e2.token=e.token AND e2.venue=e.venue "
                "  AND e2.window_days=24 AND e2.filled_at>0 AND e2.price_then>0 AND e2.price_24h>0 "
                "  AND e2.ts/86400=e.ts/86400 AND e2.id<e.id)"))
            return;
        sqlite3_bind_int(s, 1, perp ? 1 : 0);
        int rc = SQLITE_DONE;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
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
            r.topShare = sqlite3_column_int(s, 12) / 10000.0;
            r.rsi = sqlite3_column_int(s, 13) / 100.0;
            r.oiNanos = sqlite3_column_int64(s, 14);
            r.mktVolNanos = sqlite3_column_int64(s, 15);
            const long long evTs = sqlite3_column_int64(s, 16);
            // Плечо китов: без него признак риска на перпах при обучении
            // был бы нулём, хотя в живом расчёте он есть.
            r.levBp = sqlite3_column_int(s, 17);
            r.topDir        = sqlite3_column_int(s, 18) / 10000.0;
            r.liqLongNanos  = sqlite3_column_int64(s, 19);
            r.liqShortNanos = sqlite3_column_int64(s, 20);
            r.regime        = sqlite3_column_int(s, 21) / 10000.0;
            const long long then = sqlite3_column_int64(s, 10);
            const long long later = sqlite3_column_int64(s, 11);
            if (then <= 0 || later <= 0) continue;
            const double ret = static_cast<double>(later - then) / static_cast<double>(then);
            if (std::fabs(ret) < 0.02) continue;
            const long long net = r.buy - r.sell;
            if (net == 0) continue;
            Sample sm;
            sm.ts = evTs;
            featuresOf(r, sm.f);
            sm.y = ((net > 0) == (ret > 0)) ? 1.0 : 0.0;
            xs.push_back(sm);
        }
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE) return;
    }
    if (static_cast<int>(xs.size()) < AI_MIN_TRAIN) return;

    // Генератор случайных чисел здесь когда-то перемешивал выборку. После
    // перехода на проверку по времени перемешивать нечего, и обучение стало
    // повторяемым: одни и те же данные дают одни и те же веса.
    // Проверяем на САМЫХ СВЕЖИХ примерах, а не на случайных. Со случайным
    // разбиением модель учится на будущем и проверяется на прошлом - в бою
    // так не бывает, и точность выходит завышенной.
    std::sort(xs.begin(), xs.end(),
              [](const Sample& a, const Sample& b) { return a.ts < b.ts; });

    const size_t holdN = xs.size() / 5;
    std::vector<Sample> hold(xs.end() - static_cast<long>(holdN), xs.end());
    xs.resize(xs.size() - holdN);

    std::array<double, AI_NF> w{};
    const double lr = 0.05;
    const double l2 = 0.01;
    // Затухание по времени: свежие дни весят больше. Включаем только при
    // большой выборке - на четырёхстах примерах оно отберёт вес у и так
    // небольшого набора.
    const bool useDecay = xs.size() >= 1000;
    const long long newest = xs.empty() ? 0 : xs.back().ts;

    for (int ep = 0; ep < 80; ep++) {
        for (const auto& sm : xs) {
            double wgt = 1.0;
            if (useDecay && newest > 0) {
                const double ageDays = static_cast<double>(newest - sm.ts) / 86400.0;
                wgt = std::exp(-ageDays / 30.0);   // за месяц вес падает втрое
                if (wgt < 0.1) wgt = 0.1;
            }
            double z = 0;
            for (int i = 0; i < AI_NF; i++) z += w[static_cast<size_t>(i)] * sm.f[static_cast<size_t>(i)];
            const double p = sigmoid(z);
            const double err = p - sm.y;
            for (int i = 0; i < AI_NF; i++) {
                double g = err * wgt * sm.f[static_cast<size_t>(i)];
                if (i > 0) g += l2 * w[static_cast<size_t>(i)];
                w[static_cast<size_t>(i)] -= lr * g;
            }
        }
    }
    for (double v : w) {
        if (!std::isfinite(v)) return;
    }

    // Точность на отложенных: сколько раз знак предсказания совпал с тем,
    // что вышло. Ниже 55% модель не лучше монетки - веса не применяем.
    int hit = 0;
    for (const auto& sm : hold) {
        double z = 0;
        for (int i = 0; i < AI_NF; i++) z += w[static_cast<size_t>(i)] * sm.f[static_cast<size_t>(i)];
        if ((sigmoid(z) > 0.5) == (sm.y > 0.5)) hit++;
    }
    const double acc = holdN > 0 ? static_cast<double>(hit) / static_cast<double>(holdN) : 0.0;
    if (holdN >= 40 && acc < 0.55) {
        std::cout << "[AI] " << (perp ? "perp" : "spot")
                  << ": точность на отложенных " << static_cast<int>(acc * 100)
                  << "% - веса не применяем" << std::endl;
        return;
    }

    double k = 1.0;
    if (holdN >= 40) {
        double sumP = 0, sumY = 0;
        for (const auto& sm : hold) {
            double z = 0;
            for (int i = 0; i < AI_NF; i++) z += w[static_cast<size_t>(i)] * sm.f[static_cast<size_t>(i)];
            sumP += sigmoid(z);
            sumY += sm.y;
        }
        if (sumP > 1.0) {
            k = sumY / sumP;
            if (k < 0.5) k = 0.5;
            if (k > 1.5) k = 1.5;
        }
    }
    // Считаем все размеченные исходы, а не только те, что ушли в обучение.
    // AI_MIN_TRAIN — один и тот же порог в трёх местах: до разбиения его
    // сверяют с полной выборкой (и экран показывает «413 / 400»), а
    // сохранялась пятая часть от неё, отложенная на проверку. Модель,
    // обученную на 400..499 исходах, при следующем запуске переставали
    // считать обученной: loadWeightSet и whale_api.py сверяют это же число
    // с тем же порогом. Перпы как раз в этой дыре и сидели.
    const long long seen = static_cast<long long>(xs.size() + hold.size());
    saveWeights(perp, w, seen, acc, k);
    const long long at = hl::nowSec();
    {
        std::lock_guard<std::mutex> l(g_wMutex);
        auto& prev = perp ? g_prevPerp : g_prevSpot;
        auto& stable = perp ? g_stablePerp : g_stableSpot;
        const bool hadPrev = perp ? g_trainedPerp : g_trainedSpot;
        for (int i = 0; i < AI_NF; i++) {
            const size_t u = static_cast<size_t>(i);
            stable[u] = hadPrev && prev[u] * w[u] > 0 && std::fabs(w[u]) > 0.05;
            prev[u] = w[u];
        }
        if (perp) {
            g_wPerp = w; g_trainedPerp = true;
            g_accPerp = acc; g_calPerp = k;
            g_trainAtPerp = at; g_trainNPerp = seen;
        } else {
            g_wSpot = w; g_trainedSpot = true;
            g_accSpot = acc; g_calSpot = k;
            g_trainAtSpot = at; g_trainNSpot = seen;
        }
    }
    std::cout << "[AI] trained " << (perp ? "perp" : "spot")
              << " on " << xs.size() << " of " << seen << " 24h outcomes, hold "
              << holdN << ", acc " << static_cast<int>(acc * 100.0 + 0.5) << "%"
              << std::endl;
}

void trainWeights() {
    const long long now = hl::nowSec();
    bool both = false;
    {
        std::lock_guard<std::mutex> l(g_wMutex);
        both = g_trainedSpot && g_trainedPerp;
    }
    // Раз в 4 часа, а не в сутки: рынок меняет режим быстрее, чем
    // переобучение. Веса вчерашнего дня после каскада ликвидаций или
    // листинга уже не про этот рынок.
    // Пока модели нет - пробуем каждый час, чтобы не ждать лишнего.
    if (now - g_lastTrain < (both ? 4 * 3600 : 3600)) return;
    g_lastTrain = now;
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

int countReady(bool perp) {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return 0;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "SELECT COUNT(*) FROM ai_events e "
            "WHERE filled_at>0 AND price_then>0 AND price_24h>0 "
            "AND window_days=24 AND venue=? AND buy_nanos!=sell_nanos "
            "AND ABS(price_24h-price_then)*50>=price_then "
            "AND NOT EXISTS ("
            "  SELECT 1 FROM ai_events e2 WHERE e2.token=e.token AND e2.venue=e.venue "
            "  AND e2.window_days=24 AND e2.filled_at>0 AND e2.price_then>0 AND e2.price_24h>0 "
            "  AND e2.ts/86400=e.ts/86400 AND e2.id<e.id)"))
        return 0;
    sqlite3_bind_int(s, 1, perp ? 1 : 0);
    int n = 0;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

// Окно в ЧАСАХ, как и на экране. Раньше здесь были дни, а обучение искало
// window_days=24 - события с такой пометкой не появлялись никогда.
std::vector<Row> collectPassing(int hours, long long asOf) {
    if (asOf <= 0) asOf = hl::nowSec();
    const long long since = asOf - static_cast<long long>(hours) * 3600LL;
    const auto topS = spotTopPnlWallets(AI_TOP_WALLETS);
    const auto topP = hlTopPnlWallets(AI_TOP_WALLETS);
    auto spot = loadSpot(asOf, since, bannedSpot(), topS);
    auto perp = loadPerp(asOf, since, bannedHl(), topP);
    enrichPerpFunding(perp);
    markBothVenues(spot, perp);
    enrichIndicators(spot, asOf);
    enrichIndicators(perp, asOf);
    spot.insert(spot.end(), perp.begin(), perp.end());
    return spot;
}

long long pxFromUsdRaw(__int128 usd, __int128 raw, int dec) {
    if (usd <= 0 || raw <= 0 || dec < 0 || dec > 18) return 0;
    __int128 scale = 1;
    for (int i = 0; i < dec; i++) scale *= 10;
    __int128 v = usd * scale / raw;
    if (v <= 0 || v > 9000000000000000000LL) return 0;
    return static_cast<long long>(v);
}

void insertLabeled(const Row& r, long long ts, long long slot, long long thenPx, long long laterPx) {
    if (thenPx <= 0 || laterPx <= 0) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "INSERT OR IGNORE INTO ai_events("
            "ts,hour_slot,window_days,venue,token,name,"
            "buy_nanos,sell_nanos,n_buy,n_sell,wallets,one_share_bp,score,price_then,"
            "price_24h,filled_at,net_6h,net_prior,funding_nanos,liq_nanos,lev_bp,"
            "liq_fill_nanos,both_venues,top_share_bp,rsi_bp,oi_nanos,mkt_vol_nanos,"
            "top_dir_bp,liq_long_nanos,liq_short_nanos,regime_bp) "
            "VALUES(?,?,24,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
        return;
    sqlite3_bind_int64(s, 1, ts);
    sqlite3_bind_int64(s, 2, slot);
    sqlite3_bind_int(s, 3, r.perp ? 1 : 0);
    sqlite3_bind_text(s, 4, r.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, r.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 6, r.buy);
    sqlite3_bind_int64(s, 7, r.sell);
    sqlite3_bind_int(s, 8, r.nBuy);
    sqlite3_bind_int(s, 9, r.nSell);
    sqlite3_bind_int(s, 10, r.wallets);
    sqlite3_bind_int(s, 11, static_cast<int>(r.oneShare * 10000.0 + 0.5));
    sqlite3_bind_double(s, 12, r.score);
    sqlite3_bind_int64(s, 13, thenPx);
    sqlite3_bind_int64(s, 14, laterPx);
    sqlite3_bind_int64(s, 15, ts + AI_HORIZON_24H);
    sqlite3_bind_int64(s, 16, r.net6h);
    sqlite3_bind_int64(s, 17, r.netPrior);
    sqlite3_bind_int64(s, 18, r.fundingNanos);
    sqlite3_bind_int64(s, 19, static_cast<long long>(std::min(r.liqUsd, 1000000000.0) * 1000000000.0));
    sqlite3_bind_int(s, 20, r.levBp);
    sqlite3_bind_int64(s, 21, r.liqFillNanos);
    sqlite3_bind_int(s, 22, r.bothVenues);
    sqlite3_bind_int(s, 23, static_cast<int>(r.topShare * 10000.0 + 0.5));
    sqlite3_bind_int(s, 24, static_cast<int>(r.rsi * 100.0 + 0.5));
    sqlite3_bind_int64(s, 25, r.oiNanos);
    sqlite3_bind_int64(s, 26, r.mktVolNanos);
    sqlite3_bind_int(s, 27, static_cast<int>(r.topDir * 10000.0));
    sqlite3_bind_int64(s, 28, r.liqLongNanos);
    sqlite3_bind_int64(s, 29, r.liqShortNanos);
    sqlite3_bind_int(s, 30, static_cast<int>(r.regime * 10000.0));
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void backfillJournal() {
    static bool done = false;
    if (done) return;
    ensureSchema();
    const long long now = hl::nowSec();
    const long long oldest = now - AI_EVENT_TTL_SEC;
    const auto banS = bannedSpot();
    const auto banP = bannedHl();
    const auto topS = spotTopPnlWallets(AI_TOP_WALLETS);
    const auto topP = hlTopPnlWallets(AI_TOP_WALLETS);
    const auto names = tokenSymbols();

    struct DayAgg {
        Bucket b;
        __int128 usd = 0;
        __int128 raw = 0;
        __int128 pxUsd = 0;
        __int128 pxW = 0;
    };
    std::map<std::string, std::map<long long, DayAgg>> spot;
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) { done = true; return; }
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(db, &s,
                "SELECT token,wallet,is_buy,usd_nanos,token_amount,timestamp FROM trades "
                "WHERE timestamp>=?")) {
            done = true;
            return;
        }
        sqlite3_bind_int64(s, 1, oldest);
        int rc = SQLITE_DONE;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            const std::string token = toLower(safeColumnText(s, 0));
            const std::string wallet = toLower(safeColumnText(s, 1));
            if (token.empty() || wallet.empty() || banS.count(wallet)) continue;
            const long long ts = sqlite3_column_int64(s, 5);
            const long long day = ts / 86400;
            const long long usd = sqlite3_column_int64(s, 3);
            DayAgg& a = spot[token][day];
            addVol(a.b, wallet, usd, sqlite3_column_int(s, 2) != 0, ts,
                   (day + 1) * 86400 - AI_HORIZON_6H, (day + 1) * 86400 - AI_HORIZON_24H,
                   topS.count(wallet) != 0);
            if (usd > 0) {
                a.usd += usd;
                try {
                    const long long raw = std::stoll(safeColumnText(s, 4));
                    if (raw > 0) a.raw += raw;
                } catch (...) {}
            }
        }
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE) { done = true; return; }
    }

    std::map<std::string, std::map<long long, DayAgg>> perp;
    {
        std::lock_guard<std::mutex> lock(hl::g_hlDbMutex);
        if (hl::g_hlDb) {
            sqlite3_stmt* s = nullptr;
            if (prepareOrLog(hl::g_hlDb, &s,
                    "SELECT coin,wallet,dir_code,notional_nanos,ts,leverage,px FROM hl_fills "
                    "WHERE ts>=? AND dir_code IN (1,2,6,7,8)")) {
                sqlite3_bind_int64(s, 1, oldest * 1000);
                int rc = SQLITE_DONE;
                while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
                    const std::string coin = safeColumnText(s, 0);
                    const std::string wallet = toLower(safeColumnText(s, 1));
                    if (coin.empty() || wallet.empty() || banP.count(wallet)) continue;
                    const int dir = sqlite3_column_int(s, 2);
                    const long long notional = sqlite3_column_int64(s, 3);
                    const long long tsMs = sqlite3_column_int64(s, 4);
                    const long long day = (tsMs / 1000) / 86400;
                    DayAgg& a = perp[coin][day];
                    if (dir == DIR_LIQ_LONG || dir == DIR_LIQ_SHORT || dir == DIR_LIQ_OTHER) {
                        long long x = notional < 0 ? -notional : notional;
                        a.b.liqFill += x;
                        continue;
                    }
                    addVol(a.b, wallet, notional, dir == DIR_OPEN_LONG, tsMs,
                           ((day + 1) * 86400 - AI_HORIZON_6H) * 1000,
                           ((day + 1) * 86400 - AI_HORIZON_24H) * 1000,
                           topP.count(wallet) != 0);
                    int lev = sqlite3_column_int(s, 5);
                    if (lev > 0) {
                        if (lev > 100) lev = 100;
                        long long x = notional < 0 ? -notional : notional;
                        const long long lim = 90000000000000000LL / lev;
                        if (x > lim) x = lim;
                        a.b.levWeight += x;
                        a.b.levNotional += x * static_cast<long long>(lev);
                    }
                    long long px = 0;
                    if (hl::parseDecimalToNanos(safeColumnText(s, 6), px) && px > 0) {
                        __int128 w = notional < 0 ? -notional : notional;
                        if (w <= 0) w = 1;
                        a.pxUsd += static_cast<__int128>(px) * w;
                        a.pxW += w;
                    }
                }
                sqlite3_finalize(s);
                if (rc != SQLITE_DONE) { done = true; return; }
            }
        }
    }

    int nS = 0, nP = 0;
    struct Labeled {
        Row r;
        long long ts = 0;
        long long slot = 0;
        long long thenPx = 0;
        long long laterPx = 0;
    };
    std::vector<Labeled> labeled;
    for (auto& tk : spot) {
        std::string name = getSymbol(tk.first);
        if (name.empty() || name == "UNKNOWN") {
            auto it = names.find(tk.first);
            if (it == names.end()) continue;
            name = it->second;
        }
        name = safeString(name, 16);
        const int dec = getDecimals(tk.first);
        for (auto& d : tk.second) {
            auto nxt = tk.second.find(d.first + 1);
            if (nxt == tk.second.end()) continue;
            Row r = toRow(tk.first, name, d.second.b, false, false);
            r.asOf = hl::nowSec();
            r.regime = regimeOf(r.id, r.perp);
            if (r.wallets < AI_MIN_WALLETS || (r.buy + r.sell) <= 0 || r.oneShare > AI_MAX_ONE_SHARE)
                continue;
            const long long thenPx = pxFromUsdRaw(d.second.usd, d.second.raw, dec);
            const long long laterPx = pxFromUsdRaw(nxt->second.usd, nxt->second.raw, dec);
            if (thenPx <= 0 || laterPx <= 0) continue;
            std::vector<long long> series;
            for (long long day = d.first - 14; day <= d.first; day++) {
                auto it = tk.second.find(day);
                if (it == tk.second.end()) continue;
                const long long p = pxFromUsdRaw(it->second.usd, it->second.raw, dec);
                if (p > 0) series.push_back(p);
            }
            r.rsi = rsi14(series);
            r.mktVolNanos = (r.buy > 0 ? r.buy : 0) + (r.sell > 0 ? r.sell : 0);
            fillSpotCtx(r, d.first * 86400);
            if (r.rsi == 0) r.rsi = rsi14(series);
            labeled.push_back({std::move(r), d.first * 86400, d.first * 24, thenPx, laterPx});
        }
    }
    for (auto& ck : perp) {
        for (auto& d : ck.second) {
            auto nxt = ck.second.find(d.first + 1);
            if (nxt == ck.second.end()) continue;
            Row r = toRow(ck.first, ck.first, d.second.b, true, false);
            r.asOf = hl::nowSec();
            r.regime = regimeOf(r.id, r.perp);
            if (r.wallets < AI_MIN_WALLETS || (r.buy + r.sell) <= 0 || r.oneShare > AI_MAX_ONE_SHARE)
                continue;
            if (d.second.pxW <= 0 || nxt->second.pxW <= 0) continue;
            const __int128 tpx = d.second.pxUsd / d.second.pxW;
            const __int128 lpx = nxt->second.pxUsd / nxt->second.pxW;
            if (tpx <= 0 || lpx <= 0 || tpx > 9000000000000000000LL || lpx > 9000000000000000000LL) continue;
            const long long thenPx = static_cast<long long>(tpx);
            const long long laterPx = static_cast<long long>(lpx);
            std::vector<long long> series;
            for (long long day = d.first - 14; day <= d.first; day++) {
                auto it = ck.second.find(day);
                if (it == ck.second.end() || it->second.pxW <= 0) continue;
                const __int128 p = it->second.pxUsd / it->second.pxW;
                if (p > 0 && p <= 9000000000000000000LL) series.push_back(static_cast<long long>(p));
            }
            r.rsi = rsi14(series);
            fillPerpCtx(r, d.first * 86400);
            if (r.rsi == 0) r.rsi = rsi14(series);
            labeled.push_back({std::move(r), d.first * 86400, d.first * 24, thenPx, laterPx});
        }
    }
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) { done = true; return; }
        for (const auto& x : labeled) {
            insertLabeled(x.r, x.ts, x.slot, x.thenPx, x.laterPx);
            if (x.r.perp) nP++;
            else nS++;
        }
    }
    done = true;
    std::cout << "[AI] backfill labeled " << nS << " spot · " << nP << " perp days" << std::endl;
}

/* Сигналы для приложения считает бот и кладёт в базу готовыми.
 *
 * Раньше их пересчитывал whale_api.py своей формулой: уверенность была
 * «50 + модуль потока × 8», причины — вшитым списком правил. Модель при этом
 * жила в боте и до приложения не доходила вовсе — на экране стояло 99%,
 * которых никто не считал. Второй раз повторять признаки на другом языке
 * нельзя: разойдутся — и никто не заметит. Поэтому единственный счёт здесь,
 * а API работает выдачей.
 */
void ensureSignalSchema() {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return;
    const char* schema =
        "CREATE TABLE IF NOT EXISTS ai_signals ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  made_at INTEGER NOT NULL,"
        "  venue INTEGER NOT NULL,"
        "  token TEXT NOT NULL,"
        "  sym TEXT NOT NULL,"
        "  side INTEGER NOT NULL,"
        "  conf INTEGER NOT NULL,"
        "  modelled INTEGER NOT NULL DEFAULT 0,"
        "  net_nanos INTEGER NOT NULL DEFAULT 0,"
        "  wallets INTEGER NOT NULL DEFAULT 0,"
        "  entry REAL NOT NULL DEFAULT 0,"
        "  stop REAL NOT NULL DEFAULT 0,"
        "  take1 REAL NOT NULL DEFAULT 0,"
        "  take2 REAL NOT NULL DEFAULT 0,"
        "  risk_pct REAL NOT NULL DEFAULT 0,"
        "  lev INTEGER NOT NULL DEFAULT 1,"
        "  risk_share REAL NOT NULL DEFAULT 0,"
        "  horizon INTEGER NOT NULL DEFAULT 86400,"
        "  why TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS idx_ai_signals_made ON ai_signals(made_at);"
        /* Журнал выданных сигналов. Экран истории раньше показывал строки
           журнала обучения — то есть монеты, которые сигналами никогда не
           были, — и считал по ним «угадано». Настоящая история может быть
           только такой: что выдали, с какими уровнями, и чем это кончилось
           через сутки. */
        "CREATE TABLE IF NOT EXISTS ai_signal_log ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  made_at INTEGER NOT NULL,"
        "  venue INTEGER NOT NULL,"
        "  token TEXT NOT NULL,"
        "  sym TEXT NOT NULL,"
        "  side INTEGER NOT NULL,"
        "  conf INTEGER NOT NULL,"
        "  modelled INTEGER NOT NULL DEFAULT 0,"
        "  entry REAL NOT NULL,"
        "  stop REAL NOT NULL,"
        "  take1 REAL NOT NULL,"
        "  take2 REAL NOT NULL,"
        "  closed_at INTEGER NOT NULL DEFAULT 0,"
        "  exit_px REAL NOT NULL DEFAULT 0,"
        "  outcome INTEGER NOT NULL DEFAULT 0,"   /* 1 цель, -1 стоп, 0 ни то ни другое */
        "  horizon INTEGER NOT NULL DEFAULT 86400,"
        "  ret_bp INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_signal_log_open ON ai_signal_log(closed_at, made_at);"
        "CREATE INDEX IF NOT EXISTS idx_signal_log_live ON ai_signal_log(venue, token, side, closed_at);";
    char* err = nullptr;
    if (sqlite3_exec(db, schema, nullptr, nullptr, &err) != SQLITE_OK)
        std::cerr << "[AI] сигналы: схема не создана: " << (err ? err : "?") << std::endl;
    if (err) sqlite3_free(err);

    /* Столбцы, появившиеся позже. У базы, где таблицы уже заведены,
       CREATE TABLE IF NOT EXISTS не делает ничего, и новых столбцов в ней
       не появится само.
       Забыть про это здесь стоило дорого: INSERT падал на каждой публикации,
       транзакция откатывалась вместе с очисткой, ai_signals оставалась
       пустой — и на экране стояло «бот ещё не присылал сигналов», хотя бот
       считал их каждые пять минут. Журнал выданных не писался по той же
       причине, поэтому и история не набиралась. */
    const char* alts[] = {
        "ALTER TABLE ai_signals ADD COLUMN risk_share REAL NOT NULL DEFAULT 0",
        "ALTER TABLE ai_signals ADD COLUMN horizon INTEGER NOT NULL DEFAULT 86400",
        "ALTER TABLE ai_signal_log ADD COLUMN horizon INTEGER NOT NULL DEFAULT 86400",
        nullptr,
    };
    for (int i = 0; alts[i]; i++) {
        char* aerr = nullptr;
        // «duplicate column name» — обычное дело: столбец уже на месте.
        sqlite3_exec(db, alts[i], nullptr, nullptr, &aerr);
        if (aerr) sqlite3_free(aerr);
    }
}

/* Записать выданные сигналы в журнал. Один и тот же сигнал висит в списке
   часами и переписывается каждые пять минут — заводить строку на каждую
   публикацию нельзя, иначе история наполнится копиями. Пока по монете и
   стороне есть незакрытая строка, новую не создаём. */
void logSignals(long long asOf, const std::vector<std::pair<Row, TradePlan>>& out,
                const std::vector<std::pair<int, bool>>& meta) {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return;
    sqlite3_stmt* find = nullptr;
    sqlite3_stmt* ins = nullptr;
    if (!prepareOrLog(db, &find,
            "SELECT 1 FROM ai_signal_log WHERE venue=? AND token=? AND side=? AND closed_at=0 LIMIT 1"))
        return;
    if (!prepareOrLog(db, &ins,
            "INSERT INTO ai_signal_log(made_at,venue,token,sym,side,conf,modelled,"
            "entry,stop,take1,take2,horizon) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)")) {
        sqlite3_finalize(find);
        return;
    }
    sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    for (size_t i = 0; i < out.size(); i++) {
        const Row& r = out[i].first;
        const TradePlan& p = out[i].second;
        const int side = r.score > 0 ? 1 : 0;
        sqlite3_reset(find);
        sqlite3_bind_int(find, 1, r.perp ? 1 : 0);
        sqlite3_bind_text(find, 2, r.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(find, 3, side);
        if (sqlite3_step(find) == SQLITE_ROW) continue;      // уже открыт
        sqlite3_reset(ins);
        sqlite3_bind_int64(ins, 1, asOf);
        sqlite3_bind_int(ins, 2, r.perp ? 1 : 0);
        sqlite3_bind_text(ins, 3, r.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, r.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 5, side);
        sqlite3_bind_int(ins, 6, meta[i].first);
        sqlite3_bind_int(ins, 7, meta[i].second ? 1 : 0);
        sqlite3_bind_double(ins, 8, p.entry);
        sqlite3_bind_double(ins, 9, p.stop);
        sqlite3_bind_double(ins, 10, p.take1);
        sqlite3_bind_double(ins, 11, p.take2);
        sqlite3_bind_int64(ins, 12, p.horizon > 0 ? p.horizon : AI_HORIZON_24H);
        sqlite3_step(ins);
    }
    sqlite3_finalize(find);
    sqlite3_finalize(ins);
    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
}

/* Чем кончился сигнал через сутки.
 *
 * Идём по часам от выдачи: что случилось раньше — цена дошла до цели или
 * свалилась на стоп. У перпов есть настоящие максимум и минимум часа, у
 * спота только цена закрытия часа, и это стоит помнить: по закрытиям касание
 * внутри часа не видно, и часть попаданий и стопов теряется.
 *
 * Если в одном часе задеты оба уровня, считаем стоп: предполагать, что успели
 * выйти в плюс, значит рисовать историю лучше, чем она была. */
struct Walked {
    bool done = false;
    int outcome = 0;
    double exitPx = 0;
    long long at = 0;
};

Walked walkOutcome(bool perp, const std::string& token, bool isLong, long long from,
                   long long to, double stop, double take) {
    Walked w;
    std::vector<std::tuple<long long, double, double, double>> bars;  // ts, close, high, low
    {
        std::lock_guard<std::mutex> lock(perp ? hl::g_hlDbMutex : dbMutex);
        sqlite3* h = perp ? hl::g_hlDb : db;
        if (!h) return w;
        sqlite3_stmt* s = nullptr;
        const char* q = perp
            ? "SELECT hour_ts, c, h, l FROM hl_candles WHERE coin=? AND hour_ts>=? AND hour_ts<=? "
              "ORDER BY hour_ts"
            : "SELECT ts, price_nanos, 0, 0 FROM token_price_history WHERE address=? AND ts>=? "
              "AND ts<=? ORDER BY ts";
        if (!prepareOrLog(h, &s, q)) return w;
        sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, from);
        sqlite3_bind_int64(s, 3, to);
        while (sqlite3_step(s) == SQLITE_ROW) {
            const long long ts = sqlite3_column_int64(s, 0);
            double c = perp ? sqlite3_column_double(s, 1)
                            : static_cast<double>(sqlite3_column_int64(s, 1)) / 1e9;
            double hi = perp ? sqlite3_column_double(s, 2) : c;
            double lo = perp ? sqlite3_column_double(s, 3) : c;
            if (c <= 0) continue;
            if (hi <= 0) hi = c;
            if (lo <= 0) lo = c;
            bars.emplace_back(ts, c, hi, lo);
        }
        sqlite3_finalize(s);
    }
    if (bars.empty()) return w;
    for (const auto& b : bars) {
        const double c = std::get<1>(b), hi = std::get<2>(b), lo = std::get<3>(b);
        const bool hitStop = isLong ? (lo <= stop) : (hi >= stop);
        const bool hitTake = isLong ? (hi >= take) : (lo <= take);
        if (hitStop) { w.done = true; w.outcome = -1; w.exitPx = stop; w.at = std::get<0>(b); return w; }
        if (hitTake) { w.done = true; w.outcome = 1; w.exitPx = take; w.at = std::get<0>(b); return w; }
        w.exitPx = c;
        w.at = std::get<0>(b);
    }
    w.done = true;      // сутки вышли, ни цель, ни стоп не задеты
    w.outcome = 0;
    return w;
}

void closeSignalLog() {
    const long long now = hl::nowSec();
    struct Open {
        long long id = 0, made = 0;
        int venue = 0, side = 0;
        std::string token;
        double entry = 0, stop = 0, take = 0;
        long long horizon = AI_HORIZON_24H;
    };
    std::vector<Open> open;
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) return;
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(db, &s,
                "SELECT id,made_at,venue,token,side,entry,stop,take1,horizon "
                "FROM ai_signal_log WHERE closed_at=0 AND made_at+horizon<=? LIMIT 200"))
            return;
        sqlite3_bind_int64(s, 1, now);
        while (sqlite3_step(s) == SQLITE_ROW) {
            Open o;
            o.id = sqlite3_column_int64(s, 0);
            o.made = sqlite3_column_int64(s, 1);
            o.venue = sqlite3_column_int(s, 2);
            o.token = safeColumnText(s, 3);
            o.side = sqlite3_column_int(s, 4);
            o.entry = sqlite3_column_double(s, 5);
            o.stop = sqlite3_column_double(s, 6);
            o.take = sqlite3_column_double(s, 7);
            o.horizon = sqlite3_column_int64(s, 8);
            if (o.horizon <= 0) o.horizon = AI_HORIZON_24H;
            open.push_back(std::move(o));
        }
        sqlite3_finalize(s);
    }
    if (open.empty()) return;
    int closed = 0;
    for (const Open& o : open) {
        if (o.entry <= 0) continue;
        const bool isLong = o.side == 1;
        const Walked w = walkOutcome(o.venue == 1, o.token, isLong, o.made,
                                     o.made + o.horizon, o.stop, o.take);
        if (!w.done || w.exitPx <= 0) continue;
        const double ret = (isLong ? (w.exitPx - o.entry) : (o.entry - w.exitPx)) / o.entry;
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) return;
        sqlite3_stmt* u = nullptr;
        if (!prepareOrLog(db, &u,
                "UPDATE ai_signal_log SET closed_at=?, exit_px=?, outcome=?, ret_bp=? WHERE id=?"))
            return;
        sqlite3_bind_int64(u, 1, w.at > 0 ? w.at : o.made + o.horizon);
        sqlite3_bind_double(u, 2, w.exitPx);
        sqlite3_bind_int(u, 3, w.outcome);
        sqlite3_bind_int(u, 4, static_cast<int>(std::llround(ret * 10000.0)));
        sqlite3_bind_int64(u, 5, o.id);
        if (sqlite3_step(u) == SQLITE_DONE) closed++;
        sqlite3_finalize(u);
    }
    if (closed > 0)
        std::cout << "[AI] история: закрыто сигналов " << closed << std::endl;
}

void publishSignals() {
    ensureSignalSchema();
    const long long asOf = hl::nowSec();
    auto rows = collectPassing(24, asOf);
    for (auto& r : rows) finish(r);
    std::vector<Row> spot, perp;
    for (auto& r : rows) {
        if (r.score == 0) continue;
        (r.perp ? perp : spot).push_back(std::move(r));
    }
    std::vector<Row> out;
    for (std::vector<Row>* side : {&spot, &perp}) {
        std::vector<Row> buys, avoids;
        takeSides(*side, buys, avoids);
        for (auto& r : buys) out.push_back(std::move(r));
        for (auto& r : avoids) out.push_back(std::move(r));
    }

    struct Ready {
        Row r;
        int conf = 0;
        bool modelled = false;
        TradePlan plan;
        std::string why;
    };
    std::vector<Ready> ready;
    ready.reserve(out.size());
    for (auto& r : out) {
        const bool wantLong = r.score > 0;
        Ready k;
        /* Уверенность и причина заполняются вместе с выбором горизонта ниже:
           у каждого горизонта своя вероятность и свои доводы. */
        bool trained = false;
        double acc = 0;
        {
            std::lock_guard<std::mutex> l(g_wMutex);
            trained = r.perp ? g_trainedPerp : g_trainedSpot;
            acc = r.perp ? g_accPerp : g_accSpot;
        }
        k.conf = confPct(r, trained, wantLong);
        const double live = static_cast<double>(livePriceOf(r)) / 1e9;
        const bool thin = !r.perp && r.liqUsd > 0 && r.liqUsd < 50000.0;

        // Горизонт, уровни и долю депозита выбирает оракул — тем же расчётом,
        // что уходит в сообщение бота.
        const Choice ch = choosePlan(r, wantLong, live, asOf, acc, thin, k.conf, true);
        k.plan = ch.plan;
        k.conf = ch.conf;
        k.modelled = ch.modelled;
        k.why = ch.why;
        if (!k.plan.valid) continue;
        k.r = std::move(r);
        ready.push_back(std::move(k));
    }

    {
        // Журнал выданного — до замены текущего списка: он про то, что было
        // показано, и переживает любую перезапись.
        std::vector<std::pair<Row, TradePlan>> rows;
        std::vector<std::pair<int, bool>> meta;
        rows.reserve(ready.size());
        meta.reserve(ready.size());
        for (const Ready& k : ready) {
            rows.emplace_back(k.r, k.plan);
            meta.emplace_back(k.conf, k.modelled);
        }
        logSignals(asOf, rows, meta);
    }
    closeSignalLog();

    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return;
    bool ok = sqlite3_exec(db, "DELETE FROM ai_signals", nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt* s = nullptr;
    if (ok && prepareOrLog(db, &s,
            "INSERT INTO ai_signals(made_at,venue,token,sym,side,conf,modelled,net_nanos,"
            "wallets,entry,stop,take1,take2,risk_pct,lev,risk_share,horizon,why) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")) {
        for (const Ready& k : ready) {
            sqlite3_reset(s);
            sqlite3_bind_int64(s, 1, asOf);
            sqlite3_bind_int(s, 2, k.r.perp ? 1 : 0);
            sqlite3_bind_text(s, 3, k.r.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, k.r.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 5, k.r.score > 0 ? 1 : 0);
            sqlite3_bind_int(s, 6, k.conf);
            sqlite3_bind_int(s, 7, k.modelled ? 1 : 0);
            sqlite3_bind_int64(s, 8, k.r.buy - k.r.sell);
            sqlite3_bind_int(s, 9, k.r.wallets);
            sqlite3_bind_double(s, 10, k.plan.entry);
            sqlite3_bind_double(s, 11, k.plan.stop);
            sqlite3_bind_double(s, 12, k.plan.take1);
            sqlite3_bind_double(s, 13, k.plan.take2);
            sqlite3_bind_double(s, 14, k.plan.riskPct);
            sqlite3_bind_int(s, 15, k.plan.leverage);
            sqlite3_bind_double(s, 16, k.plan.riskShare);
            sqlite3_bind_int64(s, 17, k.plan.horizon > 0 ? k.plan.horizon : AI_HORIZON_24H);
            sqlite3_bind_text(s, 18, k.why.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) != SQLITE_DONE) { ok = false; break; }
        }
        sqlite3_finalize(s);
    } else {
        ok = false;
    }
    if (!ok || sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "[AI] сигналы не записаны: " << sqlite3_errmsg(db) << std::endl;
        return;
    }
    /* Отметка «считал» — отдельно от самих сигналов.
     *
     * Пустая таблица не отличает «бот ни разу не считал» от «посчитал и
     * ничего не прошло отбор», а на экране это совсем разные вещи: первое —
     * повод перезапустить бота, второе — обычный тихий час. Поэтому время
     * последнего расчёта пишется всегда, даже когда выдавать нечего. */
    sqlite3_stmt* beat = nullptr;
    if (prepareOrLog(db, &beat, "INSERT OR REPLACE INTO ai_weights(k,v) VALUES(900,?)")) {
        sqlite3_bind_double(beat, 1, static_cast<double>(asOf));
        sqlite3_step(beat);
        sqlite3_finalize(beat);
    }
}

void snapshotHour() {
    ensureSchema();
    static long long lastSlot = 0;
    const long long slot = hl::nowSec() / 3600;
    if (slot == lastSlot) return;
    auto rows = collectPassing(24, hl::nowSec());
    recordRows(24, rows);
    lastSlot = slot;
}

}


bool aiHasAccess(const std::string& chatId) {
    if (chatId == OWNER_CHAT_ID) return true;
    ensureSchema();
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return false;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s, "SELECT 1 FROM ai_access WHERE chat_id=? LIMIT 1"))
        return false;
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return ok;
}

bool aiGrantAccess(const std::string& chatId) {
    ensureSchema();
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return false;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "INSERT OR IGNORE INTO ai_access(chat_id,granted_at) VALUES(?,?)"))
        return false;
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, hl::nowSec());
    const bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

bool aiRevokeAccess(const std::string& chatId) {
    ensureSchema();
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return false;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s, "DELETE FROM ai_access WHERE chat_id=?")) return false;
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(s) == SQLITE_DONE;
    const int changed = sqlite3_changes(db);
    sqlite3_finalize(s);
    return ok && changed > 0;
}

std::vector<std::string> aiAccessList() {
    std::vector<std::string> out;
    ensureSchema();
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return out;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s, "SELECT chat_id FROM ai_access ORDER BY granted_at"))
        return out;
    int rc = SQLITE_DONE;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        std::string c = safeColumnText(s, 0);
        if (!c.empty()) out.push_back(std::move(c));
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) out.clear();
    return out;
}

// История: что вышло из прошлых сигналов. Смотрим события с известным
// исходом, считаем, угадала ли модель направление.
struct HistItem {
    std::string id;
    std::string name;
    bool perp = false;
    bool wasLong = true;
    double retPct = 0;
    long long ts = 0;
    bool win = false;
    int plan = 0;      // 1 цель взята, -1 стоп выбит, 0 ни то ни другое
};

std::vector<HistItem> loadHistory(int venue, int limit) {
    std::vector<HistItem> out;
    ensureSchema();
    {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return out;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "SELECT name,venue,buy_nanos,sell_nanos,price_then,price_24h,ts,token "
            "FROM ai_events WHERE window_days=24 AND venue=? AND filled_at>0 "
            "AND price_then>0 AND price_24h>0 ORDER BY ts DESC LIMIT ?"))
        return out;
    sqlite3_bind_int(s, 1, venue);
    sqlite3_bind_int(s, 2, limit);
    int rc = SQLITE_DONE;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        HistItem h;
        h.name  = safeColumnText(s, 0);
        h.perp  = sqlite3_column_int(s, 1) != 0;
        const long long buy  = sqlite3_column_int64(s, 2);
        const long long sell = sqlite3_column_int64(s, 3);
        const long long then = sqlite3_column_int64(s, 4);
        const long long later = sqlite3_column_int64(s, 5);
        h.ts = sqlite3_column_int64(s, 6);
        h.id = safeColumnText(s, 7);
        if (buy == sell || then <= 0) continue;
        h.wasLong = buy > sell;
        h.retPct = 100.0 * static_cast<double>(later - then) / static_cast<double>(then);
        h.win = h.wasLong ? (h.retPct > 0) : (h.retPct < 0);
        out.push_back(std::move(h));
    }
    sqlite3_finalize(s);
    // Обрыв чтения дал бы неполную выборку, а доля угаданных
    // считается по ней и показывается человеку как факт.
    if (rc != SQLITE_DONE) out.clear();
    }

    // Волатильность читает базу и берёт тот же замок - считать план можно
    // только ПОСЛЕ его освобождения, иначе поток встанет намертво.
    for (auto& h : out) {
        const double vol = volatilityOf(h.id, h.perp);
        double stopPct = vol > 0 ? vol * 2.5 : 0.03;
        if (stopPct < 0.015) stopPct = 0.015;
        if (stopPct > 0.15)  stopPct = 0.15;
        const double moved = h.wasLong ? h.retPct : -h.retPct;
        if (moved >= stopPct * 1.5 * 100.0)  h.plan = 1;
        else if (moved <= -stopPct * 100.0)  h.plan = -1;
    }
    
    return out;
}

AiMessage buildAiHistory(const std::string& chatId, int venue) {
    ensureSchema();
    const Lang lang = langFromCode(getUserLanguage(chatId));
    if (venue != 1) venue = 0;

    std::ostringstream t;
    t << "\U0001F4DC <b>" << tr(lang, "ai_hist_title") << "</b> \u00B7 "
      << tr(lang, venue ? "ai_perp" : "ai_spot") << "\n\n";

    const auto items = loadHistory(venue, 20);
    if (items.empty()) {
        t << tr(lang, "ai_hist_empty") << "\n";
    } else {
        int wins = 0;
        double sum = 0;
        for (const auto& h : items) {
            if (h.win) wins++;
            sum += h.wasLong ? h.retPct : -h.retPct;
        }
        const int rate = static_cast<int>(100.0 * wins / items.size() + 0.5);
        int tp = 0, sl = 0;
        for (const auto& h : items) { if (h.plan == 1) tp++; else if (h.plan == -1) sl++; }

        t << tr(lang, "ai_hist_rate") << " <b>" << rate << "%</b> ("
          << wins << "/" << items.size() << ")\n"
          << tr(lang, "ai_hist_plans") << " <b>" << tp << "</b> "
          << tr(lang, "ai_hist_tp") << " \u00B7 <b>" << sl << "</b> "
          << tr(lang, "ai_hist_sl") << "\n"
          << tr(lang, "ai_hist_avg") << " <b>" << std::fixed << std::setprecision(1)
          << (sum / items.size()) << "%</b>\n\n";
        t.unsetf(std::ios::fixed);

        for (const auto& h : items) {
            const long long ago = (hl::nowSec() - h.ts) / 3600;
            t << (h.win ? "\u2705 " : "\u274C ")
              << (h.wasLong ? "\U0001F7E2 " : "\U0001F534 ")
              << "<b>" << h.name << "</b> "
              << std::fixed << std::setprecision(1)
              << (h.retPct >= 0 ? "+" : "") << h.retPct << "%"
              << "  <i>" << ago << tr(lang, "ai_hist_hours") << "</i>\n";
            t.unsetf(std::ios::fixed);
        }
    }

    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        json{{"text", std::string(venue == 0 ? "\u2022 " : "") + tr(lang, "ai_spot")}, {"callback_data", "ai_hist:s"}},
        json{{"text", std::string(venue == 1 ? "\u2022 " : "") + tr(lang, "ai_perp")}, {"callback_data", "ai_hist:p"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        json{{"text", tr(lang, "back_button")}, {"callback_data", "ai_open:24:s:b"}}
    }));
    return {t.str(), kb.dump()};
}

// Состояние модели: всё, что происходит внутри, одним экраном.
// Без этого обучение — чёрный ящик: непонятно, работает ли оно вообще.
AiMessage buildAiStatus(const std::string& chatId) {
    ensureSchema();
    const Lang lang = langFromCode(getUserLanguage(chatId));

    std::ostringstream t;
    t << "\U0001F9E0 <b>" << tr(lang, "ai_st_title") << "</b>\n\n";

    const char* fname[AI_NF] = {
        "bias", "flow", "wallets", "spread", "volume",
        "accel", "funding", "risk", "top wallets", "RSI", "liq skew"
    };

    for (int v = 0; v < 2; v++) {
        const bool perp = v == 1;
        bool trained;
        std::array<double, AI_NF> w{};
        {
            std::lock_guard<std::mutex> l(g_wMutex);
            trained = perp ? g_trainedPerp : g_trainedSpot;
            w = perp ? g_wPerp : g_wSpot;
        }
        const int ready = countReady(perp);
        const double acc = perp ? g_accPerp : g_accSpot;
        const double cal = perp ? g_calPerp : g_calSpot;
        const long long at = perp ? g_trainAtPerp : g_trainAtSpot;
        const long long ns = perp ? g_trainNPerp : g_trainNSpot;

        t << "<b>" << tr(lang, perp ? "ai_perp" : "ai_spot") << "</b>\n";
        t << tr(lang, "ai_st_ready") << " <b>" << ready << "</b> / " << AI_MIN_TRAIN << "\n";

        // Оракул старше линейной модели и показывается вместо неё. Числа —
        // с теста, которого он не видел при обучении, и рядом всегда стоит
        // то же число у постоянного прогноза: без него «потери 0.66» ничего
        // не значат.
        if (oracleReady(perp)) {
            const OracleStats os = oracleStats(perp);
            t << "🧠 <b>Cortex</b> \u00B7 " << os.trees << " " << tr(lang, "ai_st_trees")
              << " \u00B7 " << os.samples << " " << tr(lang, "ai_st_samples") << "\n";
            t << "AUC <b>" << std::fixed << std::setprecision(3) << os.auc << "</b>";
            t.unsetf(std::ios::fixed);
            t << " \u00B7 " << tr(lang, "ai_st_acc") << " <b>"
              << static_cast<int>(os.acc * 100.0 + 0.5) << "%</b>\n";
            t << tr(lang, "ai_st_loss") << " " << std::fixed << std::setprecision(3)
              << os.logloss << " \u00B7 " << tr(lang, "ai_st_base") << " " << os.baseLogloss << "\n";
            t << tr(lang, "ai_st_wf") << " AUC " << os.wfAuc << "\n";
            t.unsetf(std::ios::fixed);
            if (os.at > 0)
                t << "\u2699 " << (hl::nowSec() - os.at) / 3600 << tr(lang, "ai_hist_hours") << "\n";
            if (!os.top.empty()) {
                t << tr(lang, "ai_st_top") << " ";
                for (size_t k = 0; k < os.top.size() && k < 3; k++) {
                    if (k) t << " \u00B7 ";
                    t << os.top[k].first << " <code>"
                      << static_cast<int>(os.top[k].second * 100.0 + 0.5) << "%</code>";
                }
                t << "\n";
            }
            t << "\n";
            continue;
        }

        if (!trained) {
            t << tr(lang, "ai_st_untrained") << "\n\n";
            continue;
        }

        t << tr(lang, "ai_st_acc") << " <b>" << static_cast<int>(acc * 100.0 + 0.5) << "%</b>"
          << " \u00B7 " << tr(lang, "ai_st_samples") << " " << ns << "\n";
        t << tr(lang, "ai_st_cal") << " <b>" << std::fixed << std::setprecision(2) << cal << "</b>";
        t.unsetf(std::ios::fixed);
        if (at > 0) {
            const long long ago = (hl::nowSec() - at) / 3600;
            t << " \u00B7 " << ago << tr(lang, "ai_hist_hours");
        }
        t << "\n";

        // Три признака с наибольшим влиянием: видно, на что модель смотрит.
        std::vector<std::pair<double, int>> ranked;
        for (int i = 1; i < AI_NF; i++)
            ranked.emplace_back(std::fabs(w[static_cast<size_t>(i)]), i);
        std::sort(ranked.begin(), ranked.end(),
                  [](const std::pair<double,int>& a, const std::pair<double,int>& b) {
                      return a.first > b.first;
                  });
        t << tr(lang, "ai_st_top") << " ";
        for (int i = 0; i < 3 && i < static_cast<int>(ranked.size()); i++) {
            if (i) t << " \u00B7 ";
            const int idx = ranked[static_cast<size_t>(i)].second;
            const double val = w[static_cast<size_t>(idx)];
            t << fname[idx] << " <code>" << (val >= 0 ? "+" : "")
              << std::fixed << std::setprecision(2) << val << "</code>";
            t.unsetf(std::ios::fixed);
        }
        t << "\n\n";
    }

    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        json{{"text", tr(lang, "back_button")}, {"callback_data", "ai_open:24:s:b"}}
    }));
    return {t.str(), kb.dump()};
}

AiMessage buildAiSignals(const std::string& chatId, int days, int venue, int side) {
    days = clampDays(days);
    if (venue != 1) venue = 0;
    if (side != 1) side = 0;
    // Спот всегда лонг: шортить нечего. Старая ссылка на шорт по споту
    // не должна показать пустой экран.
    if (venue == 0) side = 0;
    ensureSchema();
    const Lang lang = langFromCode(getUserLanguage(chatId));
    const long long since = hl::nowSec() - static_cast<long long>(days) * 3600LL;
    auto aiCb = [](int d, int v, int s) {
        std::string out = "ai_open:" + std::to_string(d);
        out += v ? ":p" : ":s";
        out += s ? ":a" : ":b";
        return out;
    };

    json kbBase;
    kbBase["inline_keyboard"] = json::array();
    kbBase["inline_keyboard"].push_back(json::array({
        json{{"text", std::string(venue == 0 ? "\u2022 " : "") + tr(lang, "ai_spot")}, {"callback_data", aiCb(days, 0, side)}},
        json{{"text", std::string(venue == 1 ? "\u2022 " : "") + tr(lang, "ai_perp")}, {"callback_data", aiCb(days, 1, side)}}
    }));
    // На споте шорта нет: продать можно только то, что уже купил.
    // Кнопки выбора стороны показываем только на перпах.
    if (venue == 1) {
        kbBase["inline_keyboard"].push_back(json::array({
            json{{"text", std::string(side == 0 ? "\u2022 " : "") + "🟢 " + tr(lang, "ai_long")}, {"callback_data", aiCb(days, venue, 0)}},
            json{{"text", std::string(side == 1 ? "\u2022 " : "") + "🔴 " + tr(lang, "ai_short")}, {"callback_data", aiCb(days, venue, 1)}}
        }));
    }
    kbBase["inline_keyboard"].push_back(json::array({
        json{{"text", std::string(days == 1 ? "\u2022 " : "") + tr(lang, "ai_w1h")}, {"callback_data", aiCb(1, venue, side)}},
        json{{"text", std::string(days == 6 ? "\u2022 " : "") + tr(lang, "ai_w6h")}, {"callback_data", aiCb(6, venue, side)}},
        json{{"text", std::string(days == 24 ? "\u2022 " : "") + tr(lang, "ai_w24")}, {"callback_data", aiCb(24, venue, side)}}
    }));
    kbBase["inline_keyboard"].push_back(json::array({
        json{{"text", tr(lang, "ai_hist_btn")}, {"callback_data", venue ? "ai_hist:p" : "ai_hist:s"}},
        json{{"text", tr(lang, "ai_st_btn")}, {"callback_data", "ai_stat:x"}}
    }));
    kbBase["inline_keyboard"].push_back(json::array({
        json{{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));

    const int slot = cacheSlot(days);
    const int key = slot * 10000 + venue * 1000 + side * 100 + static_cast<int>(lang);
    {
        std::lock_guard<std::mutex> l(g_aiCacheMutex);
        auto it = g_aiCache.find(key);
        if (it != g_aiCache.end() &&
            time(nullptr) - it->second.at < AI_CACHE_TTL_SEC[slot])
            return {it->second.text, kbBase.dump()};
    }

    const auto topS = spotTopPnlWallets(AI_TOP_WALLETS);
    const auto topP = hlTopPnlWallets(AI_TOP_WALLETS);
    const long long asOf = hl::nowSec();
    auto spot = loadSpot(asOf, since, bannedSpot(), topS);
    auto perp = loadPerp(asOf, since, bannedHl(), topP);
    enrichPerpFunding(perp);
    markBothVenues(spot, perp);
    enrichIndicators(spot, asOf);
    enrichIndicators(perp, asOf);
    std::vector<Row> spotBuy, spotAvoid, perpBuy, perpAvoid;
    takeSides(spot, spotBuy, spotAvoid);
    takeSides(perp, perpBuy, perpAvoid);

    std::ostringstream t;
    t << "\U0001F916 <b>" << tr(lang, "ai_title") << "</b> · "
      << tr(lang, venue ? "ai_perp" : "ai_spot") << " · "
      << windowLabel(days, lang) << "\n\n";
    t << "<i>" << tr(lang, "ai_trade_hint") << "</i>\n";
    // Развёрнутое описание только на первом экране: дальше оно занимало бы
    // место, которое нужно самим сигналам.
    if (days == 24 && venue == 0 && side == 0)
        t << "<i>" << tr(lang, "ai_hint") << "</i>\n";
    // Точность на отложенных примерах: видно, стоит ли доверять модели.
    {
        const double acc = venue ? g_accPerp : g_accSpot;
        if (acc > 0)
            t << "<i>" << tr(lang, "ai_acc") << ": "
              << static_cast<int>(acc * 100.0 + 0.5) << "%</i>\n";
    }
    bool ts = false, tp = false;
    {
        const int ns = countReady(false);
        const int np = countReady(true);
        {
            std::lock_guard<std::mutex> l(g_wMutex);
            ts = g_trainedSpot;
            tp = g_trainedPerp;
        }
        t << "\n<code>";
        t << tr(lang, "ai_spot") << " " << (ts ? "✓" : std::to_string(ns) + "/" + std::to_string(AI_MIN_TRAIN));
        t << " · ";
        t << tr(lang, "ai_perp") << " " << (tp ? "✓" : std::to_string(np) + "/" + std::to_string(AI_MIN_TRAIN));
        t << "</code>\n";
        const bool trainedHere = venue ? tp : ts;
        t << "<i>" << tr(lang, trainedHere ? "ai_mode_model" : "ai_mode_formula") << "</i>\n";
    }

    const std::vector<Row>& rows = venue
        ? (side ? perpAvoid : perpBuy)
        : (side ? spotAvoid : spotBuy);
    const bool trainedHere = venue ? tp : ts;
    const bool wantLong = side == 0;
    if (rows.empty()) {
        t << "\n" << tr(lang, "ai_empty");
    } else {
        t << "\n";
        int shown = 0;
        for (const auto& r : rows) {
            if (shown >= AI_TRADE_N) break;
            const long long live = livePriceOf(r);
            if (!keepSignal(r, wantLong, live)) continue;
            if (shown) t << "\n";
            writeTrade(t, shown, r, lang, trainedHere, wantLong, live);
            shown++;
        }
        if (shown == 0) t << tr(lang, "ai_empty");
    }

    {
        std::lock_guard<std::mutex> l(g_aiCacheMutex);
        g_aiCache[key] = {time(nullptr), t.str()};
    }
    return {t.str(), kbBase.dump()};
}

bool handleAiHistoryCallback(const std::string& chatId, const std::string& param,
                            long long messageId) {
    const AiMessage m = buildAiHistory(chatId, param == "p" ? 1 : 0);
    if (messageId > 0) replyInPlace(chatId, messageId, m.text, m.keyboard);
    else sendMsg(chatId, m.text, m.keyboard);
    return true;
}

bool handleAiCallback(const std::string& chatId, const std::string& action,
                      const std::string& param, const std::string& data,
                      long long messageId, const std::string& callbackQueryId) {
    if (action != "ai_open" && action != "ai_hist" && action != "ai_stat") return false;
    if (!aiHasAccess(chatId)) return true;

    if (action == "ai_hist")
        return handleAiHistoryCallback(chatId, param, messageId);

    if (action == "ai_stat") {
        const AiMessage m = buildAiStatus(chatId);
        if (messageId > 0) replyInPlace(chatId, messageId, m.text, m.keyboard);
        else sendMsg(chatId, m.text, m.keyboard);
        return true;
    }

    int days = 24;
    int venue = 0;
    int side = 0;
    std::string rest = param;
    const size_t sep1 = rest.find(':');
    try {
        days = std::stoi(sep1 == std::string::npos ? rest : rest.substr(0, sep1));
    } catch (...) { days = 24; }
    days = clampDays(days);
    if (sep1 != std::string::npos) {
        rest = rest.substr(sep1 + 1);
        if (!rest.empty()) {
            if (rest[0] == 'p' || rest[0] == '1') venue = 1;
            const size_t sep2 = rest.find(':');
            if (sep2 != std::string::npos && sep2 + 1 < rest.size()) {
                const char s = rest[sep2 + 1];
                if (s == 'a' || s == '1') side = 1;
            }
        }
    }
    rememberView(chatId, "ai_open:" + std::to_string(days) + (venue ? ":p" : ":s") + (side ? ":a" : ":b"));
    auto msg = buildAiSignals(chatId, days, venue, side);
    replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    (void)data;
    (void)callbackQueryId;
    return true;
}

void aiTick() {
    // Оракул живёт своим расписанием: свечи каждые пять минут, ряды раз в
    // час, переобучение раз в шесть.
    oracleTick();
    backfillJournal();
    fillOutcomes();
    snapshotHour();
    {
        // Раз в пять минут: чаще незачем — поток китов за минуту не меняется,
        // а счёт по всем монетам стоит секунды.
        static long long last = 0;
        const long long now = hl::nowSec();
        if (now - last >= 300) { last = now; publishSignals(); }
    }
    trainWeights();
    static int n = 0;
    if (++n % 60 == 0) cleanupEvents();
}
