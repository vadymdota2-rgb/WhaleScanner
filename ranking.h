#pragma once
// ==================== RANKING MODULE ====================
// Self-contained wallet-ranking system built entirely from trades the bot
// has already classified via analyzeTx(). No extra RPC calls, no external
// APIs, no additional threads — everything runs on the existing dbMutex /
// sqlite3* db connection from main.cpp.
//
// Ranking metric: Realized PnL (Average Cost Basis), computed locally from
// the `trades` table over a rolling 30-day window. No other data source is
// used.
//
// TxResult is declared here (instead of in main.cpp) purely so this header
// can be included from both main.cpp and ranking.cpp without duplicating
// the struct. Its fields are unchanged from the original definition.

#include <string>
#include <boost/multiprecision/cpp_int.hpp>

using boost::multiprecision::cpp_int;

struct TxResult {
    bool valid, isSwap, isBuy;
    cpp_int rawAmount, usdNanos;
    std::string tokenAddr;
    std::string venue;
    std::string counterAddr;
    cpp_int counterAmount;
};

// Creates the `trades` table (and its indexes) if it doesn't exist yet.
// Call once at startup, right after initDB().
void initRankingDB();

// Records a single BUY/SELL trade for `wallet`. Internally a no-op unless
// tx.valid, tx.isSwap and tx.usdNanos > 0 all hold — TRANSFERs and anything
// analyzeTx() couldn't classify are never stored. Duplicate tx_hash values
// are silently ignored (INSERT OR IGNORE), so re-processing a block is safe.
void saveTrade(const std::string& wallet, const TxResult& tx,
               const std::string& hash, long long block);

// Deletes trades older than 30 days. The `trades` table only ever holds a
// rolling 30-day window, matching the Top PnL (30D) ranking window. Call
// this alongside the bot's existing periodic cleanup (cleanupOldAlerts()).
void cleanupOldTrades();

// Resolves a user-supplied token argument (either a "0x..." contract
// address or a ticker symbol such as "cake") to a canonical lowercase
// contract address using the bot's own token_cache table. Returns an
// empty string if the symbol/address isn't known yet (i.e. the bot has
// never seen a trade involving it).
std::string resolveTokenArg(const std::string& arg);

// Telegram-ready {text, keyboard-JSON} pair, same shape/convention as
// TelegramUI::UIMessage in main.cpp.
struct RankingMessage {
    std::string text;
    std::string keyboard;
};

// Computes the Top PnL (30D) leaderboard for `tokenArg` (symbol or address),
// caches it under `chatId` for cheap pagination, and renders `page` (1-based,
// 5 wallets per page). This is the only entry point that actually recomputes
// the ranking from the `trades` table.
RankingMessage buildTopPnlMessage(const std::string& chatId, const std::string& tokenArg, int page = 1);

// Renders `page` of the leaderboard previously computed for `chatId` by
// buildTopPnlMessage(), without touching the `trades` table again. Used to
// serve pagination button presses. If nothing is cached (or the cache has
// expired), returns a message asking the user to request the ranking again.
RankingMessage buildTopPnlPage(const std::string& chatId, int page);

