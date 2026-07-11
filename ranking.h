#pragma once

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

void initRankingDB();

void saveTrade(const std::string& wallet, const TxResult& tx,
               const std::string& hash,
               long long block,
               long long blockTimestamp);

void closeRankingDB();

void cleanupOldTrades();

void rankingCacheLoop();

RankingMessage buildDailyChannelDigest();

std::string resolveTokenArg(const std::string& arg);

struct RankingMessage {
    std::string text;
    std::string keyboard;
};

RankingMessage buildTopPnlMessage(const std::string& chatId, const std::string& tokenArg, int page = 1);

RankingMessage buildTopPnlPage(const std::string& chatId, int page);

enum class GlobalRankKind { PNL, ROI, WIN_RATE, ACTIVE };

bool parseGlobalRankKind(const std::string& s, GlobalRankKind& out);
std::string globalRankKindToString(GlobalRankKind k);

RankingMessage buildGlobalTopMenu();

RankingMessage buildGlobalTopMessage(const std::string& chatId, GlobalRankKind kind,
                                     int maxRank, bool showUpgrade);

RankingMessage buildGlobalTopPage(const std::string& chatId, GlobalRankKind kind, int page,
                                  int maxRank, bool showUpgrade);
