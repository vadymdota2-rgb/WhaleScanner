#include "ranking.h"

#include <sqlite3.h>
#include <mutex>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdint>
#include <ctime>

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

namespace {

// Clamp a cpp_int (which can in principle exceed 64 bits) down to a value
// that fits in the sqlite3 INTEGER column, instead of invoking UB on
// convert_to<int64_t>() overflow. Realistic USD*1e9 amounts never get near
// this ceiling, so clamping is purely a defensive measure.
int64_t cppIntToClampedI64(const cpp_int& v) {
    static const cpp_int maxV(INT64_MAX);
    static const cpp_int minV(INT64_MIN);
    if (v > maxV) return INT64_MAX;
    if (v < minV) return INT64_MIN;
    return v.convert_to<int64_t>();
}

// Formats whole-dollar amounts with thousands separators, e.g. 1540000 ->
// "$1,540,000". usdNanos follows the same convention as the rest of the
// bot: USD * 1e9, truncated (not rounded) down to whole dollars.
std::string formatUsdWhole(int64_t usdNanos) {
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
    return (neg ? "-$" : "$") + out;
}

struct RankRow {
    std::string wallet;
    int64_t buyNanos = 0;
    int64_t sellNanos = 0;
    int64_t trades = 0;
};

std::string rankLabel(int rank) {
    switch (rank) {
        case 1: return "🥇 #1";
        case 2: return "🥈 #2";
        case 3: return "🥉 #3";
        default: return "#" + std::to_string(rank);
    }
}

std::string shortenAddr(const std::string& a) {
    if (a.size() < 10) return a;
    return a.substr(0, 6) + "..." + a.substr(a.size() - 4);
}

std::string buildTopGeneric(const std::string& tokenArg, int days,
                             const std::string& orderExpr, const std::string& title) {
    std::string token = resolveTokenArg(tokenArg);
    if (token.empty()) {
        return "❌ Unknown token: " + safeString(tokenArg, 32) +
               "\n\nEither give the contract address directly, or a symbol the bot "
               "has already seen in a trade.";
    }

    long long since = static_cast<long long>(time(nullptr)) - static_cast<long long>(days) * 86400LL;

    std::vector<RankRow> rows;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        std::string sql =
            "SELECT wallet, "
            "SUM(CASE WHEN is_buy=1 THEN usd_nanos ELSE 0 END) AS buy_n, "
            "SUM(CASE WHEN is_buy=0 THEN usd_nanos ELSE 0 END) AS sell_n, "
            "COUNT(*) AS trades "
            "FROM trades WHERE token=? AND timestamp>=? "
            "GROUP BY wallet ORDER BY " + orderExpr + " DESC LIMIT 20";
        if (!prepareOrLog(db, &s, sql.c_str())) return "❌ Internal error building the ranking.";
        sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, since);
        while (sqlite3_step(s) == SQLITE_ROW) {
            RankRow r;
            r.wallet = safeColumnText(s, 0);
            r.buyNanos = sqlite3_column_int64(s, 1);
            r.sellNanos = sqlite3_column_int64(s, 2);
            r.trades = sqlite3_column_int64(s, 3);
            rows.push_back(r);
        }
        sqlite3_finalize(s);
    }

    if (rows.empty()) {
        return "📊 No trades recorded yet for <code>" + safeString(token, 42) + "</code>.";
    }

    std::stringstream out;
    out << "🏆 <b>" << title << "</b>\n<code>" << safeString(token, 42) << "</code>\n\n";
    int rank = 1;
    for (auto& r : rows) {
        int64_t net = r.buyNanos - r.sellNanos;
        out << rankLabel(rank) << "\n<code>" << safeString(shortenAddr(r.wallet), 20) << "</code>\n";
        out << "Net: <b>" << (net >= 0 ? "+" : "") << formatUsdWhole(net) << "</b>\n";
        out << "Buy: " << formatUsdWhole(r.buyNanos) << "\n";
        out << "Sell: " << formatUsdWhole(r.sellNanos) << "\n";
        out << "Trades: " << r.trades << "\n";
        out << "----------------\n";
        rank++;
    }
    return out.str();
}

} // namespace

// ==================== SAVE ====================
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

// ==================== TOKEN RESOLUTION ====================
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

// ==================== LEADERBOARDS ====================
std::string buildTopNet(const std::string& token, int days) {
    return buildTopGeneric(token, days,
        "(SUM(CASE WHEN is_buy=1 THEN usd_nanos ELSE 0 END) - SUM(CASE WHEN is_buy=0 THEN usd_nanos ELSE 0 END))",
        "TOP Traders (Net)");
}

std::string buildTopBuy(const std::string& token, int days) {
    return buildTopGeneric(token, days,
        "SUM(CASE WHEN is_buy=1 THEN usd_nanos ELSE 0 END)",
        "TOP Buyers");
}

std::string buildTopSell(const std::string& token, int days) {
    return buildTopGeneric(token, days,
        "SUM(CASE WHEN is_buy=0 THEN usd_nanos ELSE 0 END)",
        "TOP Sellers");
}

std::string buildTopVolume(const std::string& token, int days) {
    return buildTopGeneric(token, days,
        "(SUM(CASE WHEN is_buy=1 THEN usd_nanos ELSE 0 END) + SUM(CASE WHEN is_buy=0 THEN usd_nanos ELSE 0 END))",
        "TOP Volume");
}
