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
               const std::string& hash,
               long long block,
               long long blockTimestamp);

void closeRankingDB();

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

// ==================== GLOBAL TOP TRADERS (30D) ====================
// Cross-token leaderboard: the same Average Cost Basis PnL model as
// buildTopPnlMessage(), but aggregated over every token traded by each
// wallet in the last 30 days, instead of one token at a time. Four
// independent sortings (PnL, ROI, Win Rate, trade count) are computed
// together in a single pass over `trades` and cached per chat; opening any
// of the four views, or flipping pages, never re-touches the `trades`
// table beyond the first computation for that chat.
//
// Premium gating (see premium.h): the full leaderboards are Top 50; how
// much of them a given user actually sees is decided by the CALLER via the
// `maxRank` parameter (10 Free / 50 Premium — main.cpp gets the number
// from the Premium module's premiumTopTradersLimit()). The cached rankings
// themselves are plan-agnostic: the cut happens only at render time, so a
// user who upgrades mid-session sees Top 50 immediately, without a
// recompute.
enum class GlobalRankKind { PNL, ROI, WIN_RATE, ACTIVE };

// Parses/serializes the short kind token used in callback_data
// ("pnl" / "roi" / "winrate" / "active"). parse returns false if unrecognized.
bool parseGlobalRankKind(const std::string& s, GlobalRankKind& out);
std::string globalRankKindToString(GlobalRankKind k);

// The "choose a ranking" submenu shown from the main menu's 🏆 Top Traders
// button: 💵 Top PnL / 📈 Top ROI / 🎯 Top Win Rate / 🔄 Most Active.
RankingMessage buildGlobalTopMenu();

// Computes (if not already cached for this chat) all four Top-50 global
// leaderboards and renders page 1 of `kind`.
// maxRank — how many positions this user may see (10 Free / 50 Premium);
// showUpgrade — append the "Unlock Top 50 with Premium." footer with the
// ⭐ Upgrade to Premium button (callers pass !isPremium(chatId)).
RankingMessage buildGlobalTopMessage(const std::string& chatId, GlobalRankKind kind,
                                     int maxRank, bool showUpgrade);

// Renders `page` of `kind` from the cache populated by
// buildGlobalTopMessage(). Never recomputes. If nothing is cached (or it
// has expired), asks the user to reopen the ranking from the menu.
// maxRank / showUpgrade — same as in buildGlobalTopMessage(); pagination
// is computed over the visible (plan-limited) part of the list.
RankingMessage buildGlobalTopPage(const std::string& chatId, GlobalRankKind kind, int page,
                                  int maxRank, bool showUpgrade);
