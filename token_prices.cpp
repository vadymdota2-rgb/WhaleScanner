#include "token_prices.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <sqlite3.h>

#include "json.hpp"
#include "utils.h"
#include "rpc_client.h"
#include "tx_analyzer.h"

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;

namespace {
std::function<void(int)> g_priceStatHandler;
}
void setPriceStatHandler(std::function<void(int)> h) { g_priceStatHandler = std::move(h); }
namespace { inline void bump(int kind) { if (g_priceStatHandler) g_priceStatHandler(kind); } }

namespace {

constexpr time_t PRICE_TTL = 300;          // обычные токены: 5 мин
constexpr time_t PRICE_TTL_MAJOR = 900;    // major (WBNB/BTCB/стейблы…): 15 мин
constexpr time_t NATIVE_PRICE_TTL = 300;   // 5 мин
constexpr long long PRICE_HISTORY_STEP_SEC = 3600;
constexpr long long PRICE_HISTORY_TTL_SEC  = 90LL * 86400LL;
constexpr double MIN_POOL_LIQUIDITY_USD = 1000.0;
constexpr double STRONG_POOL_LIQ_USD = 10000.0;  // выше — DexScreener не зовём
constexpr double SPIKE_RATIO = 0.40;             // >40% vs кэш без глубокой liq → отбой
constexpr double SPIKE_TRUST_LIQ_USD = 50000.0;

std::mutex cacheMutex;
std::map<std::string, std::string> TOKEN_SYMBOLS;
std::map<std::string, int> TOKEN_DECIMALS;
std::map<std::string, std::pair<uint64_t, time_t>> PRICE_NANOS_CACHE;
std::map<std::string, double> POOL_LIQUIDITY_CACHE;
std::map<std::string, time_t> POOL_LIQUIDITY_TS;

std::mutex g_pairCacheMutex;
std::map<std::string, std::string> PAIR_CACHE;
std::map<std::string, time_t> PAIR_CACHE_TS;
// V3: позитивный адрес — навсегда; пустой + ts — негатив с TTL (не яд при сбое RPC)
std::map<std::string, std::string> V3_POOL_CACHE;
std::map<std::string, time_t> V3_POOL_CACHE_TS;
constexpr time_t V3_NEGATIVE_TTL = 1800;

static bool isMajorToken(const std::string& a) {
    const auto& ctx = chainCtx();
    if (ctx.stablecoins.count(a)) return true;
    if (!ctx.wrappedNative.empty() && a == toLower(ctx.wrappedNative)) return true;
    if (ctx.baseAssets.count(a)) return true;
    return false;
}

static time_t priceTtlFor(const std::string& a) {
    const auto& ctx = chainCtx();
    if (!ctx.wrappedNative.empty() && a == toLower(ctx.wrappedNative))
        return NATIVE_PRICE_TTL; // совпадает с пулами
    return isMajorToken(a) ? PRICE_TTL_MAJOR : PRICE_TTL;
}
constexpr time_t PAIR_CACHE_TTL = 6 * 3600;      // пары: 6 ч
constexpr time_t NEGATIVE_PAIR_TTL = 1800;        // «пары нет»: 30 мин
constexpr time_t LIQ_CACHE_TTL = 20 * 60;         // оценка liq в RAM: 20 мин
constexpr time_t CLEAN_NEG_EVERY = 10 * 60;
constexpr time_t CLEAN_PRICE_RAM_EVERY = 15 * 60;
constexpr time_t CLEAN_LIQ_EVERY = 20 * 60;
constexpr time_t CLEAN_PAIR_EVERY = 30 * 60;
constexpr time_t CLEAN_HIST_EVERY = 6 * 3600;

std::map<std::string, time_t> NEGATIVE_PAIR_UNTIL;

static void ensurePairCacheSchema() {
    static bool done = false;
    if (done) return;
    char* err = nullptr;
    sqlite3_exec(db, "ALTER TABLE pair_cache ADD COLUMN ts INTEGER NOT NULL DEFAULT 0", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    done = true;
}

}

void loadTokenCache() {
    std::vector<std::tuple<std::string,std::string,int,uint64_t,time_t>> rows;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (!prepareOrLog(db,&s,"SELECT address,symbol,decimals,price_nanos,price_ts FROM token_cache")) return;
        while (sqlite3_step(s)==SQLITE_ROW) {
            rows.emplace_back(
                safeColumnText(s,0), safeColumnText(s,1),
                sqlite3_column_int(s,2),
                static_cast<uint64_t>(sqlite3_column_int64(s,3)),
                static_cast<time_t>(sqlite3_column_int64(s,4)));
        }
        sqlite3_finalize(s);
    }
    std::lock_guard<std::mutex> cl(cacheMutex);
    for (auto& [a, sym, d, pn, ts] : rows) {
        if (!sym.empty()) {
            std::string clean;
            for (unsigned char c : sym) if (c >= 0x20 && c < 0x7F) clean += static_cast<char>(c);
            while (!clean.empty() && clean.back() == ' ') clean.pop_back();
            if (clean.size() * 2 < sym.size()) clean.clear();
            if (clean.size() > 16) clean.resize(16);
            if (!clean.empty() && clean != "UNKNOWN") TOKEN_SYMBOLS[a] = std::move(clean);
        }
        if (d > 0) TOKEN_DECIMALS[a] = d;
        if (pn > 0) PRICE_NANOS_CACHE[a] = {pn, ts};
    }
}

void savePairCache(const std::string& token, const std::string& val) {
    if (val.empty()) return;
    const time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> pl(g_pairCacheMutex);
        PAIR_CACHE[token] = val;
        PAIR_CACHE_TS[token] = now;
    }
    {
        std::lock_guard<std::mutex> l(dbMutex);
        ensurePairCacheSchema();
        sqlite3_stmt* s;
        if (!prepareOrLog(db,&s,
            "INSERT INTO pair_cache(token,val,ts) VALUES(?,?,?) "
            "ON CONFLICT(token) DO UPDATE SET val=excluded.val, ts=excluded.ts")) return;
        sqlite3_bind_text(s,1,token.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,2,val.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(s,3,static_cast<sqlite3_int64>(now));
        sqlite3_step(s); sqlite3_finalize(s);
    }
}

void loadPairCache() {
    const time_t now = time(nullptr);
    const time_t cutoff = now - PAIR_CACHE_TTL;
    std::vector<std::tuple<std::string,std::string,time_t>> rows;
    int skipped = 0;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        ensurePairCacheSchema();
        sqlite3_stmt* d;
        if (prepareOrLog(db,&d,"DELETE FROM pair_cache WHERE ts>0 AND ts<?")) {
            sqlite3_bind_int64(d,1,static_cast<sqlite3_int64>(cutoff));
            sqlite3_step(d);
            sqlite3_finalize(d);
        }
        sqlite3_stmt* s;
        // ts=0 — старые записи: берём, возраст = now, протухнут через TTL
        if (!prepareOrLog(db,&s,"SELECT token,val,ts FROM pair_cache")) return;
        while (sqlite3_step(s)==SQLITE_ROW) {
            std::string t = toLower(safeColumnText(s,0));
            std::string v = safeColumnText(s,1);
            time_t ts = static_cast<time_t>(sqlite3_column_int64(s,2));
            if (t.size()!=42 || v.empty()) continue;
            if (ts > 0 && ts < cutoff) { ++skipped; continue; }
            rows.emplace_back(std::move(t), std::move(v), ts > 0 ? ts : now);
        }
        sqlite3_finalize(s);
    }
    {
        std::lock_guard<std::mutex> pl(g_pairCacheMutex);
        for (auto& [t,v,ts] : rows) {
            PAIR_CACHE[t] = std::move(v);
            PAIR_CACHE_TS[t] = ts;
        }
    }
    if (!rows.empty() || skipped > 0)
        std::cout << "[STARTUP] Loaded " << rows.size() << " cached pairs"
                  << (skipped ? (", skipped expired " + std::to_string(skipped)) : "")
                  << std::endl;
}

void cleanupPairCache() {
    const time_t cutoff = time(nullptr) - PAIR_CACHE_TTL;
    int ram = 0, dbn = 0;
    {
        std::lock_guard<std::mutex> pl(g_pairCacheMutex);
        for (auto it = PAIR_CACHE_TS.begin(); it != PAIR_CACHE_TS.end(); ) {
            if (it->second < cutoff) {
                PAIR_CACHE.erase(it->first);
                it = PAIR_CACHE_TS.erase(it);
                ++ram;
            } else ++it;
        }
    }
    {
        std::lock_guard<std::mutex> l(dbMutex);
        ensurePairCacheSchema();
        sqlite3_stmt* s;
        if (prepareOrLog(db,&s,"DELETE FROM pair_cache WHERE ts>0 AND ts<?")) {
            sqlite3_bind_int64(s,1,static_cast<sqlite3_int64>(cutoff));
            if (sqlite3_step(s)==SQLITE_DONE) dbn = sqlite3_changes(db);
            sqlite3_finalize(s);
        }
    }
    if (ram > 0 || dbn > 0)
        std::cout << "[PAIR-CACHE] expired: ram=" << ram << " db=" << dbn << std::endl;
}

static void cleanupNegativePairs() {
    const time_t now = time(nullptr);
    int n = 0, v3n = 0;
    std::lock_guard<std::mutex> pl(g_pairCacheMutex);
    for (auto it = NEGATIVE_PAIR_UNTIL.begin(); it != NEGATIVE_PAIR_UNTIL.end(); ) {
        if (it->second <= now) { it = NEGATIVE_PAIR_UNTIL.erase(it); ++n; }
        else ++it;
    }
    for (auto it = V3_POOL_CACHE_TS.begin(); it != V3_POOL_CACHE_TS.end(); ) {
        if (now - it->second >= V3_NEGATIVE_TTL) {
            V3_POOL_CACHE.erase(it->first);
            it = V3_POOL_CACHE_TS.erase(it);
            ++v3n;
        } else ++it;
    }
    if (n > 0 || v3n > 0)
        std::cout << "[PAIR-CACHE] negative expired: v2=" << n << " v3=" << v3n << std::endl;
}

static void cleanupPriceRam() {
    const time_t now = time(nullptr);
    int n = 0;
    std::lock_guard<std::mutex> l(cacheMutex);
    for (auto it = PRICE_NANOS_CACHE.begin(); it != PRICE_NANOS_CACHE.end(); ) {
        const time_t ttl = priceTtlFor(it->first);
        if (now - it->second.second > ttl) {
            it = PRICE_NANOS_CACHE.erase(it);
            ++n;
        } else ++it;
    }
    if (n > 0) std::cout << "[PRICE] ram expired: " << n << std::endl;
}

static void cleanupLiqCache() {
    const time_t now = time(nullptr);
    int n = 0;
    std::lock_guard<std::mutex> l(cacheMutex);
    for (auto it = POOL_LIQUIDITY_TS.begin(); it != POOL_LIQUIDITY_TS.end(); ) {
        if (now - it->second > LIQ_CACHE_TTL) {
            POOL_LIQUIDITY_CACHE.erase(it->first);
            it = POOL_LIQUIDITY_TS.erase(it);
            ++n;
        } else ++it;
    }
    if (n > 0) std::cout << "[PRICE] liq cache expired: " << n << std::endl;
}

void cleanupTokenPricesPeriodic() {
    static time_t lastNeg = 0, lastPriceRam = 0, lastLiq = 0, lastPair = 0, lastHist = 0;
    const time_t now = time(nullptr);

    if (now - lastNeg >= CLEAN_NEG_EVERY) {
        cleanupNegativePairs();
        lastNeg = now;
    }
    if (now - lastPriceRam >= CLEAN_PRICE_RAM_EVERY) {
        cleanupPriceRam();
        lastPriceRam = now;
    }
    if (now - lastLiq >= CLEAN_LIQ_EVERY) {
        cleanupLiqCache();
        lastLiq = now;
    }
    if (now - lastPair >= CLEAN_PAIR_EVERY) {
        cleanupPairCache();
        lastPair = now;
    }
    if (now - lastHist >= CLEAN_HIST_EVERY) {
        cleanupPriceHistory();
        lastHist = now;
    }
}

void saveTokenMetadata(const std::string& a, const std::string& sym, int dec) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT INTO token_cache(address,symbol,decimals) VALUES(?,?,?) ON CONFLICT(address) DO UPDATE SET symbol=CASE WHEN excluded.symbol!='' THEN excluded.symbol ELSE token_cache.symbol END, decimals=CASE WHEN excluded.decimals>0 THEN excluded.decimals ELSE token_cache.decimals END")) return;
    sqlite3_bind_text(s,1,a.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,sym.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int(s,3,dec);
    sqlite3_step(s); sqlite3_finalize(s);
}

void savePriceHistory(const std::string& a, uint64_t pn) {
    if (!pn) return;
    const long long slot = (static_cast<long long>(time(nullptr)) / PRICE_HISTORY_STEP_SEC)
                         * PRICE_HISTORY_STEP_SEC;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "INSERT OR IGNORE INTO token_price_history(address,ts,price_nanos) VALUES(?,?,?)")) return;
    sqlite3_bind_text(s, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, slot);
    sqlite3_bind_int64(s, 3, static_cast<sqlite3_int64>(pn));
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void cleanupPriceHistory() {
    const long long cutoff = static_cast<long long>(time(nullptr)) - PRICE_HISTORY_TTL_SEC;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "DELETE FROM token_price_history WHERE ts<?")) return;
    sqlite3_bind_int64(s, 1, cutoff);
    if (sqlite3_step(s) == SQLITE_DONE) {
        const int n = sqlite3_changes(db);
        if (n > 0) std::cout << "[PRICE-HIST] удалено снимков старше срока: " << n << std::endl;
    }
    sqlite3_finalize(s);
}

void saveTokenPrice(const std::string& a, uint64_t pn) {
    if (!pn) return; std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT INTO token_cache(address,price_nanos,price_ts) VALUES(?,?,?) ON CONFLICT(address) DO UPDATE SET price_nanos=excluded.price_nanos, price_ts=excluded.price_ts")) return;
    sqlite3_bind_text(s,1,a.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,pn); sqlite3_bind_int64(s,3,time(nullptr));
    sqlite3_step(s); sqlite3_finalize(s);
}

int getDecimals(const std::string& addr) {
    std::string a=toLower(addr); { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_DECIMALS.count(a)) return TOKEN_DECIMALS[a]; }
    auto r=rpc("eth_call",{{{"to",addr},{"data","0x313ce567"}},"latest"});
    if (!r.is_string()) { bump(3); return 18; }
    int d=18;
    if (r.get<std::string>().length()>=66) try { d=std::stoi(r.get<std::string>().substr(2),nullptr,16); } catch (...) {}
    { std::lock_guard<std::mutex> l(cacheMutex); TOKEN_DECIMALS[a]=d; } saveTokenMetadata(a,"",d); return d;
}

static std::string parseTokenText(const json& r) {
    if (!r.is_string()) return {};
    const std::string raw = r.get<std::string>();
    std::string sym;
    if (raw.length() > 130) {
        try {
            std::string h = raw.substr(2);
            int len = std::stoi(h.substr(64, 64), nullptr, 16);
            if (len > 0 && len <= 32) {
                std::string sh = h.substr(128, static_cast<size_t>(len) * 2);
                for (size_t i = 0; i < sh.length(); i += 2)
                    sym += static_cast<char>(std::stoi(sh.substr(i, 2), nullptr, 16));
            }
        } catch (...) {}
    }
    if (sym.empty() && raw.length() >= 66) {
        try {
            std::string h = raw.substr(2, 64);
            for (size_t i = 0; i < h.length(); i += 2) {
                char c = static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16));
                if (c == '\0') break;
                sym += c;
            }
        } catch (...) {}
    }
    std::string clean;
    for (unsigned char c : sym)
        if (c >= 0x20 && c < 0x7F) clean += static_cast<char>(c);
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    while (!clean.empty() && clean.front() == ' ') clean.erase(clean.begin());
    if (clean.size() * 2 < sym.size()) clean.clear();
    if (clean.size() > 16) clean.resize(16);
    return clean;
}

std::string getSymbol(const std::string& addr) {
    std::string a = toLower(addr);
    {
        std::lock_guard<std::mutex> l(cacheMutex);
        auto it = TOKEN_SYMBOLS.find(a);
        if (it != TOKEN_SYMBOLS.end() && !it->second.empty() && it->second != "UNKNOWN")
            return it->second;
    }

    auto r = rpc("eth_call", {{{"to", addr}, {"data", "0x95d89b41"}}, "latest"});
    if (!r.is_string())
        bump(3);
    std::string sym = parseTokenText(r);

    if (sym.empty()) {
        auto rn = rpc("eth_call", {{{"to", addr}, {"data", "0x06fdde03"}}, "latest"});
        if (!rn.is_string())
            bump(3);
        else
            sym = parseTokenText(rn);
    }

    if (sym.empty()) return "UNKNOWN";

    {
        std::lock_guard<std::mutex> l(cacheMutex);
        TOKEN_SYMBOLS[a] = sym;
    }
    saveTokenMetadata(a, sym, 0);
    return sym;
}

double getPoolLiquidityUsd(const std::string& token) {
    const std::string a = toLower(token);
    std::lock_guard<std::mutex> l(cacheMutex);
    auto it = POOL_LIQUIDITY_CACHE.find(a);
    if (it == POOL_LIQUIDITY_CACHE.end()) return 0.0;
    auto ts = POOL_LIQUIDITY_TS.find(a);
    if (ts != POOL_LIQUIDITY_TS.end() && time(nullptr) - ts->second > LIQ_CACHE_TTL) {
        POOL_LIQUIDITY_CACHE.erase(it);
        POOL_LIQUIDITY_TS.erase(ts);
        return 0.0;
    }
    return it->second;
}

static uint64_t cachedNativePriceNanos() {
    const std::string w = toLower(chainCtx().wrappedNative);
    std::lock_guard<std::mutex> l(cacheMutex);
    auto it = PRICE_NANOS_CACHE.find(w);
    if (it == PRICE_NANOS_CACHE.end()) return 0;
    if (time(nullptr) - it->second.second > NATIVE_PRICE_TTL) return 0;
    return it->second.first;
}

// Цена token в nanos USD из конкретной V2-пары. liqUsdOut — оценка 2*base.
static bool quoteV2Pair(const std::string& token, const std::string& base, uint64_t basePriceNanos,
                        const std::string& pair, bool tokenIsZero,
                        uint64_t& priceOut, double& liqUsdOut) {
    json rr = rpc("eth_call", json::array({ json{{"to", pair}, {"data", "0x0902f1ac"}}, "latest" }));
    if (!rr.is_string()) return false;
    const std::string res = rr.get<std::string>();
    if (res.size() < 2 + 64 * 2) return false;
    const std::string body = res.substr(2);
    cpp_int r0 = hexToCppInt("0x" + body.substr(0, 64));
    cpp_int r1 = hexToCppInt("0x" + body.substr(64, 64));
    if (r0 <= 0 || r1 <= 0) return false;

    const cpp_int tokenRes = tokenIsZero ? r0 : r1;
    const cpp_int baseRes  = tokenIsZero ? r1 : r0;
    const int tokenDec = getDecimals(token);
    const int baseDec  = getDecimals(base);
    if (tokenDec < 0 || tokenDec > 36 || baseDec < 0 || baseDec > 36) return false;

    cpp_int tokenScale = 1, baseScale = 1;
    for (int i = 0; i < tokenDec; i++) tokenScale *= 10;
    for (int i = 0; i < baseDec;  i++) baseScale  *= 10;

    const cpp_int num = baseRes * tokenScale * cpp_int(basePriceNanos);
    const cpp_int den = tokenRes * baseScale;
    if (den <= 0) return false;
    const cpp_int priceNanos = num / den;
    if (priceNanos <= 0 || priceNanos > cpp_int("1000000000000000")) return false;

    const cpp_int liqNanos = (baseRes * cpp_int(basePriceNanos) * 2) / baseScale;
    liqUsdOut = static_cast<double>(static_cast<long long>(
        liqNanos > cpp_int("9000000000000000000") ? cpp_int("9000000000000000000") : liqNanos)) / 1e9;
    priceOut = static_cast<uint64_t>(static_cast<unsigned long long>(priceNanos));
    return true;
}

static bool resolvePairSide(const std::string& token, const std::string& pair, bool& tokenIsZero) {
    json t0 = rpc("eth_call", json::array({ json{{"to", pair}, {"data", "0x0dfe1681"}}, "latest" }));
    if (!t0.is_string()) return false;
    const std::string t0hex = t0.get<std::string>();
    if (t0hex.size() < 66) return false;
    tokenIsZero = ("0x" + toLower(t0hex.substr(t0hex.size() - 40))) == toLower(token);
    return true;
}

static std::string factoryGetPair(const std::string& a, const std::string& b) {
    const auto& ctx = chainCtx();
    std::string data = "0xe6a43905";
    data += std::string(24, '0') + toLower(a).substr(2);
    data += std::string(24, '0') + toLower(b).substr(2);
    json r = rpc("eth_call", json::array({ json{{"to", ctx.v2Factory}, {"data", data}}, "latest" }));
    if (!r.is_string()) return {};
    std::string pairHex = r.get<std::string>();
    if (pairHex.size() < 66) return {};
    std::string pair = "0x" + toLower(pairHex.substr(pairHex.size() - 40));
    if (pair == "0x" + std::string(40, '0')) return {};
    return pair;
}

// Uniswap V3 / Pancake V3 factory по сети
static std::string v3FactoryAddress() {
    const std::string id = chainCtx().dexscreenerChainId;
    if (id == "bsc") return "0x0bfbcf9fa4f9c56b0f40a671ad40e0805a091865";       // Pancake V3
    if (id == "ethereum") return "0x1f98431c8ad98523631ae4a59f267346ea31f984";  // Uniswap V3
    if (id == "base") return "0x33128a8fc17869897dce68ed026d694621f6fdfd";
    if (id == "arbitrum") return "0x1f98431c8ad98523631ae4a59f267346ea31f984";
    return {};
}

static std::string padAddr(const std::string& addr) {
    std::string a = toLower(addr);
    if (a.rfind("0x", 0) == 0) a = a.substr(2);
    if (a.size() > 40) a = a.substr(a.size() - 40);
    return std::string(64 - a.size(), '0') + a;
}

static std::string padUint(uint64_t v) {
    char buf[65];
    std::snprintf(buf, sizeof(buf), "%064llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

static std::string v3GetPool(const std::string& factory, const std::string& a,
                             const std::string& b, uint32_t fee) {
    const std::string key = toLower(a) + "|" + toLower(b) + "|" + std::to_string(fee);
    {
        std::lock_guard<std::mutex> l(g_pairCacheMutex);
        auto it = V3_POOL_CACHE.find(key);
        if (it != V3_POOL_CACHE.end()) {
            if (!it->second.empty()) return it->second; // позитивный — навсегда
            auto tsIt = V3_POOL_CACHE_TS.find(key);
            if (tsIt != V3_POOL_CACHE_TS.end() &&
                time(nullptr) - tsIt->second < V3_NEGATIVE_TTL)
                return {}; // негатив ещё свежий
            // негатив протух — переспросим
            V3_POOL_CACHE.erase(it);
            if (tsIt != V3_POOL_CACHE_TS.end()) V3_POOL_CACHE_TS.erase(tsIt);
        }
    }
    // getPool(address,address,uint24) → 0x1698ee82
    std::string data = "0x1698ee82";
    data += padAddr(a);
    data += padAddr(b);
    data += padUint(fee);
    json r = rpc("eth_call", json::array({ json{{"to", factory}, {"data", data}}, "latest" }));

    // Сбой RPC — НЕ кэшируем (иначе яд до рестарта)
    if (!r.is_string()) return {};

    std::string pool;
    std::string hex = r.get<std::string>();
    if (hex.size() >= 66) {
        pool = "0x" + toLower(hex.substr(hex.size() - 40));
        if (pool == "0x" + std::string(40, '0')) pool.clear();
    }
    {
        std::lock_guard<std::mutex> l(g_pairCacheMutex);
        V3_POOL_CACHE[key] = pool;
        if (pool.empty())
            V3_POOL_CACHE_TS[key] = time(nullptr); // негатив с TTL
        else
            V3_POOL_CACHE_TS.erase(key);
    }
    return pool;
}

// V3 slot0 → цена token в USD nanos. liqUsdOut — грубая оценка через liquidity().
static bool quoteV3Pool(const std::string& token, const std::string& base, uint64_t basePriceNanos,
                        const std::string& pool, uint64_t& priceOut, double& liqUsdOut) {
    // slot0(): 0x3850c7bd
    json rs = rpc("eth_call", json::array({ json{{"to", pool}, {"data", "0x3850c7bd"}}, "latest" }));
    if (!rs.is_string()) return false;
    const std::string sh = rs.get<std::string>();
    if (sh.size() < 2 + 64) return false;
    cpp_int sqrtP = hexToCppInt("0x" + sh.substr(2, 64));
    if (sqrtP <= 0) return false;

    bool tokenIsZero = false;
    if (!resolvePairSide(token, pool, tokenIsZero)) return false;

    const int tokenDec = getDecimals(token);
    const int baseDec  = getDecimals(base);
    if (tokenDec < 0 || tokenDec > 36 || baseDec < 0 || baseDec > 36) return false;

    cpp_int tokenScale = 1, baseScale = 1;
    for (int i = 0; i < tokenDec; i++) tokenScale *= 10;
    for (int i = 0; i < baseDec;  i++) baseScale  *= 10;

    const cpp_int Q96 = cpp_int(1) << 96;
    const cpp_int Q192 = Q96 * Q96;
    cpp_int priceNanos;
    // price(token0 in token1) = sqrtP^2 / 2^192
    if (tokenIsZero) {
        // token = token0, base = token1
        // usd = basePrice * (sqrtP^2 / 2^192) * 10^tokenDec / 10^baseDec
        const cpp_int num = cpp_int(basePriceNanos) * sqrtP * sqrtP * tokenScale;
        const cpp_int den = Q192 * baseScale;
        if (den <= 0) return false;
        priceNanos = num / den;
    } else {
        // token = token1, base = token0
        // usd = basePrice * (2^192 / sqrtP^2) * 10^tokenDec / 10^baseDec
        const cpp_int num = cpp_int(basePriceNanos) * Q192 * tokenScale;
        const cpp_int den = sqrtP * sqrtP * baseScale;
        if (den <= 0) return false;
        priceNanos = num / den;
    }
    if (priceNanos <= 0 || priceNanos > cpp_int("1000000000000000")) return false;

    // liquidity() + sqrtP → виртуальные резервы обеих сторон (как в V3 whitepaper для L)
    // depth ≈ 2 * min(virt_base_usd) — без tick-range это верхняя оценка, но стабильнее одной стороны
    double liqProxy = 0.0;
    json rl = rpc("eth_call", json::array({ json{{"to", pool}, {"data", "0x1a686502"}}, "latest" }));
    if (rl.is_string()) {
        const std::string lh = rl.get<std::string>();
        if (lh.size() >= 66) {
            cpp_int L = hexToCppInt(lh);
            if (L > 0 && sqrtP > 0) {
                const cpp_int virt1 = (L * sqrtP) / Q96; // token1 raw
                const cpp_int virt0 = (L * Q96) / sqrtP; // token0 raw
                const cpp_int baseVirt = tokenIsZero ? virt1 : virt0;
                const cpp_int liqNanos = (baseVirt * cpp_int(basePriceNanos) * 2) / baseScale;
                liqProxy = static_cast<double>(static_cast<long long>(
                    liqNanos > cpp_int("9000000000000000000") ? cpp_int("9000000000000000000") : liqNanos)) / 1e9;
                // Без tick-range оценка завышена — режем вес V3 в средневзвешенной
                liqProxy *= 0.5;
            }
        }
    }
    if (liqProxy < MIN_POOL_LIQUIDITY_USD) return false;

    priceOut = static_cast<uint64_t>(static_cast<unsigned long long>(priceNanos));
    liqUsdOut = liqProxy;
    return true;
}

// BNB/ETH из пула к стейблу — без DexScreener.
static uint64_t nativePriceFromPool() {
    const auto& ctx = chainCtx();
    const std::string w = toLower(ctx.wrappedNative);
    if (w.empty() || ctx.v2Factory.empty()) return 0;

    uint64_t bestPrice = 0;
    double bestLiq = -1.0;
    int stableTried = 0;
    for (const auto& sc : ctx.stablecoins) {
        if (++stableTried > 3) break;
        const std::string base = toLower(sc);
        std::string pair = factoryGetPair(w, base);
        if (pair.empty()) continue;
        bool tokenIsZero = false;
        if (!resolvePairSide(w, pair, tokenIsZero)) continue;
        uint64_t px = 0;
        double liq = 0;
        // base = stable → $1
        if (!quoteV2Pair(w, base, 1000000000ULL, pair, tokenIsZero, px, liq)) continue;
        if (liq < MIN_POOL_LIQUIDITY_USD) continue;
        if (liq > bestLiq) {
            bestLiq = liq;
            bestPrice = px;
        }
    }
    return bestPrice;
}

static uint64_t priceFromPoolReserves(const std::string& token) {
    const auto& ctx = chainCtx();
    if (ctx.v2Factory.empty() || ctx.wrappedNative.empty()) return 0;
    const std::string t = toLower(token);
    if (t.size() != 42 || t.rfind("0x", 0) != 0) return 0;
    if (t == toLower(ctx.wrappedNative)) return 0;
    if (ctx.stablecoins.count(t)) return 1000000000ULL;

    std::string cachedPair, cachedBase;
    int cachedSide = -1;
    {
        std::lock_guard<std::mutex> l(g_pairCacheMutex);
        auto neg = NEGATIVE_PAIR_UNTIL.find(t);
        if (neg != NEGATIVE_PAIR_UNTIL.end()) {
            if (time(nullptr) < neg->second) return 0;
            NEGATIVE_PAIR_UNTIL.erase(neg);
        }
        auto it = PAIR_CACHE.find(t);
        if (it != PAIR_CACHE.end()) {
            auto tsIt = PAIR_CACHE_TS.find(t);
            const time_t ts = (tsIt != PAIR_CACHE_TS.end()) ? tsIt->second : 0;
            if (ts > 0 && time(nullptr) - ts > PAIR_CACHE_TTL) {
                PAIR_CACHE.erase(it);
                if (tsIt != PAIR_CACHE_TS.end()) PAIR_CACHE_TS.erase(tsIt);
            } else {
                const size_t b1 = it->second.find('|');
                const size_t b2 = it->second.rfind('|');
                if (b1 != std::string::npos && b2 > b1) {
                    cachedPair = it->second.substr(0, b1);
                    cachedBase = it->second.substr(b1 + 1, b2 - b1 - 1);
                    if (b2 + 1 < it->second.size())
                        cachedSide = (it->second[b2 + 1] == '0') ? 0 : 1;
                }
            }
        }
    }

    // Стейблы первыми (цена $1), потом native. Только пулы с liq ≥ порога.
    std::vector<std::pair<std::string, uint64_t>> bases;
    {
        int added = 0;
        for (const auto& sc : ctx.stablecoins) {
            bases.push_back({toLower(sc), 1000000000ULL});
            if (++added >= 3) break;
        }
        bases.push_back({toLower(ctx.wrappedNative), 0});
    }
    // Кэшированную базу пробуем первой, если ещё свежая
    if (!cachedBase.empty()) {
        uint64_t fp = ctx.stablecoins.count(cachedBase) ? 1000000000ULL : 0;
        bases.insert(bases.begin(), {cachedBase, fp});
    }

    // Средневзвешенная: V2 + V3, вес = оценка liq, только ≥ порога
    double wSum = 0.0, pwSum = 0.0, bestLiq = -1.0;
    std::string bestStored;
    std::set<std::string> triedBase;
    const std::string v3f = v3FactoryAddress();
    // Сначала частые tier'ы; редкие — только если по этой базе ещё ничего не нашли
    static const uint32_t kV3FeesPrimary[] = {2500, 500};
    static const uint32_t kV3FeesSecondary[] = {100, 10000};

    for (const auto& [base, fixedPrice] : bases) {
        if (base == t || base.size() != 42) continue;
        if (!triedBase.insert(base).second) continue;

        uint64_t basePrice = fixedPrice;
        if (basePrice == 0) {
            basePrice = cachedNativePriceNanos();
            if (basePrice == 0) continue;
        }

        double baseWBefore = wSum;

        // --- V2 ---
        std::string pair;
        bool tokenIsZero = false;
        bool knownSide = false;
        if (!cachedPair.empty() && base == cachedBase) {
            pair = cachedPair;
            tokenIsZero = (cachedSide == 0);
            knownSide = (cachedSide >= 0);
        }
        if (pair.empty()) {
            pair = factoryGetPair(t, base);
        }
        if (!pair.empty()) {
            if (!knownSide) {
                if (!resolvePairSide(t, pair, tokenIsZero)) pair.clear();
            }
            if (!pair.empty()) {
                uint64_t px = 0;
                double liq = 0;
                if (quoteV2Pair(t, base, basePrice, pair, tokenIsZero, px, liq) &&
                    liq >= MIN_POOL_LIQUIDITY_USD) {
                    pwSum += static_cast<double>(px) * liq;
                    wSum  += liq;
                    if (liq > bestLiq) {
                        bestLiq = liq;
                        bestStored = pair + "|" + base + "|" + (tokenIsZero ? "0" : "1");
                    }
                }
            }
        }

        // --- V3: primary fees, secondary только если по базе пусто ---
        if (!v3f.empty() && bestLiq < STRONG_POOL_LIQ_USD) {
            auto tryFees = [&](const uint32_t* fees, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    std::string pool = v3GetPool(v3f, t, base, fees[i]);
                    if (pool.empty()) continue;
                    uint64_t px = 0;
                    double liq = 0;
                    if (!quoteV3Pool(t, base, basePrice, pool, px, liq)) continue;
                    pwSum += static_cast<double>(px) * liq;
                    wSum  += liq;
                    if (liq > bestLiq) bestLiq = liq;
                }
            };
            tryFees(kV3FeesPrimary, 2);
            if (wSum <= baseWBefore)
                tryFees(kV3FeesSecondary, 2);
        }

        // Ранний выход только если набрали вес с нескольких источников / очень глубокий пул
        if (bestLiq >= STRONG_POOL_LIQ_USD * 5.0 && wSum >= bestLiq * 1.5)
            break;
    }

    if (wSum > 0.0) {
        uint64_t bestPrice = static_cast<uint64_t>(pwSum / wSum);
        if (!bestStored.empty()) savePairCache(t, bestStored);
        {
            std::lock_guard<std::mutex> l(cacheMutex);
            POOL_LIQUIDITY_CACHE[t] = bestLiq > 0.0 ? bestLiq : wSum;
            POOL_LIQUIDITY_TS[t] = time(nullptr);
        }
        return bestPrice;
    }
    {
        std::lock_guard<std::mutex> l(g_pairCacheMutex);
        if (!PAIR_CACHE.count(t)) NEGATIVE_PAIR_UNTIL[t] = time(nullptr) + NEGATIVE_PAIR_TTL;
    }
    return 0;
}

// DexScreener: лучшая цена среди пар с liq ≥ порога. thinOut — видели только тонкие.
static bool fetchDexScreenerPrice(const std::string& a, double& priceOut, double& liqOut, bool& thinOut) {
    priceOut = 0; liqOut = 0; thinOut = false;
    auto r = http("https://api.dexscreener.com/latest/dex/tokens/" + a);
    try {
        auto j = json::parse(r);
        if (!j.contains("pairs") || !j["pairs"].is_array()) return false;
        const std::string wantChain = chainCtx().dexscreenerChainId;
        double bestLiquidity = -1.0;
        for (const auto& pair : j["pairs"]) {
            if (!pair.is_object()) continue;
            if (!wantChain.empty() && pair.value("chainId", std::string()) != wantChain) continue;
            if (!pair.contains("priceUsd") || !pair["priceUsd"].is_string()) continue;
            double liq = 0.0;
            if (pair.contains("liquidity") && pair["liquidity"].is_object() &&
                pair["liquidity"].contains("usd") && pair["liquidity"]["usd"].is_number())
                liq = pair["liquidity"]["usd"].get<double>();
            double price = 0.0;
            try { price = std::stod(pair["priceUsd"].get<std::string>()); } catch (...) { continue; }
            if (!std::isfinite(price) || price <= 0.0) continue;
            if (liq < MIN_POOL_LIQUIDITY_USD) { thinOut = true; continue; }
            if (liq > bestLiquidity) { bestLiquidity = liq; priceOut = price; liqOut = liq; }
        }
        return bestLiquidity >= 0.0;
    } catch (...) { return false; }
}

// Лёгкий TWAP: среднее fresh + до 4 снимков за последний час из token_price_history
static uint64_t softTwap(const std::string& a, uint64_t fresh) {
    if (!fresh) return 0;
    const long long cutoff = static_cast<long long>(time(nullptr)) - 3600;
    std::vector<uint64_t> pts;
    pts.push_back(fresh);
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s,
            "SELECT price_nanos FROM token_price_history WHERE address=? AND ts>=? "
            "ORDER BY ts DESC LIMIT 4")) {
            sqlite3_bind_text(s, 1, a.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, cutoff);
            while (sqlite3_step(s) == SQLITE_ROW) {
                uint64_t p = static_cast<uint64_t>(sqlite3_column_int64(s, 0));
                if (p > 0) pts.push_back(p);
            }
            sqlite3_finalize(s);
        }
    }
    if (pts.size() < 2) return fresh;
    unsigned long long sum = 0;
    for (uint64_t p : pts) sum += p;
    return static_cast<uint64_t>(sum / pts.size());
}

uint64_t getPriceNanosEx(const std::string& token, PriceSource* sourceOut) {
    constexpr double MAX_SANE_PRICE_USD = 1e6;
    constexpr double DIVERGENCE_REJECT = 0.15;
    if (sourceOut) *sourceOut = PriceSource::None;

    bool thinPoolSeen = false;
    std::string a = toLower(token);
    uint64_t prevCached = 0;
    {
        std::lock_guard<std::mutex> l(cacheMutex);
        auto it = PRICE_NANOS_CACHE.find(a);
        if (it != PRICE_NANOS_CACHE.end()) {
            prevCached = it->second.first;
            if (prevCached > 0 && time(nullptr) - it->second.second < priceTtlFor(a)) {
                if (sourceOut) *sourceOut = PriceSource::Cache;
                bump(8);
                return prevCached;
            }
        }
    }

    uint64_t poolPx = priceFromPoolReserves(a);
    double poolLiq = poolPx ? getPoolLiquidityUsd(a) : 0.0;

    const bool needDex = (poolPx == 0) || (poolLiq < STRONG_POOL_LIQ_USD);
    double dexP = 0, dexLiq = 0;
    bool haveDex = false;
    if (needDex) {
        bool dexThin = false;
        haveDex = fetchDexScreenerPrice(a, dexP, dexLiq, dexThin);
        if (dexThin) thinPoolSeen = true;
        if (haveDex && dexP > 1e12) { dexP = 0; haveDex = false; }
    }

    uint64_t n = 0;
    PriceSource src = PriceSource::None;

    if (poolPx > 0 && haveDex && dexP > 0.0 && dexP < MAX_SANE_PRICE_USD) {
        const double poolUsd = static_cast<double>(poolPx) / 1e9;
        const double denom = std::max(poolUsd, dexP);
        const double rel = std::fabs(poolUsd - dexP) / denom;
        if (rel <= DIVERGENCE_REJECT) {
            n = poolPx; src = PriceSource::Pool;
        } else if (dexLiq >= poolLiq) {
            n = static_cast<uint64_t>(dexP * 1e9); src = PriceSource::DexScreener;
            bump(4);
            std::cerr << "[PRICE] divergence " << (rel * 100.0) << "% " << a << " → dex\n";
        } else {
            n = poolPx; src = PriceSource::Pool;
            bump(4);
            std::cerr << "[PRICE] divergence " << (rel * 100.0) << "% " << a << " → pool\n";
        }
    } else if (poolPx > 0) {
        n = poolPx; src = PriceSource::Pool;
    } else if (haveDex && dexP > 0.0 && dexP < MAX_SANE_PRICE_USD) {
        n = static_cast<uint64_t>(dexP * 1e9); src = PriceSource::DexScreener;
        {
            std::lock_guard<std::mutex> l(cacheMutex);
            POOL_LIQUIDITY_CACHE[a] = dexLiq;
            POOL_LIQUIDITY_TS[a] = time(nullptr);
        }
    }

    if (n == 0) {
        const std::string platform = chainCtx().coingeckoPlatform.empty()
            ? std::string("binance-smart-chain") : chainCtx().coingeckoPlatform;
        auto r2 = http("https://api.coingecko.com/api/v3/simple/token_price/" + platform +
                       "?contract_addresses=" + a + "&vs_currencies=usd");
        try {
            auto j2 = json::parse(r2);
            if (j2.contains(a) && j2[a].contains("usd") && j2[a]["usd"].is_number()) {
                double cg = j2[a]["usd"].get<double>();
                if (std::isfinite(cg) && cg > 0.0 && cg < MAX_SANE_PRICE_USD) {
                    n = static_cast<uint64_t>(cg * 1e9);
                    src = PriceSource::CoinGecko;
                }
            }
        } catch (...) {}
    }

    if (n > 0 && prevCached > 0 && poolLiq < SPIKE_TRUST_LIQ_USD && src != PriceSource::CoinGecko) {
        const double prev = static_cast<double>(prevCached);
        const double cur  = static_cast<double>(n);
        const double rel = std::fabs(cur - prev) / std::max(prev, cur);
        if (rel > SPIKE_RATIO) {
            std::cerr << "[PRICE] spike " << (rel * 100.0) << "% " << a
                      << " keep cache (liq=$" << poolLiq << ")\n";
            n = prevCached;
            src = PriceSource::Cache;
            bump(5);
        }
    }

    // TWAP-сглаживание только для ончейн-цены (не ломаем CG/dex одной точкой)
    if (n > 0 && src == PriceSource::Pool) {
        uint64_t tw = softTwap(a, n);
        if (tw > 0) n = tw;
    }

    if (n > 0) {
        { std::lock_guard<std::mutex> l(cacheMutex); PRICE_NANOS_CACHE[a] = {n, time(nullptr)}; }
        saveTokenPrice(a, n);
        savePriceHistory(a, n);
        if (src == PriceSource::Pool) bump(0);
        else if (src == PriceSource::DexScreener) bump(6);
        else if (src == PriceSource::CoinGecko) bump(7);
        if (sourceOut) *sourceOut = src;
        return n;
    }

    {
        std::lock_guard<std::mutex> l(cacheMutex);
        if (PRICE_NANOS_CACHE.count(a) && PRICE_NANOS_CACHE[a].first > 0) {
            if (thinPoolSeen) bump(1);
            else {
                bump(2);
                std::cerr << "[PRICE] Stale cache: " << a << std::endl;
            }
            if (sourceOut) *sourceOut = PriceSource::Cache;
            return PRICE_NANOS_CACHE[a].first;
        }
    }
    return 0;
}

uint64_t getPriceNanos(const std::string& token) {
    return getPriceNanosEx(token, nullptr);
}

void ensureNativePrice() {
    const std::string w = toLower(chainCtx().wrappedNative);
    if (w.empty()) return;
    if (cachedNativePriceNanos() > 0) return;

    // 1) Ончейн: WBNB/WETH ↔ стейбл
    if (uint64_t fromPool = nativePriceFromPool()) {
        {
            std::lock_guard<std::mutex> l(cacheMutex);
            PRICE_NANOS_CACHE[w] = {fromPool, time(nullptr)};
        }
        saveTokenPrice(w, fromPool);
        std::cout << "[PRICE] Native from pool: $"
                  << (static_cast<double>(fromPool) / 1e9) << std::endl;
        return;
    }

    // 2) DexScreener → CoinGecko
    double p = 0.0;
    auto r = http("https://api.dexscreener.com/latest/dex/tokens/" + w);
    try {
        auto j = json::parse(r, nullptr, false);
        if (j.is_object() && j.contains("pairs") && j["pairs"].is_array()) {
            double bestLiq = -1.0;
            for (const auto& pair : j["pairs"]) {
                if (!pair.is_object()) continue;
                if (pair.value("chainId", "") != chainCtx().dexscreenerChainId) continue;
                if (!pair.contains("priceUsd") || !pair["priceUsd"].is_string()) continue;
                double liq = 0.0;
                if (pair.contains("liquidity") && pair["liquidity"].is_object() &&
                    pair["liquidity"].contains("usd") && pair["liquidity"]["usd"].is_number())
                    liq = pair["liquidity"]["usd"].get<double>();
                try {
                    double price = std::stod(pair["priceUsd"].get<std::string>());
                    if (!std::isfinite(price) || price <= 0.0 || price >= 1e6) continue;
                    if (liq > bestLiq) { bestLiq = liq; p = price; }
                } catch (...) {}
            }
        }
    } catch (...) {}

    if (p <= 0.0) {
        const std::string platform = chainCtx().coingeckoPlatform.empty()
            ? "binance-smart-chain" : chainCtx().coingeckoPlatform;
        auto r2 = http("https://api.coingecko.com/api/v3/simple/token_price/" + platform +
                       "?contract_addresses=" + w + "&vs_currencies=usd");
        try {
            auto j2 = json::parse(r2, nullptr, false);
            if (j2.contains(w) && j2[w].contains("usd") && j2[w]["usd"].is_number()) {
                double cg = j2[w]["usd"].get<double>();
                if (std::isfinite(cg) && cg > 0.0) p = cg;
            }
        } catch (...) {}
    }

    if (p > 0.0) {
        uint64_t n = static_cast<uint64_t>(p * 1e9);
        {
            std::lock_guard<std::mutex> l(cacheMutex);
            PRICE_NANOS_CACHE[w] = {n, time(nullptr)};
        }
        saveTokenPrice(w, n);
        std::cout << "[PRICE] Native price loaded (API): $" << p << std::endl;
    } else {
        std::cerr << "[PRICE] Failed to load native price — pool pricing limited" << std::endl;
    }
}
