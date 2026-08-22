#include "ranking.h"

#include <sqlite3.h>
#include <mutex>
#include <map>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <atomic>
#include <thread>
#include <chrono>
#include "json.hpp"
#include "utils.h"
#include "ru.h"
#include "premium.h"
#include "wallet_menu.h"
#include "alert_settings.h"

std::string getUserLanguage(const std::string& chatId);

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;
extern std::atomic<bool> running;

namespace {
constexpr long long WINDOW_SECONDS = 30LL * 86400LL; // bot filter + default rank
constexpr int RANK_WINDOWS_DAYS[] = {30, 90, 180, 365};
constexpr int RANK_WINDOWS_COUNT = 4;
// пересчёт в разное время — пик нагрузки не складывается
// минуты «кривые», чтобы реже совпадать с тиком 15 мин и друг с другом
constexpr long long RANK_WINDOW_INTERVAL_SEC[] = {
    15 * 60,            // 30д  — 15 мин
    47 * 60,            // 90д  — 47 мин
    3 * 3600 + 11 * 60, // 180д — 3ч 11м
    6 * 3600 + 13 * 60, // 365д — 6ч 13м
};
long long g_rankWindowBuiltAt[4] = {0, 0, 0, 0};

int clampRankWindowDays(int days) {
    for (int d : RANK_WINDOWS_DAYS) if (d == days) return d;
    return 30;
}

int rankWindowIndex(int days) {
    days = clampRankWindowDays(days);
    for (int i = 0; i < RANK_WINDOWS_COUNT; i++)
        if (RANK_WINDOWS_DAYS[i] == days) return i;
    return 0;
}

std::string rankCacheKey(const std::string& kind, int days) {
    return "global_" + kind + "_" + std::to_string(clampRankWindowDays(days));
}
constexpr long long RETENTION_SECONDS = 365LL * 86400LL;
constexpr int MIN_GLOBAL_COMPLETED_TRADES = 5;
constexpr int MAX_GLOBAL_RANKED = 100;

constexpr int MAX_BOT_FILTER_TRADES = 1000;
constexpr int GLOBAL_PER_PAGE = 5;
constexpr long long REBUILD_INTERVAL_SECONDS = 15 * 60;

const char* const CARD_SEPARATOR = "━━━━━━━━━━━━━━━━━━━━━━━━━━━━";

const char* const MENU_STRETCH =
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀";

sqlite3* g_rankingReadDb = nullptr;
std::mutex g_rankingReadMutex;

bool safeParseAmount(const std::string& amountStr, const std::string& context, cpp_int& out) {
    if (amountStr.empty()) {
        std::cerr << "[RANKING] Empty token_amount (" << context << ")" << std::endl;
        return false;
    }
    try {
        out = cpp_int(amountStr);
    } catch (const std::exception& e) {
        std::cerr << "[RANKING] Invalid token_amount \"" << amountStr << "\" (" << context
                  << "): " << e.what() << std::endl;
        return false;
    }
    if (out <= 0) {
        std::cerr << "[RANKING] Non-positive token_amount \"" << amountStr << "\" (" << context << ")" << std::endl;
        return false;
    }
    return true;
}
}

bool parseGlobalRankKind(const std::string& s, GlobalRankKind& out) {
    if (s == "pnl") { out = GlobalRankKind::PNL; return true; }
    if (s == "roi") { out = GlobalRankKind::ROI; return true; }
    if (s == "winrate") { out = GlobalRankKind::WIN_RATE; return true; }
    if (s == "active") { out = GlobalRankKind::ACTIVE; return true; }
    return false;
}

std::string globalRankKindToString(GlobalRankKind k) {
    switch (k) {
        case GlobalRankKind::PNL: return "pnl";
        case GlobalRankKind::ROI: return "roi";
        case GlobalRankKind::WIN_RATE: return "winrate";
        case GlobalRankKind::ACTIVE: return "active";
    }
    return "pnl";
}

void initRankingDB() {
    std::lock_guard<std::mutex> l(dbMutex);
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS trades(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            wallet TEXT NOT NULL,
            token TEXT NOT NULL,
            is_buy INTEGER NOT NULL,
            usd_nanos INTEGER NOT NULL,
            token_amount TEXT NOT NULL,
            tx_hash TEXT UNIQUE,
            block_number INTEGER,
            timestamp INTEGER
        );
        CREATE TABLE IF NOT EXISTS wallet_history(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            wallet TEXT NOT NULL,
            token TEXT NOT NULL,
            is_buy INTEGER NOT NULL,
            usd_nanos INTEGER NOT NULL,
            token_amount TEXT NOT NULL,
            tx_hash TEXT,
            timestamp INTEGER
        );
        CREATE UNIQUE INDEX IF NOT EXISTS idx_wh_hash ON wallet_history(tx_hash, wallet, token, is_buy);
        CREATE INDEX IF NOT EXISTS idx_wh_lookup ON wallet_history(wallet, token, timestamp);
        CREATE INDEX IF NOT EXISTS idx_wh_time ON wallet_history(timestamp);
        DROP INDEX IF EXISTS idx_trades_wallet;
        CREATE INDEX IF NOT EXISTS idx_trades_wallet_time ON trades(wallet, timestamp);
        CREATE INDEX IF NOT EXISTS idx_trades_time ON trades(timestamp);
        DROP INDEX IF EXISTS idx_trades_token;
        CREATE INDEX IF NOT EXISTS idx_trades_token_time ON trades(token,timestamp);

        CREATE TABLE IF NOT EXISTS rank_presence (
            venue TEXT NOT NULL,
            wallet TEXT NOT NULL,
            day INTEGER NOT NULL,
            PRIMARY KEY(venue, wallet, day)
        );
        CREATE INDEX IF NOT EXISTS idx_presence_lookup ON rank_presence(venue, wallet);
        CREATE TABLE IF NOT EXISTS ignored_wallets(
            wallet TEXT PRIMARY KEY,
            ignored_until INTEGER NOT NULL,
            permanent INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS ranking_cache(
            cache_key TEXT PRIMARY KEY,
            payload TEXT NOT NULL,
            updated_at INTEGER NOT NULL
        );
    )";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[RANKING][FATAL] schema init failed: " << (err ? err : "") << std::endl;
        sqlite3_free(err);
    }

    {
        const char* alters[] = {
            "ALTER TABLE ignored_wallets ADD COLUMN permanent INTEGER NOT NULL DEFAULT 0",
        };
        for (const char* migSql : alters) {
            char* mErr = nullptr;
            if (sqlite3_exec(db, migSql, nullptr, nullptr, &mErr) != SQLITE_OK) {
                std::string e = mErr ? mErr : "";
                if (e.find("duplicate column") == std::string::npos)
                    std::cerr << "[RANKING][FATAL] migration failed: " << e << std::endl;
            }
            if (mErr) sqlite3_free(mErr);
        }
    }

    const char* fn = sqlite3_db_filename(db, "main");
    if (fn && *fn) {
        if (sqlite3_open_v2(fn, &g_rankingReadDb,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                            nullptr) == SQLITE_OK) {
            sqlite3_busy_timeout(g_rankingReadDb, 5000);
            std::cout << "[RANKING] Read-only connection opened" << std::endl;
        } else {
            std::cerr << "[RANKING] ⚠️ Read-only connection failed ("
                      << (g_rankingReadDb ? sqlite3_errmsg(g_rankingReadDb) : "open error")
                      << "), rankings will fall back to the shared connection" << std::endl;
            if (g_rankingReadDb) { sqlite3_close(g_rankingReadDb); g_rankingReadDb = nullptr; }
        }
    }
}

void closeRankingDB() {
    std::lock_guard<std::mutex> l(g_rankingReadMutex);
    if (g_rankingReadDb) {
        sqlite3_close(g_rankingReadDb);
        g_rankingReadDb = nullptr;
    }
}

void markRankPresence(const char* venue, const std::vector<std::string>& wallets) {
    if (wallets.empty()) return;
    const long long day = static_cast<long long>(time(nullptr)) / 86400;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "INSERT OR IGNORE INTO rank_presence(venue, wallet, day) VALUES(?,?,?)")) return;
    for (const auto& w : wallets) {
        sqlite3_bind_text(s, 1, venue, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, w.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, day);
        sqlite3_step(s);
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    sqlite3_finalize(s);
}

int rankPresenceDays(const char* venue, const std::string& wallet) {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT COUNT(*) FROM rank_presence WHERE venue=? AND wallet=?")) return 0;
    sqlite3_bind_text(s, 1, venue, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, wallet.c_str(), -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

void cleanupOldTrades() {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    {
        sqlite3_stmt* h;
        if (prepareOrLog(db, &h, "DELETE FROM wallet_history WHERE timestamp < ?")) {
            sqlite3_bind_int64(h, 1, static_cast<sqlite3_int64>(time(nullptr)) - RETENTION_SECONDS);
            sqlite3_step(h);
            sqlite3_finalize(h);
        }
    }
    if (!prepareOrLog(db, &s, "DELETE FROM trades WHERE timestamp < ?")) return;
    sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(time(nullptr)) - RETENTION_SECONDS);
    int rc1 = sqlite3_step(s);
    int deleted = (rc1 == SQLITE_DONE) ? sqlite3_changes(db) : 0;
    if (rc1 != SQLITE_DONE) std::cerr << "[RANKING] trades cleanup DELETE failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(s);
    if (deleted > 0) std::cout << "[RANKING] Purged " << deleted << " trade(s) older than 30 days" << std::endl;

    if (prepareOrLog(db, &s, "DELETE FROM ignored_wallets WHERE permanent = 0 AND ignored_until <= ?")) {
        sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(time(nullptr)));
        int rc2 = sqlite3_step(s);
        int unblocked = (rc2 == SQLITE_DONE) ? sqlite3_changes(db) : 0;
        if (rc2 != SQLITE_DONE) std::cerr << "[RANKING] ignored_wallets cleanup DELETE failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s);
        if (unblocked > 0) std::cout << "[RANKING] Unblocked " << unblocked << " wallet(s)" << std::endl;
    }
}

namespace {

int64_t cppIntToClampedI64(const cpp_int& v) {
    static const cpp_int maxV(INT64_MAX);
    static const cpp_int minV(INT64_MIN);
    if (v > maxV) return INT64_MAX;
    if (v < minV) return INT64_MIN;
    return v.convert_to<int64_t>();
}

std::string formatUsdSigned(int64_t usdNanos) { return formatUsdNanosSigned(usdNanos); }

const char* dirMark(Lang lang) {
    return lang == Lang::AR ? "\u200F" : "";
}

std::string rankLabel(int rank) {
    switch (rank) {
        case 1: return "🥇 #1";
        case 2: return "🥈 #2";
        case 3: return "🥉 #3";
        default: return "#" + std::to_string(rank);
    }
}

std::string formatPercentPlain(double pct) { return formatPercent(pct, false); }

struct PnlRow {
    std::string wallet;
    int64_t pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int completedTrades = 0;
    long long avgHoldSeconds = 0;
};

std::atomic<bool> g_forceRebuild{false};

std::string rowsToJson(const std::vector<PnlRow>& rows) {
    json a = json::array();
    for (const PnlRow& r : rows) {
        a.push_back({{"w", r.wallet}, {"p", r.pnlNanos}, {"r", r.roiPercent},
                     {"wr", r.winRatePercent}, {"t", r.completedTrades},
                     {"h", r.avgHoldSeconds}});
    }
    return a.dump();
}

bool rowsFromJson(const std::string& payload, std::vector<PnlRow>& out) {
    try {
        json a = json::parse(payload);
        if (!a.is_array()) return false;
        std::vector<PnlRow> tmp;
        tmp.reserve(a.size());
        for (auto& e : a) {
            PnlRow r;
            r.wallet = e.value("w", "");
            r.pnlNanos = e.value("p", static_cast<int64_t>(0));
            r.roiPercent = e.value("r", 0.0);
            r.winRatePercent = e.value("wr", 0);
            r.completedTrades = e.value("t", 0);
            r.avgHoldSeconds = e.value("h", static_cast<long long>(0));
            tmp.push_back(r);
        }
        out = std::move(tmp);
        return true;
    } catch (...) { return false; }
}

bool loadCachedPayload(const std::string& key, std::string& out) {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "SELECT payload FROM ranking_cache WHERE cache_key=?")) return false;
    sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    int stepRc = sqlite3_step(s);
    if (stepRc == SQLITE_ROW) { out = safeColumnText(s, 0); found = true; }
    else if (stepRc != SQLITE_DONE) {
        std::cerr << "[RANKING] loadCachedPayload(" << key << ") read error (not a genuine miss): "
                  << sqlite3_errmsg(db) << std::endl;
    }
    sqlite3_finalize(s);
    return found;
}

struct GlobalRankings {
    std::vector<PnlRow> byPnl;
    std::vector<PnlRow> byRoi;
    std::vector<PnlRow> byWinRate;
    std::vector<PnlRow> byActive;
};

std::vector<PnlRow> computeGlobalTopWindow(long long windowSeconds, bool& ok) {
    ok = false;
    std::vector<PnlRow> results;
    if (windowSeconds < 86400LL) windowSeconds = 86400LL;
    long long since = static_cast<long long>(time(nullptr)) - windowSeconds;

    std::string curWallet, curToken;
    cpp_int heldQty = 0, heldCost = 0;
    cpp_int outerPnl = 0, outerCostDeployed = 0;
    int outerCompleted = 0, outerWinning = 0;
    cpp_int heldTimeWeighted = 0;
    long long outerHoldSeconds = 0;

    auto flush = [&]() {
        if (!curWallet.empty() && outerCompleted >= MIN_GLOBAL_COMPLETED_TRADES &&
            outerCompleted <= MAX_BOT_FILTER_TRADES) {
            PnlRow row;
            row.wallet = curWallet;
            row.pnlNanos = cppIntToClampedI64(outerPnl);
            double volD = outerCostDeployed > 0 ? outerCostDeployed.convert_to<double>() : 0.0;
            double pnlD = outerPnl.convert_to<double>();

            row.roiPercent = volD > 0.0 ? (100.0 * pnlD / volD) : 0.0;
            row.winRatePercent = outerCompleted > 0
                ? static_cast<int>(100.0 * outerWinning / outerCompleted + 0.5)
                : 0;
            row.avgHoldSeconds = outerCompleted > 0 ? outerHoldSeconds / outerCompleted : 0;
            row.completedTrades = outerCompleted;
            results.push_back(row);
        }
    };

    sqlite3* rdb = g_rankingReadDb ? g_rankingReadDb : db;
    std::unique_lock<std::mutex> readLock, writeLock;
    if (g_rankingReadDb) readLock = std::unique_lock<std::mutex>(g_rankingReadMutex);
    else                 writeLock = std::unique_lock<std::mutex>(dbMutex);

    sqlite3_stmt* s;
    if (!prepareOrLog(rdb, &s,
        "SELECT t.wallet, t.token, t.is_buy, t.usd_nanos, t.token_amount, t.timestamp FROM trades t "
        "WHERE t.timestamp>=? "
        "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw WHERE iw.wallet=t.wallet AND iw.permanent=1) "
        "ORDER BY t.wallet ASC, t.token ASC, t.timestamp ASC, t.id ASC")) return results;
    sqlite3_bind_int64(s, 1, since);

    int stepRc;
    while ((stepRc = sqlite3_step(s)) == SQLITE_ROW) {
        std::string wallet = safeColumnText(s, 0);
        std::string token = safeColumnText(s, 1);
        bool isBuy = sqlite3_column_int(s, 2) != 0;
        int64_t usdNanos = sqlite3_column_int64(s, 3);
        std::string amountStr = safeColumnText(s, 4);
        long long tradeTs = sqlite3_column_int64(s, 5);
        cpp_int amount;
        if (!safeParseAmount(amountStr, "wallet=" + wallet + " token=" + token, amount)) {
            sqlite3_finalize(s);
            std::cerr << "[RANKING] computeGlobalTopWindow() aborted: corrupted token_amount" << std::endl;
            return results;
        }

        if (wallet != curWallet) {
            flush();
            curWallet = wallet;
            curToken.clear();
            outerPnl = 0; outerCostDeployed = 0; outerCompleted = 0; outerWinning = 0;
            outerHoldSeconds = 0;
        }
        if (token != curToken) {
            curToken = token;
            heldQty = 0; heldCost = 0; heldTimeWeighted = 0;
        }

        if (isBuy) {
            heldQty += amount;
            heldCost += cpp_int(usdNanos);
            heldTimeWeighted += amount * cpp_int(tradeTs);
        } else if (heldQty > 0 && amount > 0) {
            cpp_int matchedQty = amount < heldQty ? amount : heldQty;
            cpp_int costOfMatched = (heldCost * matchedQty) / heldQty;
            cpp_int timeOfMatched = (heldTimeWeighted * matchedQty) / heldQty;
            cpp_int proceedsMatched = (cpp_int(usdNanos) * matchedQty) / amount;

            outerPnl += (proceedsMatched - costOfMatched);
            outerCostDeployed += costOfMatched;
            outerCompleted++;
            if (proceedsMatched > costOfMatched) outerWinning++;

            cpp_int avgBuyTs = matchedQty > 0 ? (timeOfMatched / matchedQty) : cpp_int(tradeTs);
            long long held = tradeTs - avgBuyTs.convert_to<long long>();
            if (held > 0) outerHoldSeconds += held;

            heldQty -= matchedQty;
            heldCost -= costOfMatched;
            heldTimeWeighted -= timeOfMatched;
        }

    }
    sqlite3_finalize(s);
    if (stepRc != SQLITE_DONE) {
        std::cerr << "[RANKING] computeGlobalTopWindow() interrupted mid-read: "
                  << sqlite3_errmsg(rdb) << std::endl;
        return results;
    }
    ok = true;
    flush();
    return results;
}

GlobalRankings buildGlobalRankings(const std::vector<PnlRow>& base) {
    GlobalRankings out;

    out.byPnl = base;
    std::sort(out.byPnl.begin(), out.byPnl.end(),
        [](const PnlRow& a, const PnlRow& b) { return a.pnlNanos > b.pnlNanos; });
    if (out.byPnl.size() > static_cast<size_t>(MAX_GLOBAL_RANKED)) out.byPnl.resize(MAX_GLOBAL_RANKED);

    out.byRoi = base;
    std::sort(out.byRoi.begin(), out.byRoi.end(),
        [](const PnlRow& a, const PnlRow& b) { return a.roiPercent > b.roiPercent; });
    if (out.byRoi.size() > static_cast<size_t>(MAX_GLOBAL_RANKED)) out.byRoi.resize(MAX_GLOBAL_RANKED);

    out.byWinRate = base;
    std::sort(out.byWinRate.begin(), out.byWinRate.end(), [](const PnlRow& a, const PnlRow& b) {
        if (a.winRatePercent != b.winRatePercent) return a.winRatePercent > b.winRatePercent;
        return a.completedTrades > b.completedTrades;
    });
    if (out.byWinRate.size() > static_cast<size_t>(MAX_GLOBAL_RANKED)) out.byWinRate.resize(MAX_GLOBAL_RANKED);

    out.byActive = base;
    std::sort(out.byActive.begin(), out.byActive.end(), [](const PnlRow& a, const PnlRow& b) {
        if (a.completedTrades != b.completedTrades) return a.completedTrades > b.completedTrades;
        return a.pnlNanos > b.pnlNanos;
    });
    if (out.byActive.size() > static_cast<size_t>(MAX_GLOBAL_RANKED)) out.byActive.resize(MAX_GLOBAL_RANKED);

    return out;
}

std::string globalTitle(GlobalRankKind kind, Lang lang) {
    // rk_btn_* уже с эмодзи, без «(30д)» — срок только windowDays
    switch (kind) {
        case GlobalRankKind::PNL: return tr(lang, "rk_btn_top_pnl");
        case GlobalRankKind::ROI: return tr(lang, "rk_btn_top_roi");
        case GlobalRankKind::WIN_RATE: return tr(lang, "rk_btn_top_winrate");
        case GlobalRankKind::ACTIVE: return tr(lang, "rk_btn_most_active");
    }
    return "🏆 " + tr(lang, "rk_top_traders_30d");
}

RankingMessage renderGlobalPage(GlobalRankKind kind, const std::vector<PnlRow>& rows, int page,
                                int maxRank, bool showUpgrade, Lang lang, int windowDays) {
    windowDays = clampRankWindowDays(windowDays);
    if (maxRank < 1) maxRank = 1;
    int visible = std::min(static_cast<int>(rows.size()), maxRank);
    int totalPages = std::max(1, (visible + GLOBAL_PER_PAGE - 1) / GLOBAL_PER_PAGE);
    page = std::max(1, std::min(page, totalPages));
    int startIdx = (page - 1) * GLOBAL_PER_PAGE;
    int endIdx = std::min(visible, startIdx + GLOBAL_PER_PAGE);

    std::stringstream text;
    const char* const dm = dirMark(lang);

    text << dm << "🟡 <b>BSC \u2014 " << globalTitle(kind, lang) << "</b> · " << windowDays << tr(lang, "unit_day") << "\n\n";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    if (rows.empty()) {
        text << dm << "📊 " << tr(lang, "rk_no_completed_trades");
    } else {
        for (int i = startIdx; i < endIdx; i++) {
            const PnlRow& r = rows[i];
            int rank = i + 1;
            text << dm << rankLabel(rank) << "\n";
            text << dm << "<code>" << safeString(r.wallet, 42) << "</code>\n\n";
            text << dm << "💵 <b>PnL:</b> " << formatUsdSigned(r.pnlNanos) << "\n";
            text << dm << "📈 <b>" << tr(lang, "rk_roi_per_trade") << ":</b> " << formatPercentPlain(r.roiPercent) << "\n";
            text << dm << "🎯 <b>" << tr(lang, "ws_winrate") << ":</b> " << r.winRatePercent << "%\n";
            text << dm << "🔄 <b>" << tr(lang, "rk_trades") << ":</b> " << r.completedTrades << "\n";
            text << dm << "⏳ <b>" << tr(lang, "rk_avg_hold") << ":</b> " << formatHoldTime(r.avgHoldSeconds, lang) << "\n";
            if (int days = rankPresenceDays("spot", r.wallet); days > 0)
                text << dm << "\U0001F4C5 " << tr(lang, "rk_in_top") << ": <b>"
                     << days << "</b> " << tr(lang, "rk_days") << "\n";
            if (i + 1 < endIdx) text << "\n" << dm << CARD_SEPARATOR << "\n\n";

            json row;
            row.push_back({{"text", tr(lang, "rk_track") + " #" + std::to_string(rank)}, {"callback_data", "tt_track:" + r.wallet}});
            keyboard["inline_keyboard"].push_back(row);
        }
    }

    if (showUpgrade && !rows.empty()) {
        text << "\n" << dm << CARD_SEPARATOR << "\n";
        text << dm << tr(lang, "rk_unlock_top100");
        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
        }));
    }

    std::string kindParam = globalRankKindToString(kind);
    const std::string winSuffix = ":" + std::to_string(windowDays);
    json navRow = json::array();
    if (page > 1) navRow.push_back({{"text", "⬅️"}, {"callback_data", "gt_page:" + kindParam + ":" + std::to_string(page - 1) + winSuffix}});
    navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "tt_noop"}});
    if (page < totalPages) navRow.push_back({{"text", "➡️"}, {"callback_data", "gt_page:" + kindParam + ":" + std::to_string(page + 1) + winSuffix}});
    keyboard["inline_keyboard"].push_back(navRow);

    // после «Отслеживать» и навигации — окна, перед «Назад»
    json winRow = json::array();
    for (int d : RANK_WINDOWS_DAYS) {
        const bool on = (d == windowDays);
        const std::string label = on
            ? ("· " + std::to_string(d) + tr(lang, "unit_day") + " ·")
            : (std::to_string(d) + tr(lang, "unit_day"));
        winRow.push_back({{"text", label},
                          {"callback_data", "gt_open:" + kindParam + ":" + std::to_string(d)}});
    }
    keyboard["inline_keyboard"].push_back(winRow);

    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    return {text.str(), keyboard.dump()};
}

RankingMessage buildGeneratingMessage(Lang lang) {
    return {tr(lang, "rk_generating"), ""};
}

RankingMessage buildGlobalFromCache(GlobalRankKind kind, int page, int maxRank, bool showUpgrade, Lang lang,
                                    int windowDays = 30) {
    windowDays = clampRankWindowDays(windowDays);
    std::string payload;
    const std::string key = rankCacheKey(globalRankKindToString(kind), windowDays);
    if (!loadCachedPayload(key, payload)) {
        // окно ещё не считали (разнесённый график) — помечаем due, чтобы
        // следующий тик rankingCacheLoop взял именно его
        const int idx = rankWindowIndex(windowDays);
        g_rankWindowBuiltAt[idx] = 0;
        g_forceRebuild.store(true, std::memory_order_relaxed);
        return buildGeneratingMessage(lang);
    }
    std::vector<PnlRow> rows;
    if (!rowsFromJson(payload, rows)) return buildGeneratingMessage(lang);
    return renderGlobalPage(kind, rows, page, maxRank, showUpgrade, lang, windowDays);
}

void rebuildAllRankings() {
    auto t0 = std::chrono::steady_clock::now();
    const long long now = static_cast<long long>(time(nullptr));
    const bool force = g_forceRebuild.load(std::memory_order_relaxed);

    std::vector<std::pair<std::string, std::string>> entries;
    bool anyOk = false;

    // одно самое просроченное / ни разу не собранное окно.
    // force только будит цикл раньше сна — не гоняет все 4 окна сразу.
    std::vector<int> todo;
    int best = -1;
    double bestScore = -1.0;
    for (int i = 0; i < RANK_WINDOWS_COUNT; i++) {
        const long long interval = RANK_WINDOW_INTERVAL_SEC[i];
        const long long built = g_rankWindowBuiltAt[i];
        if (!force && built > 0 && (now - built) < interval) continue;
        // не собранное — приоритет коротким (30д), иначе % просрочки
        const double score = (built <= 0)
            ? (1000.0 - i)
            : static_cast<double>(now - built) / static_cast<double>(interval);
        if (score > bestScore) { bestScore = score; best = i; }
    }
    if (best >= 0) todo.push_back(best);

    for (int i : todo) {
        const int days = RANK_WINDOWS_DAYS[i];
        bool ok = false;
        std::vector<PnlRow> base = computeGlobalTopWindow(static_cast<long long>(days) * 86400LL, ok);
        if (!ok) {
            std::cerr << "[RANKING] cache rebuild: window " << days
                      << "d interrupted, keeping previous payload" << std::endl;
            continue;
        }
        anyOk = true;
        GlobalRankings g = buildGlobalRankings(base);
        if (days == 30) {
            std::vector<std::string> top;
            top.reserve(g.byPnl.size());
            for (const auto& r : g.byPnl) top.push_back(r.wallet);
            markRankPresence("spot", top);
        }
        entries.emplace_back(rankCacheKey("pnl", days), rowsToJson(g.byPnl));
        entries.emplace_back(rankCacheKey("roi", days), rowsToJson(g.byRoi));
        entries.emplace_back(rankCacheKey("winrate", days), rowsToJson(g.byWinRate));
        entries.emplace_back(rankCacheKey("active", days), rowsToJson(g.byActive));
        if (days == 30) {
            entries.emplace_back("global_pnl", rowsToJson(g.byPnl));
            entries.emplace_back("global_roi", rowsToJson(g.byRoi));
            entries.emplace_back("global_winrate", rowsToJson(g.byWinRate));
            entries.emplace_back("global_active", rowsToJson(g.byActive));
        }
        g_rankWindowBuiltAt[i] = now;
    }

    if (entries.empty()) {
        // нечего писать — все окна ещё свежие
        return;
    }
    if (!anyOk) {
        std::cerr << "[RANKING] cache rebuild aborted: no window completed" << std::endl;
        return;
    }

    {
        std::lock_guard<std::mutex> l(dbMutex);
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
            std::cerr << "[RANKING] cache rebuild: BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
            return;
        }
        sqlite3_stmt* s;
        if (!prepareOrLog(db, &s,
            "INSERT OR REPLACE INTO ranking_cache(cache_key,payload,updated_at) VALUES(?,?,?)")) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return;
        }
        bool insOk = true;
        for (const auto& e : entries) {
            sqlite3_reset(s);
            sqlite3_clear_bindings(s);
            sqlite3_bind_text(s, 1, e.first.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, e.second.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, now);
            if (sqlite3_step(s) != SQLITE_DONE) { insOk = false; break; }
        }
        sqlite3_finalize(s);
        if (!insOk) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "[RANKING] cache rebuild failed, rolled back" << std::endl;
            return;
        }
        char* commitErr = nullptr;
        if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, &commitErr) != SQLITE_OK) {
            std::cerr << "[RANKING] cache COMMIT failed: " << (commitErr ? commitErr : sqlite3_errmsg(db)) << std::endl;
            if (commitErr) sqlite3_free(commitErr);
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return;
        }
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "[RANKING] Cache rebuilt: " << entries.size() << " entrie(s), " << ms << "ms" << std::endl;
}

}

std::string formatHoldTime(long long seconds, Lang lang) {
    if (seconds <= 0) return "—";
    const std::string D = tr(lang, "unit_day"), H = tr(lang, "unit_hour"),
                      M = tr(lang, "unit_min"), S = tr(lang, "unit_sec");
    long long d = seconds / 86400;
    long long h = (seconds % 86400) / 3600;
    long long m = (seconds % 3600) / 60;
    if (d > 0) return h > 0 ? (std::to_string(d) + D + " " + std::to_string(h) + H)
                            : (std::to_string(d) + D);
    if (h > 0) return m > 0 ? (std::to_string(h) + H + " " + std::to_string(m) + M)
                            : (std::to_string(h) + H);
    if (m > 0) return std::to_string(m) + M;
    return std::to_string(seconds) + S;
}

bool isPermanentlyBanned(const std::string& wallet) {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "SELECT 1 FROM ignored_wallets WHERE wallet=? AND permanent=1")) return false;
    sqlite3_bind_text(s, 1, toLower(wallet).c_str(), -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return found;
}

bool liftPermanentBan(const std::string& wallet) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (!prepareOrLog(db, &s, "DELETE FROM ignored_wallets WHERE wallet=? AND permanent=1")) return false;
        sqlite3_bind_text(s, 1, toLower(wallet).c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_DONE) removed = sqlite3_changes(db) > 0;
        else std::cerr << "[RANKING] lift permanent ban failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s);
    }
    if (removed) {
        std::cout << "[RANKING] Permanent ban lifted for " << toLower(wallet) << std::endl;
        g_forceRebuild.store(true, std::memory_order_relaxed);
    }
    return removed;
}

namespace {

int countTrades30DLocked(const std::string& wallet, long long since) {
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT COUNT(*) FROM trades WHERE wallet=? AND timestamp>=?")) return 0;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, since);
    int n = 0;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

int countCompletedTrades30DLocked(const std::string& wallet, long long since, int stopAfter) {
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT token, is_buy, token_amount FROM trades "
        "WHERE wallet=? AND timestamp>=? ORDER BY token ASC, timestamp ASC, id ASC")) return 0;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, since);

    std::string curToken;
    cpp_int heldQty = 0;
    int completed = 0;
    int stepRc;
    bool stoppedEarly = false;
    while ((stepRc = sqlite3_step(s)) == SQLITE_ROW) {
        std::string token = safeColumnText(s, 0);
        bool isBuy = sqlite3_column_int(s, 1) != 0;
        std::string amountStr = safeColumnText(s, 2);
        cpp_int amount;
        if (!safeParseAmount(amountStr, "wallet=" + wallet + " token=" + token, amount)) amount = 0;

        if (token != curToken) {
            curToken = token;
            heldQty = 0;
        }
        if (isBuy) {
            heldQty += amount;
        } else if (heldQty > 0 && amount > 0) {
            completed++;
            heldQty -= (amount < heldQty ? amount : heldQty);
            if (completed > stopAfter) { stoppedEarly = true; break; }
        }
    }
    sqlite3_finalize(s);
    if (!stoppedEarly && stepRc != SQLITE_DONE) {
        std::cerr << "[RANKING] countCompletedTrades30DLocked(" << wallet << ") interrupted mid-read: "
                  << sqlite3_errmsg(db) << " (returning undercount " << completed << ")" << std::endl;
    }
    return completed;
}

}

void saveTrade(const std::string& walletArg, const TxResult& tx,
               const std::string& hash, long long block, long long blockTimestamp) {

    if (!tx.valid || !tx.isSwap) return;
    if (tx.usdNanos <= 0) return;
    if (tx.usdNanos > cpp_int("10000000000000000")) {
        std::cerr << "[RANKING] сделка с нереальной суммой отброшена: " << hash << std::endl;
        return;
    }
    if (tx.tokenAddr.empty()) return;
    if (tx.rawAmount <= 0) return;

    long long now = static_cast<long long>(time(nullptr));
    long long ts = blockTimestamp > 0 ? blockTimestamp : now;

    const std::string wallet = toLower(walletArg);
    std::string token = toLower(tx.tokenAddr);
    std::string amountStr = tx.rawAmount.convert_to<std::string>();
    int64_t usdNanos64 = cppIntToClampedI64(tx.usdNanos);

    bool blockedNow = false;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;

        bool ignored = false;
        bool permanent = false;
        long long ignoredUntil = 0;
        if (!prepareOrLog(db, &s, "SELECT ignored_until, permanent FROM ignored_wallets WHERE wallet=?")) return;
        sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            ignored = true;
            ignoredUntil = sqlite3_column_int64(s, 0);
            permanent = sqlite3_column_int(s, 1) != 0;
        }
        sqlite3_finalize(s);

        if (permanent) return;

        if (ignored) {
            if (ignoredUntil > now) return;
            if (prepareOrLog(db, &s, "DELETE FROM ignored_wallets WHERE wallet=?")) {
                sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(s);
                sqlite3_finalize(s);
                std::cout << "[RANKING] Block expired, wallet re-enabled: " << wallet << std::endl;
            }
        }

        if (!prepareOrLog(db, &s,
            "INSERT OR IGNORE INTO trades(wallet,token,is_buy,usd_nanos,token_amount,tx_hash,block_number,timestamp) "
            "VALUES(?,?,?,?,?,?,?,?)")) return;
        sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 3, tx.isBuy ? 1 : 0);
        sqlite3_bind_int64(s, 4, usdNanos64);
        sqlite3_bind_text(s, 5, amountStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 6, hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 7, block);
        sqlite3_bind_int64(s, 8, static_cast<sqlite3_int64>(ts));
        int insRc = sqlite3_step(s);
        bool inserted = false;
        if (insRc == SQLITE_DONE) {
            inserted = sqlite3_changes(db) == 1;
        } else {
            std::cerr << "[RANKING] trade insert failed: " << sqlite3_errmsg(db) << std::endl;
        }
        sqlite3_finalize(s);

        if (inserted && !tx.isBuy) {
            long long since = now - WINDOW_SECONDS;
            if (countTrades30DLocked(wallet, since) > MAX_BOT_FILTER_TRADES &&
                countCompletedTrades30DLocked(wallet, since, MAX_BOT_FILTER_TRADES) > MAX_BOT_FILTER_TRADES) {
                bool blockSaved = false;
                if (prepareOrLog(db, &s,
                    "INSERT OR REPLACE INTO ignored_wallets(wallet, ignored_until, permanent) VALUES(?, ?, 1)")) {
                    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(s, 2, now);
                    int blockRc = sqlite3_step(s);
                    if (blockRc == SQLITE_DONE) {
                        blockSaved = sqlite3_changes(db) == 1;
                    } else {
                        std::cerr << "[RANKING] permanent block failed: " << sqlite3_errmsg(db) << std::endl;
                    }
                    sqlite3_finalize(s);
                }
                if (blockSaved) {
                    blockedNow = true;
                    std::cout << "[RANKING] Bot detected: " << wallet
                              << " — permanently excluded from ranking (>"
                              << MAX_BOT_FILTER_TRADES << " trades/30d)" << std::endl;
                } else {
                    std::cerr << "[RANKING] Bot detected: " << wallet
                              << " but permanent block was NOT saved - will re-evaluate on next trade" << std::endl;
                }
            }
        }
    }

    if (blockedNow) {
        g_forceRebuild.store(true, std::memory_order_relaxed);
        untrackWalletFromService(wallet);
    }
}

void saveWalletHistory(const std::string& walletArg, const TxResult& tx,
                       const std::string& hash, long long blockTimestamp) {
    if (tx.usdNanos > cpp_int("10000000000000000")) return;
    if (!tx.isSwap || tx.tokenAddr.empty() || tx.usdNanos <= 0) return;

    const std::string wallet = toLower(walletArg);
    const std::string token = toLower(tx.tokenAddr);
    const long long ts = blockTimestamp > 0 ? blockTimestamp
                                            : static_cast<long long>(time(nullptr));

    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "INSERT OR IGNORE INTO wallet_history"
        "(wallet,token,is_buy,usd_nanos,token_amount,tx_hash,timestamp) "
        "VALUES(?,?,?,?,?,?,?)")) return;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 3, tx.isBuy ? 1 : 0);
    sqlite3_bind_int64(s, 4, static_cast<sqlite3_int64>(tx.usdNanos));
    const std::string amt = tx.rawAmount.convert_to<std::string>();
    sqlite3_bind_text(s, 5, amt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 6, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 7, ts);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

bool lastBuyOutcome(const std::string& walletArg, const std::string& tokenArg,
                    const std::string& currentHash, long long currentPriceNanos,
                    PriorBuy& out) {
    const std::string wallet = toLower(walletArg);
    const std::string token = toLower(tokenArg);
    const long long now = static_cast<long long>(time(nullptr));

    if (currentPriceNanos <= 0) return false;

    const int dec = getDecimals(token);

    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;

    if (!prepareOrLog(db, &s,
        "SELECT usd_nanos, token_amount, timestamp FROM wallet_history "
        "WHERE wallet=? AND token=? AND is_buy=1 AND (tx_hash IS NULL OR tx_hash<>?) "
        "ORDER BY timestamp DESC LIMIT 1")) return false;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, currentHash.c_str(), -1, SQLITE_TRANSIENT);

    long long usdNanos = 0, ts = 0;
    std::string amountStr;
    const bool found = sqlite3_step(s) == SQLITE_ROW;
    if (found) {
        usdNanos = sqlite3_column_int64(s, 0);
        amountStr = safeColumnText(s, 1);
        ts = sqlite3_column_int64(s, 2);
    }
    sqlite3_finalize(s);

    {
        sqlite3_stmt* h;
        if (prepareOrLog(db, &h,
            "SELECT is_buy, usd_nanos, token_amount, tx_hash FROM wallet_history "
            "WHERE wallet=? AND token=? ORDER BY timestamp ASC, id ASC")) {
            sqlite3_bind_text(h, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(h, 2, token.c_str(), -1, SQLITE_TRANSIENT);
            cpp_int pos = 0, cost = 0;
            int buys = 0;
            while (sqlite3_step(h) == SQLITE_ROW) {
                const bool isBuy = sqlite3_column_int(h, 0) != 0;
                const long long u = sqlite3_column_int64(h, 1);
                const std::string a = safeColumnText(h, 2);
                const std::string rowHash = safeColumnText(h, 3);
                if (!currentHash.empty() && rowHash == currentHash) continue;
                cpp_int amt = 0;
                try { amt = cpp_int(a); } catch (...) { continue; }
                if (amt <= 0 || u < 0) continue;
                if (isBuy) { pos += amt; cost += cpp_int(u); buys++; continue; }
                if (pos <= 0) continue;
                const cpp_int take = amt < pos ? amt : pos;
                cost -= cost * take / pos;
                pos -= take;
            }
            sqlite3_finalize(h);
            if (pos > 0 && cost > 0) {
                const cpp_int avgBig = calcUnitPriceNanos(cost, pos, dec);
                if (avgBig > 0 && avgBig <= cpp_int("1000000000000000"))
                    out.avgEntryNanos = static_cast<long long>(avgBig);
            }
            out.buyCount = buys;
        }
    }
    if (!found || usdNanos <= 0 || ts <= 0) return false;

    cpp_int amount = 0;
    try { amount = cpp_int(amountStr); } catch (...) { return false; }
    if (amount <= 0) return false;

    const cpp_int thenPxBig = calcUnitPriceNanos(cpp_int(usdNanos), amount, dec);
    if (thenPxBig <= 0) return false;
    const long long thenPx = static_cast<long long>(thenPxBig);

    out.thenPriceNanos = thenPx;
    out.nowPriceNanos = currentPriceNanos;
    out.ageSeconds = now - ts;
    out.changePercent = 100.0 * (static_cast<double>(currentPriceNanos) - static_cast<double>(thenPx))
                              / static_cast<double>(thenPx);
    return true;
}

bool sellOutcome(const std::string& walletArg, const std::string& tokenArg,
                 long long sellUsdNanos, const std::string& sellAmountStr,
                 const std::string& currentHash, SellPnl& out) {
    const std::string wallet = toLower(walletArg);
    const std::string token = toLower(tokenArg);
    if (sellUsdNanos <= 0) return false;

    cpp_int selling = 0;
    try { selling = cpp_int(sellAmountStr); } catch (...) { return false; }
    if (selling <= 0) return false;

    const int dec = getDecimals(token);

    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;

    if (!prepareOrLog(db, &s,
        "SELECT is_buy, usd_nanos, token_amount, tx_hash FROM wallet_history "
        "WHERE wallet=? AND token=? ORDER BY timestamp ASC, id ASC")) return false;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, token.c_str(), -1, SQLITE_TRANSIENT);

    cpp_int position = 0;
    cpp_int costBasis = 0;
    int buys = 0;

    while (sqlite3_step(s) == SQLITE_ROW) {
        const bool isBuy = sqlite3_column_int(s, 0) != 0;
        const long long usd = sqlite3_column_int64(s, 1);
        const std::string amtStr = safeColumnText(s, 2);
        const std::string rowHash = safeColumnText(s, 3);
        cpp_int amt = 0;
        try { amt = cpp_int(amtStr); } catch (...) { continue; }
        if (amt <= 0 || usd < 0) continue;

        if (isBuy) {
            position += amt;
            costBasis += cpp_int(usd);
            buys++;
            continue;
        }

        if (!currentHash.empty() && rowHash == currentHash) continue;

        if (position <= 0) continue;
        const cpp_int take = amt < position ? amt : position;
        costBasis -= costBasis * take / position;
        position -= take;
    }
    sqlite3_finalize(s);

    if (buys == 0 || position <= 0 || costBasis <= 0) return false;

    const cpp_int take = selling < position ? selling : position;
    const cpp_int costOfSold = costBasis * take / position;
    if (costOfSold <= 0) return false;

    const cpp_int avgBig = calcUnitPriceNanos(costOfSold, take, dec);
    if (avgBig > 0 && avgBig <= cpp_int("1000000000000000"))
        out.avgEntryNanos = static_cast<long long>(avgBig);

    const cpp_int pnl = cpp_int(sellUsdNanos) - costOfSold;
    out.pnlNanos = static_cast<long long>(pnl);
    out.costNanos = static_cast<long long>(costOfSold);
    out.buyCount = buys;
    out.pnlPercent = 100.0 * static_cast<double>(out.pnlNanos)
                           / static_cast<double>(out.costNanos);
    return true;
}

RankingMessage buildGlobalTopMenu(const std::string& chatId) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "rk_btn_top_pnl")}, {"callback_data", "gt_open:pnl"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "rk_btn_top_roi")}, {"callback_data", "gt_open:roi"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "rk_btn_top_winrate")}, {"callback_data", "gt_open:winrate"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "rk_btn_most_active")}, {"callback_data", "gt_open:active"}}
    }));

    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    return {std::string("🟡 <b>BSC \u2014 " + tr(lang, "rk_top_traders_30d") + "</b>\n")
            + "<code>" + MENU_STRETCH + "</code>"
            + "\n" + tr(lang, "rk_choose_ranking"), keyboard.dump()};
}

RankingMessage buildGlobalTopMessage(const std::string& chatId, GlobalRankKind kind,
                                     int maxRank, bool showUpgrade) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    return buildGlobalFromCache(kind, 1, maxRank, showUpgrade, lang);
}

RankingMessage buildGlobalTopPage(const std::string& chatId, GlobalRankKind kind, int page,
                                  int maxRank, bool showUpgrade) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    return buildGlobalFromCache(kind, page, maxRank, showUpgrade, lang);
}

bool spotRankOf(const std::string& wallet, SpotRankInfo& out) {
    std::string payload;
    if (!loadCachedPayload("global_pnl", payload)) return false;
    std::vector<PnlRow> rows;
    if (!rowsFromJson(payload, rows) || rows.empty()) return false;

    const std::string w = toLower(wallet);
    for (size_t i = 0; i < rows.size(); i++) {
        if (toLower(rows[i].wallet) != w) continue;
        out.rank = static_cast<int>(i) + 1;
        out.total = static_cast<int>(rows.size());
        out.pnlNanos = rows[i].pnlNanos;
        out.roiPercent = rows[i].roiPercent;
        out.winRatePercent = rows[i].winRatePercent;
        out.completedTrades = rows[i].completedTrades;
        return true;
    }
    return false;
}

void rankingCacheLoop() {
    while (running.load(std::memory_order_relaxed)) {
        try {
            rebuildAllRankings();
        } catch (const std::exception& e) {
            std::cerr << "[RANKING] rebuild threw: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[RANKING] rebuild threw unknown exception" << std::endl;
        }
        for (long long slept = 0;
             running.load(std::memory_order_relaxed) && slept < REBUILD_INTERVAL_SECONDS;
             slept++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (g_forceRebuild.exchange(false)) break;
        }
    }
}

bool handleRankingCallback(const std::string& chatId, const std::string& action,
                           const std::string& param, const std::string& data,
                           long long messageId, const std::string& callbackQueryId) {
    if (action == "tt_track") {
        std::string address = toLower(param);
        Lang trackLang = langFromCode(getUserLanguage(chatId));
        if (!isValidAddress(address)) {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, tr(trackLang, "toast_invalid_address"), true);
        }
        else if (isTrackingWallet(chatId, address)) {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, tr(trackLang, "toast_already_tracking"), true);
        }
        else if (chatId != SERVICE_CHAT_ID && countUserWhales(chatId) >= premiumMaxWallets(chatId)) {
            std::string feedback = isPremium(chatId)
                ? tr(trackLang, "wallet_limit_50_short")
                : tr(trackLang, "free_plan_1_wallet");
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, feedback, true);
        }
        else {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId);
            g_sessionManager.setState(chatId, UserState::AWAITING_TRACK_NAME, address, messageId);
            replyInPlace(chatId, messageId, tr(trackLang, "track_name_prompt"), TelegramUI::buildCancelButton(trackLang));
        }
    }
    else if (action == "tt_noop") {
        if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId);
    }
    else if (action == "gt_open") {
        // gt_open:pnl  |  gt_open:pnl:90
        std::string kindStr = param;
        int windowDays = 30;
        const size_t sep = param.find(':');
        if (sep != std::string::npos) {
            kindStr = param.substr(0, sep);
            try { windowDays = std::stoi(param.substr(sep + 1)); } catch (...) {}
        }
        GlobalRankKind kind;
        if (parseGlobalRankKind(kindStr, kind)) {
            rememberView(chatId, data);
            Lang lang = langFromCode(getUserLanguage(chatId));
            auto msg = buildGlobalFromCache(kind, 1,
                                            premiumTopTradersLimit(chatId),
                                            !isPremium(chatId), lang, windowDays);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
    }
    else if (action == "gt_page") {
        // gt_page:pnl:2  |  gt_page:pnl:2:90
        size_t sep = param.find(':');
        if (sep != std::string::npos) {
            std::string kindStr = param.substr(0, sep);
            std::string rest = param.substr(sep + 1);
            int page = 1;
            int windowDays = 30;
            size_t sep2 = rest.find(':');
            try {
                if (sep2 == std::string::npos) page = std::stoi(rest);
                else {
                    page = std::stoi(rest.substr(0, sep2));
                    windowDays = std::stoi(rest.substr(sep2 + 1));
                }
            } catch (...) {}
            GlobalRankKind kind;
            if (parseGlobalRankKind(kindStr, kind)) {
                rememberView(chatId, data);
                Lang lang = langFromCode(getUserLanguage(chatId));
                auto msg = buildGlobalFromCache(kind, page,
                                                premiumTopTradersLimit(chatId),
                                                !isPremium(chatId), lang, windowDays);
                replyInPlace(chatId, messageId, msg.text, msg.keyboard);
            }
        }
    }
    else return false;
    return true;
}
