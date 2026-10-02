#pragma once

#include <string>
#include <unordered_set>
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

void markRankPresence(const char* venue, const std::vector<std::string>& wallets);

void rebuildAllRankings();
void rankingCacheLoop();

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
