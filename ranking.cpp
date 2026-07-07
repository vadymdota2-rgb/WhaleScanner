#include "ranking.h"

#include <sqlite3.h>
#include <mutex>
#include <map>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include "json.hpp"

using json = nlohmann::json;

// ---- Shared state / helpers from main.cpp ----
// These are ordinary (non-static) globals and free functions defined in
// main.cpp; declaring them here again gives this translation unit the
// symbols it needs without touching main.cpp's own definitions.
extern sqlite3* db;
extern std::mutex dbMutex;

std::string toLower(std::string s);
std::string trim(const std::string& s);
bool prepareOrLog(sqlite3* db, sqlite3_stmt** stmt, const char* sql);
std::string safeColumnText(sqlite3_stmt* stmt, int col);
bool isValidAddress(const std::string& a);
std::string safeString(const std::string& s, size_t maxLen);

namespace {
constexpr long long WINDOW_SECONDS = 30LL * 86400LL;   // 30-day rolling window
constexpr int MIN_COMPLETED_TRADES = 5;                // per-token ranking entry threshold
constexpr int MAX_RANKED_WALLETS = 20;                 // per-token ranking size
constexpr int MIN_GLOBAL_COMPLETED_TRADES = 1;         // global ranking entry threshold
constexpr int MAX_GLOBAL_RANKED = 100;                 // global ranking size (Top 100)
// Wallets with more than this many completed trades in the 30-day window
// are almost certainly trading bots rather than human "traders" — excluded
// from every ranking (per-token and global alike) so they don't crowd out
// real traders at the top of the leaderboard.
constexpr int MAX_BOT_FILTER_TRADES = 500;
constexpr int PER_PAGE = 5;
constexpr time_t CACHE_TTL_SECONDS = 15 * 60;          // how long a computed ranking stays pageable
// Visual divider between cards on a page. A full-width run of U+2501 also
// nudges Telegram Android into laying the message (and thus the inline
// keyboard) out at full width instead of its collapsed minimum width.
const char* const CARD_SEPARATOR = "━━━━━━━━━━━━━━";
// Width "stretcher" for menu messages that have no other wide content.
// Rendered inside <code> so it's monospace — Telegram never shrinks a
// monospace run below its natural width, which is what makes the ranking
// cards (with their <code> wallet address) render near-full-width.
// U+2800 (BRAILLE PATTERN BLANK) is used instead of a visible line: it is
// NOT whitespace, so Telegram won't trim it, yet it renders as empty space —
// the bubble stretches with no ugly ruled line in the menu.
// Length 30: on a typical phone ~36 monospace chars fit per line (see how
// the 42-char wallet address wraps), so 30 stays on ONE line with margin
// while still pushing the bubble to (near) full width. Don't raise past
// ~34 or it will wrap again on narrow screens.
const char* const MENU_STRETCH =
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀"; // 30 × U+2800

// Dedicated READ-ONLY SQLite connection for the heavy full-table ranking
// scans. With WAL enabled (initDB() sets it), a reader on its own
// connection never blocks writers on the main connection and vice versa —
// so computing a ranking no longer stalls the whole bot behind dbMutex.
// Opened in initRankingDB(); if opening fails, code falls back to the
// shared `db` + dbMutex, which is correct but slower.
sqlite3* g_rankingReadDb = nullptr;
std::mutex g_rankingReadMutex;
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

// ==================== SCHEMA ====================
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
        CREATE INDEX IF NOT EXISTS idx_trades_token ON trades(token);
        CREATE INDEX IF NOT EXISTS idx_trades_wallet ON trades(wallet);
        CREATE INDEX IF NOT EXISTS idx_trades_time ON trades(timestamp);
        CREATE INDEX IF NOT EXISTS idx_trades_token_time ON trades(token,timestamp);
    )";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[RANKING][FATAL] schema init failed: " << (err ? err : "") << std::endl;
        sqlite3_free(err);
    }

    // Second, read-only connection to the same file for ranking scans.
    // Requires WAL (already set in initDB()); without WAL a long read on a
    // second connection would make writers fail with SQLITE_BUSY instead.
    // sqlite3_db_filename() is used instead of extern-ing DB_FILE because
    // `const std::string DB_FILE` in main.cpp has internal linkage.
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

// ==================== ROLLING 30-DAY RETENTION ====================
void cleanupOldTrades() {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "DELETE FROM trades WHERE timestamp < ?")) return;
    sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(time(nullptr)) - WINDOW_SECONDS);
    sqlite3_step(s);
    int deleted = sqlite3_changes(db);
    sqlite3_finalize(s);
    if (deleted > 0) std::cout << "[RANKING] Purged " << deleted << " trade(s) older than 30 days" << std::endl;
}

namespace {

// Clamp a cpp_int (which can in principle exceed 64 bits) down to a value
// that fits in an int64_t, instead of invoking UB on convert_to<int64_t>()
// overflow. Realistic USD*1e9 amounts never get near this ceiling.
int64_t cppIntToClampedI64(const cpp_int& v) {
    static const cpp_int maxV(INT64_MAX);
    static const cpp_int minV(INT64_MIN);
    if (v > maxV) return INT64_MAX;
    if (v < minV) return INT64_MIN;
    return v.convert_to<int64_t>();
}

// Formats whole-dollar amounts with thousands separators, e.g. 18432000000000
// (usd_nanos) -> "+$18,432". usdNanos follows the bot-wide convention of
// USD * 1e9, truncated (not rounded) down to whole dollars.
std::string formatUsdSigned(int64_t usdNanos) {
    bool neg = usdNanos < 0;
    int64_t dollars = usdNanos / 1000000000LL;
    if (neg) dollars = -dollars;
    std::string s = std::to_string(dollars);
    std::string out;
    int cnt = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (cnt != 0 && cnt % 3 == 0) out.push_back(',');
        out.push_back(*it);
        cnt++;
    }
    std::reverse(out.begin(), out.end());
    return (neg ? "-$" : "+$") + out;
}

std::string formatPercentSigned(double pct) {
    long long rounded = static_cast<long long>(pct >= 0 ? pct + 0.5 : pct - 0.5);
    std::string sign = rounded >= 0 ? "+" : "-";
    return sign + std::to_string(rounded < 0 ? -rounded : rounded) + "%";
}

std::string rankLabel(int rank) {
    switch (rank) {
        case 1: return "🥇 #1";
        case 2: return "🥈 #2";
        case 3: return "🥉 #3";
        default: return "#" + std::to_string(rank);
    }
}

// Percentage without a forced leading "+" (e.g. "148%" / "-32%"), used for
// ROI in the global ranking cards, per that feature's spec'd format.
std::string formatPercentPlain(double pct) {
    long long rounded = static_cast<long long>(pct >= 0 ? pct + 0.5 : pct - 0.5);
    return (rounded < 0 ? "-" : "") + std::to_string(rounded < 0 ? -rounded : rounded) + "%";
}

// ==================== PNL MODEL ====================
struct PnlRow {
    std::string wallet;
    int64_t pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int completedTrades = 0;
};

// Walks every (wallet, token) trade in the 30-day window, ordered by wallet
// then time, and derives Realized PnL per wallet using the Average Cost
// Basis method:
//   - a BUY adds to the held quantity and to the cost basis (in USD) of the
//     open position;
//   - a SELL is matched against whatever quantity/cost basis is currently
//     held. If the wallet sells more than the window can account for (i.e.
//     tokens bought more than 30 days ago), only the portion backed by a
//     known cost basis counts as a "completed" trade — the rest is ignored
//     rather than guessed at.
// Everything is done with a single ordered SQL query and local arithmetic;
// no extra RPC calls, no per-wallet queries.
std::vector<PnlRow> computeTopPnl(const std::string& token) {
    std::vector<PnlRow> results;
    long long since = static_cast<long long>(time(nullptr)) - WINDOW_SECONDS;

    std::string curWallet;
    cpp_int heldQty = 0;
    cpp_int heldCost = 0;          // USD*1e9 cost basis of the currently held position
    cpp_int realizedPnl = 0;       // USD*1e9
    cpp_int totalCostDeployed = 0; // USD*1e9, sum of cost basis consumed by completed sells
    int completedTrades = 0;
    int winningTrades = 0;

    auto flush = [&]() {
        if (!curWallet.empty() && completedTrades >= MIN_COMPLETED_TRADES &&
            completedTrades <= MAX_BOT_FILTER_TRADES) {
            PnlRow row;
            row.wallet = curWallet;
            row.pnlNanos = cppIntToClampedI64(realizedPnl);
            double costD = totalCostDeployed > 0 ? totalCostDeployed.convert_to<double>() : 0.0;
            double pnlD = realizedPnl.convert_to<double>();
            row.roiPercent = costD > 0.0 ? (100.0 * pnlD / costD) : 0.0;
            row.winRatePercent = completedTrades > 0
                ? static_cast<int>(100.0 * winningTrades / completedTrades + 0.5)
                : 0;
            row.completedTrades = completedTrades;
            results.push_back(row);
        }
    };

    // Prefer the dedicated read-only connection: this is a full scan of the
    // token's trades and must not hold dbMutex for its whole duration (the
    // rest of the bot — block processing, alerts — would stall behind it).
    // g_rankingReadDb is set once at startup and cleared once at shutdown,
    // so reading the pointer here without a lock is fine.
    sqlite3* rdb = g_rankingReadDb ? g_rankingReadDb : db;
    std::unique_lock<std::mutex> readLock, writeLock;
    if (g_rankingReadDb) readLock = std::unique_lock<std::mutex>(g_rankingReadMutex);
    else                 writeLock = std::unique_lock<std::mutex>(dbMutex);

    sqlite3_stmt* s;
    if (!prepareOrLog(rdb, &s,
        "SELECT wallet, is_buy, usd_nanos, token_amount FROM trades "
        "WHERE token=? AND timestamp>=? ORDER BY wallet ASC, timestamp ASC")) return results;
    sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, since);

    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string wallet = safeColumnText(s, 0);
        bool isBuy = sqlite3_column_int(s, 1) != 0;
        int64_t usdNanos = sqlite3_column_int64(s, 2);
        std::string amountStr = safeColumnText(s, 3);
        cpp_int amount = amountStr.empty() ? cpp_int(0) : cpp_int(amountStr);

        if (wallet != curWallet) {
            flush();
            curWallet = wallet;
            heldQty = 0; heldCost = 0; realizedPnl = 0; totalCostDeployed = 0;
            completedTrades = 0; winningTrades = 0;
        }

        if (isBuy) {
            heldQty += amount;
            heldCost += cpp_int(usdNanos);
        } else if (heldQty > 0 && amount > 0) {
            cpp_int matchedQty = amount < heldQty ? amount : heldQty;
            cpp_int costOfMatched = (heldCost * matchedQty) / heldQty;
            cpp_int proceedsMatched = (cpp_int(usdNanos) * matchedQty) / amount;

            realizedPnl += (proceedsMatched - costOfMatched);
            totalCostDeployed += costOfMatched;
            completedTrades++;
            if (proceedsMatched > costOfMatched) winningTrades++;

            heldQty -= matchedQty;
            heldCost -= costOfMatched;
        }
        // A sell with heldQty<=0 (nothing known to have been bought within
        // the window) has no knowable cost basis, so it's skipped entirely —
        // it neither counts as a completed trade nor moves realizedPnl.
    }
    sqlite3_finalize(s);
    flush();

    std::sort(results.begin(), results.end(), [](const PnlRow& a, const PnlRow& b) {
        if (a.pnlNanos != b.pnlNanos) return a.pnlNanos > b.pnlNanos;
        if (a.roiPercent != b.roiPercent) return a.roiPercent > b.roiPercent;
        return a.completedTrades > b.completedTrades;
    });
    if (results.size() > static_cast<size_t>(MAX_RANKED_WALLETS)) results.resize(MAX_RANKED_WALLETS);
    return results;
}

// ==================== PAGINATION CACHE ====================
// Small in-memory cache so switching pages never re-touches the `trades`
// table. Keyed by chatId (one active ranking view per user at a time);
// entries older than CACHE_TTL_SECONDS are treated as gone.
struct CachedRanking {
    std::string token;
    std::vector<PnlRow> rows;
    time_t computedAt = 0;
};

std::mutex g_cacheMutex;
std::map<std::string, CachedRanking> g_topPnlCache;

// Card layout (compact, per the display-fix spec): one "label: value" line
// per metric instead of a label line followed by a value line. The full
// 42-char wallet address is shown in <code> so it can be copied with one
// tap — its monospace width also keeps Telegram from collapsing the whole
// message (and the inline keyboard under it) to half the screen width.
RankingMessage renderPage(const std::string& token, const std::vector<PnlRow>& rows, int page) {
    int totalPages = std::max(1, static_cast<int>((rows.size() + PER_PAGE - 1) / PER_PAGE));
    page = std::max(1, std::min(page, totalPages));
    int startIdx = (page - 1) * PER_PAGE;
    int endIdx = std::min(static_cast<int>(rows.size()), startIdx + PER_PAGE);

    std::stringstream text;
    text << "🏆 <b>Top PnL (30D)</b>\n<code>" << safeString(token, 42) << "</code>\n\n";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    if (rows.empty()) {
        text << "📊 No wallets with at least " << MIN_COMPLETED_TRADES
             << " completed trades in the last 30 days.";
    } else {
        for (int i = startIdx; i < endIdx; i++) {
            const PnlRow& r = rows[i];
            int rank = i + 1;
            text << rankLabel(rank) << "\n";
            text << "<code>" << safeString(r.wallet, 42) << "</code>\n\n";
            text << "💵 <b>PnL:</b> " << formatUsdSigned(r.pnlNanos) << "\n";
            text << "📈 <b>ROI:</b> " << formatPercentSigned(r.roiPercent) << "\n";
            text << "🎯 <b>Win Rate:</b> " << r.winRatePercent << "%\n";
            text << "🔄 <b>Trades:</b> " << r.completedTrades << "\n";
            if (i + 1 < endIdx) text << "\n" << CARD_SEPARATOR << "\n\n";

            json row;
            row.push_back({{"text", "➕ Track"}, {"callback_data", "tt_track:" + r.wallet}});
            row.push_back({{"text", "🔍 BscScan"}, {"url", "https://bscscan.com/address/" + r.wallet}});
            keyboard["inline_keyboard"].push_back(row);
        }
    }

    json navRow = json::array();
    if (page > 1) navRow.push_back({{"text", "⬅️ Prev"}, {"callback_data", "tt_page:" + std::to_string(page - 1)}});
    navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "tt_noop"}});
    if (page < totalPages) navRow.push_back({{"text", "Next ➡️"}, {"callback_data", "tt_page:" + std::to_string(page + 1)}});
    keyboard["inline_keyboard"].push_back(navRow);

    return {text.str(), keyboard.dump()};
}

// ==================== GLOBAL (CROSS-TOKEN) TOP-100 RANKINGS ====================
// Same Average Cost Basis model as computeTopPnl(), but the position (held
// quantity / cost basis) is tracked per (wallet, token) while PnL, buy
// volume, completed-trade and win counts are accumulated per *wallet*
// across every token it traded — one pass over the whole `trades` table,
// ordered by wallet then token then time.
struct GlobalRankings {
    std::vector<PnlRow> byPnl;
    std::vector<PnlRow> byRoi;
    std::vector<PnlRow> byWinRate;
    std::vector<PnlRow> byActive;
};

std::vector<PnlRow> computeGlobalTop30D() {
    std::vector<PnlRow> results;
    long long since = static_cast<long long>(time(nullptr)) - WINDOW_SECONDS;

    std::string curWallet, curToken;
    cpp_int heldQty = 0, heldCost = 0;             // per (wallet, token) position
    cpp_int outerPnl = 0, outerBuyVol = 0;         // per wallet, across all its tokens
    int outerCompleted = 0, outerWinning = 0;

    auto flush = [&]() {
        if (!curWallet.empty() && outerCompleted >= MIN_GLOBAL_COMPLETED_TRADES &&
            outerCompleted <= MAX_BOT_FILTER_TRADES) {
            PnlRow row;
            row.wallet = curWallet;
            row.pnlNanos = cppIntToClampedI64(outerPnl);
            double volD = outerBuyVol > 0 ? outerBuyVol.convert_to<double>() : 0.0;
            double pnlD = outerPnl.convert_to<double>();
            // Global ROI is defined against total buy volume (not just the
            // cost basis consumed by completed sells) per this feature's spec.
            row.roiPercent = volD > 0.0 ? (100.0 * pnlD / volD) : 0.0;
            row.winRatePercent = outerCompleted > 0
                ? static_cast<int>(100.0 * outerWinning / outerCompleted + 0.5)
                : 0;
            row.completedTrades = outerCompleted;
            results.push_back(row);
        }
    };

    // Same rationale as computeTopPnl(): this scans the ENTIRE trades table,
    // so it runs on the read-only connection and never holds dbMutex.
    sqlite3* rdb = g_rankingReadDb ? g_rankingReadDb : db;
    std::unique_lock<std::mutex> readLock, writeLock;
    if (g_rankingReadDb) readLock = std::unique_lock<std::mutex>(g_rankingReadMutex);
    else                 writeLock = std::unique_lock<std::mutex>(dbMutex);

    sqlite3_stmt* s;
    if (!prepareOrLog(rdb, &s,
        "SELECT wallet, token, is_buy, usd_nanos, token_amount FROM trades "
        "WHERE timestamp>=? ORDER BY wallet ASC, token ASC, timestamp ASC")) return results;
    sqlite3_bind_int64(s, 1, since);

    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string wallet = safeColumnText(s, 0);
        std::string token = safeColumnText(s, 1);
        bool isBuy = sqlite3_column_int(s, 2) != 0;
        int64_t usdNanos = sqlite3_column_int64(s, 3);
        std::string amountStr = safeColumnText(s, 4);
        cpp_int amount = amountStr.empty() ? cpp_int(0) : cpp_int(amountStr);

        if (wallet != curWallet) {
            flush();
            curWallet = wallet;
            curToken.clear();
            outerPnl = 0; outerBuyVol = 0; outerCompleted = 0; outerWinning = 0;
        }
        if (token != curToken) {
            curToken = token;
            heldQty = 0; heldCost = 0;
        }

        if (isBuy) {
            heldQty += amount;
            heldCost += cpp_int(usdNanos);
            outerBuyVol += cpp_int(usdNanos);
        } else if (heldQty > 0 && amount > 0) {
            cpp_int matchedQty = amount < heldQty ? amount : heldQty;
            cpp_int costOfMatched = (heldCost * matchedQty) / heldQty;
            cpp_int proceedsMatched = (cpp_int(usdNanos) * matchedQty) / amount;

            outerPnl += (proceedsMatched - costOfMatched);
            outerCompleted++;
            if (proceedsMatched > costOfMatched) outerWinning++;

            heldQty -= matchedQty;
            heldCost -= costOfMatched;
        }
        // Sells with no known cost basis for that (wallet, token) pair are
        // skipped, exactly as in computeTopPnl().
    }
    sqlite3_finalize(s);
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

const std::vector<PnlRow>& selectGlobalList(const GlobalRankings& r, GlobalRankKind kind) {
    switch (kind) {
        case GlobalRankKind::PNL: return r.byPnl;
        case GlobalRankKind::ROI: return r.byRoi;
        case GlobalRankKind::WIN_RATE: return r.byWinRate;
        case GlobalRankKind::ACTIVE: return r.byActive;
    }
    return r.byPnl;
}

std::string globalTitle(GlobalRankKind kind) {
    switch (kind) {
        case GlobalRankKind::PNL: return "💵 Top PnL (30D)";
        case GlobalRankKind::ROI: return "📈 Top ROI (30D)";
        case GlobalRankKind::WIN_RATE: return "🎯 Top Win Rate (30D)";
        case GlobalRankKind::ACTIVE: return "🔄 Most Active (30D)";
    }
    return "🏆 Top Traders (30D)";
}

struct CachedGlobalRanking {
    GlobalRankings rankings;
    time_t computedAt = 0;
};

std::map<std::string, CachedGlobalRanking> g_globalRankingCache;

// Card layout is unified across all four ranking kinds (PnL, ROI, Win Rate,
// Trades are always all shown, in this fixed order) — only the sort order
// and title differ per kind. Compact "label: value" lines with a divider
// between cards, per the display-fix spec. The wallet address is shown in
// full (42 chars, <code>) so it's completely visible and copyable; the
// monospace line also forces Telegram to render the message — and the
// inline keyboard — at full width. Still a single "➕ Track" button per
// wallet (no BscScan link), per this feature's spec'd card format.
RankingMessage renderGlobalPage(GlobalRankKind kind, const std::vector<PnlRow>& rows, int page) {
    int totalPages = std::max(1, static_cast<int>((rows.size() + PER_PAGE - 1) / PER_PAGE));
    page = std::max(1, std::min(page, totalPages));
    int startIdx = (page - 1) * PER_PAGE;
    int endIdx = std::min(static_cast<int>(rows.size()), startIdx + PER_PAGE);

    std::stringstream text;
    text << "🏆 <b>" << globalTitle(kind) << "</b>\n\n";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    if (rows.empty()) {
        text << "📊 No completed trades in the last 30 days yet.";
    } else {
        for (int i = startIdx; i < endIdx; i++) {
            const PnlRow& r = rows[i];
            int rank = i + 1;
            text << rankLabel(rank) << "\n";
            text << "<code>" << safeString(r.wallet, 42) << "</code>\n\n";
            text << "💵 <b>PnL:</b> " << formatUsdSigned(r.pnlNanos) << "\n";
            text << "📈 <b>ROI:</b> " << formatPercentPlain(r.roiPercent) << "\n";
            text << "🎯 <b>Win Rate:</b> " << r.winRatePercent << "%\n";
            text << "🔄 <b>Trades:</b> " << r.completedTrades << "\n";
            if (i + 1 < endIdx) text << "\n" << CARD_SEPARATOR << "\n\n";

            json row;
            row.push_back({{"text", "➕ Track"}, {"callback_data", "tt_track:" + r.wallet}});
            keyboard["inline_keyboard"].push_back(row);
        }
    }

    std::string kindParam = globalRankKindToString(kind);
    json navRow = json::array();
    if (page > 1) navRow.push_back({{"text", "⬅️"}, {"callback_data", "gt_page:" + kindParam + ":" + std::to_string(page - 1)}});
    navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "tt_noop"}});
    if (page < totalPages) navRow.push_back({{"text", "➡️"}, {"callback_data", "gt_page:" + kindParam + ":" + std::to_string(page + 1)}});
    keyboard["inline_keyboard"].push_back(navRow);

    return {text.str(), keyboard.dump()};
}

} // namespace

// ==================== SAVE ====================
// blockTimestamp is the on-chain timestamp of the block containing the tx.
// Using it (instead of time(nullptr)) keeps the 30-day window and the
// buy/sell ordering correct even when the bot catches up on a backlog of
// old blocks after downtime — previously every backfilled trade got stamped
// "now", which skewed both the window and the Average Cost Basis order.
// Wall clock remains only as a fallback for a malformed/missing block field.
void saveTrade(const std::string& wallet, const TxResult& tx,
               const std::string& hash, long long block, long long blockTimestamp) {
    // Only BUY/SELL swaps get ranked — never TRANSFERs, never unclassified txs.
    if (!tx.valid || !tx.isSwap) return;
    if (tx.usdNanos <= 0) return;
    if (tx.tokenAddr.empty()) return;

    long long ts = blockTimestamp > 0
        ? blockTimestamp
        : static_cast<long long>(time(nullptr));

    std::string token = toLower(tx.tokenAddr);
    std::string amountStr = tx.rawAmount.convert_to<std::string>();
    int64_t usdNanos64 = cppIntToClampedI64(tx.usdNanos);

    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
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
    sqlite3_step(s);
    sqlite3_finalize(s);
}

// ==================== TOKEN RESOLUTION (unchanged) ====================
std::string resolveTokenArg(const std::string& argIn) {
    std::string arg = trim(argIn);
    if (arg.empty()) return "";
    if (isValidAddress(arg)) return toLower(arg);

    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "SELECT address FROM token_cache WHERE symbol = ? COLLATE NOCASE LIMIT 1")) return "";
    sqlite3_bind_text(s, 1, arg.c_str(), -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(s) == SQLITE_ROW) result = safeColumnText(s, 0);
    sqlite3_finalize(s);
    return result;
}

// ==================== TOP PNL (30D) ====================
RankingMessage buildTopPnlMessage(const std::string& chatId, const std::string& tokenArg, int page) {
    std::string token = resolveTokenArg(tokenArg);
    if (token.empty()) {
        return {"❌ Unknown token: " + safeString(tokenArg, 32) +
                "\n\nEither give the contract address directly, or a symbol the bot "
                "has already seen in a trade.", ""};
    }

    std::vector<PnlRow> rows = computeTopPnl(token);

    {
        std::lock_guard<std::mutex> l(g_cacheMutex);
        g_topPnlCache[chatId] = CachedRanking{token, rows, time(nullptr)};
    }

    return renderPage(token, rows, page);
}

RankingMessage buildTopPnlPage(const std::string& chatId, int page) {
    std::string token;
    std::vector<PnlRow> rows;
    {
        std::lock_guard<std::mutex> l(g_cacheMutex);
        auto it = g_topPnlCache.find(chatId);
        if (it == g_topPnlCache.end() || time(nullptr) - it->second.computedAt > CACHE_TTL_SECONDS) {
            return {"⏳ This ranking has expired. Please request it again (e.g. /toptrader TOKEN).", ""};
        }
        token = it->second.token;
        rows = it->second.rows;
    }
    return renderPage(token, rows, page);
}

// ==================== GLOBAL TOP TRADERS (30D) ====================
RankingMessage buildGlobalTopMenu() {
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "💵 Top PnL"}, {"callback_data", "gt_open:pnl"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "📈 Top ROI"}, {"callback_data", "gt_open:roi"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "🎯 Top Win Rate"}, {"callback_data", "gt_open:winrate"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "🔄 Most Active"}, {"callback_data", "gt_open:active"}}
    }));
    // Per-token leaderboard (the /toptrader TOKEN feature). An inline button
    // can't carry a user-supplied argument, so this only sends the "gt_token"
    // callback; main.cpp's handler must then ask the user which token
    // (see the gt_token handler next to the other gt_* callbacks).
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "🪙 Top PnL by Token"}, {"callback_data", "gt_token"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "← Back"}, {"callback_data", "menu:main"}}
    }));
    // The <code> MENU_STRETCH line is an invisible monospace "width
    // stretcher" (30 × U+2800): same mechanism that makes the ranking cards
    // full-width via their <code> wallet address, but rendered as blank
    // space instead of a visible ruled line. Kept short enough to never
    // wrap onto a second line (see the constant's comment).
    // Note: the final bubble width is still decided by the Telegram client
    // and cannot be forced to exactly 100%.
    return {std::string("🏆 <b>Top Traders (30D)</b>\n")
            + "<code>" + MENU_STRETCH + "</code>"
            + "\nChoose a ranking:", keyboard.dump()};
}

RankingMessage buildGlobalTopMessage(const std::string& chatId, GlobalRankKind kind) {
    GlobalRankings rankings;
    {
        std::lock_guard<std::mutex> l(g_cacheMutex);
        auto it = g_globalRankingCache.find(chatId);
        if (it != g_globalRankingCache.end() && time(nullptr) - it->second.computedAt <= CACHE_TTL_SECONDS) {
            rankings = it->second.rankings;
        } else {
            std::vector<PnlRow> base = computeGlobalTop30D();
            rankings = buildGlobalRankings(base);
            g_globalRankingCache[chatId] = CachedGlobalRanking{rankings, time(nullptr)};
        }
    }
    return renderGlobalPage(kind, selectGlobalList(rankings, kind), 1);
}

RankingMessage buildGlobalTopPage(const std::string& chatId, GlobalRankKind kind, int page) {
    std::lock_guard<std::mutex> l(g_cacheMutex);
    auto it = g_globalRankingCache.find(chatId);
    if (it == g_globalRankingCache.end() || time(nullptr) - it->second.computedAt > CACHE_TTL_SECONDS) {
        return {"⏳ This ranking has expired. Please reopen 🏆 Top Traders from the menu.", ""};
    }
    return renderGlobalPage(kind, selectGlobalList(it->second.rankings, kind), page);
}
