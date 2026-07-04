#pragma once
// ==================== RANKING MODULE ====================
// Self-contained wallet-ranking system built entirely from trades the bot
// has already classified via analyzeTx(). No extra RPC calls, no external
// APIs, no additional threads — everything runs on the existing dbMutex /
// sqlite3* db connection from main.cpp.
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

// Resolves a user-supplied token argument (either a "0x..." contract
// address or a ticker symbol such as "cake") to a canonical lowercase
// contract address using the bot's own token_cache table. Returns an
// empty string if the symbol/address isn't known yet (i.e. the bot has
// never seen a trade involving it).
std::string resolveTokenArg(const std::string& arg);

// Leaderboards (top 20), scoped to trades within the last `days` days
// (default: effectively "all time"). All four are built from the same
// `trades` table and differ only in sort order / which totals they rank by.
std::string buildTopNet(const std::string& token, int days = 3650);
std::string buildTopBuy(const std::string& token, int days = 3650);
std::string buildTopSell(const std::string& token, int days = 3650);
std::string buildTopVolume(const std::string& token, int days = 3650);
