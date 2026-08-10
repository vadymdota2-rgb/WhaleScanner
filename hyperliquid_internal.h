#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <sqlite3.h>
#include "json.hpp"
#include "ru.h"

namespace hl {

extern sqlite3* g_hlDb;
extern std::mutex g_hlDbMutex;
extern std::atomic<bool> g_connected;
extern std::atomic<long long> g_lastMsgTs;
extern std::atomic<unsigned long long> g_tradesSeen;
extern std::atomic<unsigned long long> g_hits;
extern std::atomic<unsigned long long> g_enriched;
extern std::atomic<unsigned long long> g_alertsSent;
extern std::atomic<unsigned long long> g_budgetSkips;
extern std::atomic<unsigned long long> g_botsBanned;
extern std::atomic<unsigned long long> g_reconnects;
extern std::atomic<int> g_subscribedCoins;
extern std::mutex g_queueMutex;
extern std::set<std::string> g_enrichQueue;

constexpr int HL_WEIGHT_CLEARINGHOUSE = 2;
constexpr long long NANOS_PER_UNIT = 1000000000LL;
constexpr long long HL_RANK_WINDOW_SEC = 30LL * 86400LL;
extern const char* const HL_CARD_SEPARATOR;

void invalidateRankCache();
void rebuildRankCache();

long long nowSec();
const char* dirMark(Lang lang);
std::string fmtUsd(long long nanos);
std::string formatPriceNanos(long long nanos);
bool parseDecimalToNanos(const std::string& s, long long& out);
std::string jstr(const nlohmann::json& j, const char* key, const char* def = "");
nlohmann::json infoPost(const nlohmann::json& body, int weight);
size_t watchedCount();

}
