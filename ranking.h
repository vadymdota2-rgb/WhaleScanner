#pragma once

#include <string>
#include "tx_analyzer.h"

void initRankingDB();

void saveTrade(const std::string& wallet, const TxResult& tx,
               const std::string& hash,
               long long block,
               long long blockTimestamp);

void closeRankingDB();

void cleanupOldTrades();

void rankingCacheLoop();

std::string resolveTokenArg(const std::string& arg);

struct RankingMessage {
    std::string text;
    std::string keyboard;
};

RankingMessage buildTopPnlMessage(const std::string& chatId, const std::string& tokenArg, int page = 1);

RankingMessage buildTopPnlPage(const std::string& chatId, int page);

RankingMessage buildDailyChannelDigest();

enum class GlobalRankKind { PNL, ROI, WIN_RATE, ACTIVE };

bool parseGlobalRankKind(const std::string& s, GlobalRankKind& out);
std::string globalRankKindToString(GlobalRankKind k);

struct TraderStats {
    int rank = 0;
    long long pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int trades = 0;
    long long lastTs = 0;
};

bool getTraderStats(const std::string& wallet, TraderStats& out);

RankingMessage buildGlobalTopMenu(const std::string& chatId);

RankingMessage buildGlobalTopMessage(const std::string& chatId, GlobalRankKind kind,
                                     int maxRank, bool showUpgrade);

RankingMessage buildGlobalTopPage(const std::string& chatId, GlobalRankKind kind, int page,
                                  int maxRank, bool showUpgrade);
