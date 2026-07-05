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
constexpr int MIN_COMPLETED_TRADES = 5;
constexpr int MAX_RANKED_WALLETS = 20;
constexpr int PER_PAGE = 5;
constexpr time_t CACHE_TTL_SECONDS = 15 * 60;          // how long a computed ranking stays pageable
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
        if (!curWallet.empty() && completedTrades >= MIN_COMPLETED_TRADES) {
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

    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
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
            text << rankLabel(rank) << "\n\n";
            text << "💵 <b>PnL</b>\n" << formatUsdSigned(r.pnlNanos) << "\n\n";
            text << "📈 <b>ROI</b>\n" << formatPercentSigned(r.roiPercent) << "\n\n";
            text << "🎯 <b>Win Rate</b>\n" << r.winRatePercent << "%\n\n";
            text << "🔄 <b>Trades</b>\n" << r.completedTrades << "\n\n";
            text << "💼 <b>Wallet</b>\n\n<code>" << safeString(r.wallet, 42) << "</code>\n\n";

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

} // namespace

// ==================== SAVE (unchanged algorithm) ====================
void saveTrade(const std::string& wallet, const TxResult& tx,
               const std::string& hash, long long block) {
    // Only BUY/SELL swaps get ranked — never TRANSFERs, never unclassified txs.
    if (!tx.valid || !tx.isSwap) return;
    if (tx.usdNanos <= 0) return;
    if (tx.tokenAddr.empty()) return;

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
    sqlite3_bind_int64(s, 8, static_cast<sqlite3_int64>(time(nullptr)));
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
