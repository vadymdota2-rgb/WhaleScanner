#pragma once

#include <string>
#include <vector>
#include "tx_analyzer.h"
#include "ru.h"

void initRankingDB();

void saveTrade(const std::string& wallet, const TxResult& tx,
               const std::string& hash,
               long long block,
               long long blockTimestamp);

void closeRankingDB();

void cleanupOldTrades();

bool isPermanentlyBanned(const std::string& wallet);
bool liftPermanentBan(const std::string& wallet);

struct SpotRankInfo {
    int rank = 0;
    int total = 0;
    long long pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int completedTrades = 0;
};

bool spotRankOf(const std::string& wallet, SpotRankInfo& out);

void markRankPresence(const char* venue, const std::vector<std::string>& wallets);
int  rankPresenceDays(const char* venue, const std::string& wallet);

void rebuildAllRankings();
void rankingCacheLoop();

struct RankingMessage {
    std::string text;
    std::string keyboard;
};

enum class GlobalRankKind { PNL, ROI, WIN_RATE, ACTIVE };

bool parseGlobalRankKind(const std::string& s, GlobalRankKind& out);

std::string getUserLanguage(const std::string& chatId);
void rememberView(const std::string& chatId, const std::string& data);
size_t countUserWhales(const std::string& chatId);
extern const std::string SERVICE_CHAT_ID;

std::string formatHoldTime(long long seconds, Lang lang);

void saveWalletHistory(const std::string& wallet, const TxResult& tx,
                       const std::string& hash, long long blockTimestamp);

struct PriorBuy {
    long long thenPriceNanos = 0;
    long long nowPriceNanos = 0;
    long long ageSeconds = 0;
    double changePercent = 0.0;
    long long avgEntryNanos = 0;
    int buyCount = 0;
};
bool lastBuyOutcome(const std::string& wallet, const std::string& token,
                    const std::string& currentHash, long long currentPriceNanos,
                    PriorBuy& out);

struct SellPnl {
    long long pnlNanos = 0;
    long long costNanos = 0;
    double pnlPercent = 0.0;
    int buyCount = 0;
    long long avgEntryNanos = 0;
};
bool sellOutcome(const std::string& wallet, const std::string& token,
                 long long sellUsdNanos, const std::string& sellAmountStr,
                 const std::string& currentHash, SellPnl& out);

bool handleRankingCallback(const std::string& chatId, const std::string& action,
                           const std::string& param, const std::string& data,
                           long long messageId, const std::string& callbackQueryId);

RankingMessage buildGlobalTopMenu(const std::string& chatId);

RankingMessage buildGlobalTopMessage(const std::string& chatId, GlobalRankKind kind,
                                     int maxRank, bool showUpgrade);

RankingMessage buildGlobalTopPage(const std::string& chatId, GlobalRankKind kind, int page,
                                  int maxRank, bool showUpgrade);
