#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <sqlite3.h>
#include "json.hpp"
#include "ru.h"

enum HlDirCode {
    DIR_UNKNOWN     = 0,
    DIR_OPEN_LONG   = 1,
    DIR_OPEN_SHORT  = 2,
    DIR_CLOSE_LONG  = 3,
    DIR_CLOSE_SHORT = 4,
    DIR_FLIP        = 5,
    DIR_LIQ_LONG    = 6,
    DIR_LIQ_SHORT   = 7,
    DIR_LIQ_OTHER   = 8,
};

inline int dirCode(const std::string& dirRaw) {
    std::string dir;
    dir.reserve(dirRaw.size());
    for (char c : dirRaw)
        dir += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;

    const bool isLong  = dir.find("long")  != std::string::npos;
    const bool isShort = dir.find("short") != std::string::npos;

    if (dir.find("liquidat") != std::string::npos ||
        dir.find("adl")      != std::string::npos) {
        if (isShort) return DIR_LIQ_SHORT;
        if (isLong)  return DIR_LIQ_LONG;
        return DIR_LIQ_OTHER;
    }
    if (dir.find("long > short") != std::string::npos ||
        dir.find("short > long") != std::string::npos) return DIR_FLIP;
    if (dir.find("open long")   != std::string::npos) return DIR_OPEN_LONG;
    if (dir.find("open short")  != std::string::npos) return DIR_OPEN_SHORT;
    if (dir.find("close long")  != std::string::npos) return DIR_CLOSE_LONG;
    if (dir.find("close short") != std::string::npos) return DIR_CLOSE_SHORT;
    return DIR_UNKNOWN;
}

inline bool dirIsLiquidation(int c) { return c >= DIR_LIQ_LONG && c <= DIR_LIQ_OTHER; }
inline bool dirIsOpen(int c)        { return c == DIR_OPEN_LONG || c == DIR_OPEN_SHORT; }
inline bool dirIsLong(int c)        { return c == DIR_OPEN_LONG || c == DIR_CLOSE_LONG || c == DIR_LIQ_LONG; }

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
