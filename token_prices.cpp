#include "token_prices.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
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

constexpr time_t PRICE_TTL = 600;
constexpr time_t NATIVE_PRICE_TTL = 900;
constexpr long long PRICE_HISTORY_STEP_SEC = 3600;
constexpr long long PRICE_HISTORY_TTL_SEC  = 90LL * 86400LL;
constexpr double MIN_POOL_LIQUIDITY_USD = 1000.0;

std::mutex cacheMutex;
std::map<std::string, std::string> TOKEN_SYMBOLS;
std::map<std::string, int> TOKEN_DECIMALS;
std::map<std::string, std::pair<uint64_t, time_t>> PRICE_NANOS_CACHE;
std::map<std::string, double> POOL_LIQUIDITY_CACHE;

std::mutex g_pairCacheMutex;
std::map<std::string, std::string> PAIR_CACHE;
constexpr time_t NEGATIVE_PAIR_TTL = 1800;
std::map<std::string, time_t> NEGATIVE_PAIR_UNTIL;

}

void loadTokenCache() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT address,symbol,decimals,price_nanos,price_ts FROM token_cache")) return;
    while (sqlite3_step(s)==SQLITE_ROW) {
        std::string a=safeColumnText(s,0), sym=safeColumnText(s,1);
        if (!sym.empty()) {
            std::string clean;
            for (unsigned char c : sym) if (c >= 0x20 && c < 0x7F) clean += static_cast<char>(c);
            while (!clean.empty() && clean.back() == ' ') clean.pop_back();
            if (clean.size() * 2 < sym.size()) clean.clear();
            if (clean.size() > 16) clean.resize(16);
            if (!clean.empty() && clean != "UNKNOWN") TOKEN_SYMBOLS[a]=clean;
        }
        int d=sqlite3_column_int(s,2); if (d>0) TOKEN_DECIMALS[a]=d;
        uint64_t pn=sqlite3_column_int64(s,3); time_t ts=sqlite3_column_int64(s,4);
        if (pn>0) PRICE_NANOS_CACHE[a]={pn,ts};
    } sqlite3_finalize(s);
}

void savePairCache(const std::string& token, const std::string& val) {
    if (val.empty()) return;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT INTO pair_cache(token,val) VALUES(?,?) ON CONFLICT(token) DO UPDATE SET val=excluded.val")) return;
    sqlite3_bind_text(s,1,token.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,val.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_step(s); sqlite3_finalize(s);
}

void loadPairCache() {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT token,val FROM pair_cache")) return;
    int n = 0;
    {
        std::lock_guard<std::mutex> pl(g_pairCacheMutex);
        while (sqlite3_step(s)==SQLITE_ROW) {
            std::string t = toLower(safeColumnText(s,0));
            std::string v = safeColumnText(s,1);
            if (t.size()==42 && !v.empty()) { PAIR_CACHE[t]=v; ++n; }
        }
    }
    sqlite3_finalize(s);
    if (n>0) std::cout << "[STARTUP] Loaded " << n << " cached pairs" << std::endl;
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

void seedWalletTokensFromTrades() {
    std::lock_guard<std::mutex> l(dbMutex);
    const char* sql =
        "INSERT OR IGNORE INTO wallet_tokens(wallet, token, last_seen) "
        "SELECT wallet, token, MAX(timestamp) FROM trades GROUP BY wallet, token";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[STARTUP] wallet_tokens seed failed: " << (err ? err : "") << std::endl;
        sqlite3_free(err);
        return;
    }
    int n = sqlite3_changes(db);
    if (n > 0) std::cout << "[STARTUP] Seeded " << n << " wallet/token pairs from trade history" << std::endl;
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
    std::lock_guard<std::mutex> l(cacheMutex);
    auto it = POOL_LIQUIDITY_CACHE.find(toLower(token));
    return it != POOL_LIQUIDITY_CACHE.end() ? it->second : 0.0;
}

static uint64_t cachedNativePriceNanos() {
    const std::string w = toLower(chainCtx().wrappedNative);
    std::lock_guard<std::mutex> l(cacheMutex);
    auto it = PRICE_NANOS_CACHE.find(w);
    if (it == PRICE_NANOS_CACHE.end()) return 0;
    if (time(nullptr) - it->second.second > NATIVE_PRICE_TTL) return 0;
    return it->second.first;
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

    std::vector<std::pair<std::string, uint64_t>> bases;
    if (!cachedBase.empty()) {
        bases.push_back({cachedBase, ctx.stablecoins.count(cachedBase) ? 1000000000ULL : 0});
    } else {
        bases.push_back({toLower(ctx.wrappedNative), 0});
        int added = 0;
        for (const auto& sc : ctx.stablecoins) {
            bases.push_back({sc, 1000000000ULL});
            if (++added >= 2) break;
        }
    }

    uint64_t bestPrice = 0;
    double bestLiq = -1.0;
    std::string bestStored;

    for (const auto& [base, fixedPrice] : bases) {
        if (base == t || base.size() != 42) continue;

        uint64_t basePrice = fixedPrice;
        if (basePrice == 0) {
            basePrice = cachedNativePriceNanos();
            if (basePrice == 0) continue;
        }

        std::string pair;
        bool tokenIsZero = false;
        bool knownSide = false;
        if (!cachedPair.empty() && base == cachedBase) {
            pair = cachedPair;
            tokenIsZero = (cachedSide == 0);
            knownSide = (cachedSide >= 0);
        }

        if (pair.empty()) {
            std::string data = "0xe6a43905";
            data += std::string(24, '0') + t.substr(2);
            data += std::string(24, '0') + base.substr(2);
            json r = rpc("eth_call", json::array({ json{{"to", ctx.v2Factory}, {"data", data}}, "latest" }));
            if (!r.is_string()) continue;
            std::string pairHex = r.get<std::string>();
            if (pairHex.size() < 66) continue;
            pair = "0x" + toLower(pairHex.substr(pairHex.size() - 40));
            if (pair == "0x" + std::string(40, '0')) continue;
        }

        json rr = rpc("eth_call", json::array({ json{{"to", pair}, {"data", "0x0902f1ac"}}, "latest" }));
        if (!rr.is_string()) continue;
        const std::string res = rr.get<std::string>();
        if (res.size() < 2 + 64 * 2) continue;
        const std::string body = res.substr(2);

        cpp_int r0 = hexToCppInt("0x" + body.substr(0, 64));
        cpp_int r1 = hexToCppInt("0x" + body.substr(64, 64));
        if (r0 <= 0 || r1 <= 0) continue;

        if (!knownSide) {
            json t0 = rpc("eth_call", json::array({ json{{"to", pair}, {"data", "0x0dfe1681"}}, "latest" }));
            if (!t0.is_string()) continue;
            const std::string t0hex = t0.get<std::string>();
            if (t0hex.size() < 66) continue;
            tokenIsZero = ("0x" + toLower(t0hex.substr(t0hex.size() - 40))) == t;
        }

        const cpp_int tokenRes = tokenIsZero ? r0 : r1;
        const cpp_int baseRes  = tokenIsZero ? r1 : r0;

        const int tokenDec = getDecimals(t);
        const int baseDec  = getDecimals(base);
        if (tokenDec < 0 || tokenDec > 36 || baseDec < 0 || baseDec > 36) continue;

        cpp_int tokenScale = 1, baseScale = 1;
        for (int i = 0; i < tokenDec; i++) tokenScale *= 10;
        for (int i = 0; i < baseDec;  i++) baseScale  *= 10;

        const cpp_int num = baseRes * tokenScale * cpp_int(basePrice);
        const cpp_int den = tokenRes * baseScale;
        if (den <= 0) continue;
        const cpp_int priceNanos = num / den;

        if (priceNanos <= 0 || priceNanos > cpp_int("1000000000000000")) continue;

        const cpp_int liqNanos = (baseRes * cpp_int(basePrice) * 2) / baseScale;
        const double liqUsd = static_cast<double>(static_cast<long long>(
            liqNanos > cpp_int("9000000000000000000") ? cpp_int("9000000000000000000") : liqNanos)) / 1e9;

        if (liqUsd > bestLiq) {
            bestLiq = liqUsd;
            bestPrice = static_cast<uint64_t>(static_cast<unsigned long long>(priceNanos));
            bestStored = pair + "|" + base + "|" + (tokenIsZero ? "0" : "1");
        }
    }

    if (bestPrice > 0) {
        if (!bestStored.empty()) {
            {
                std::lock_guard<std::mutex> l(g_pairCacheMutex);
                PAIR_CACHE[t] = bestStored;
            }
            savePairCache(t, bestStored);
        }
        if (bestLiq > 0.0) {
            std::lock_guard<std::mutex> l(cacheMutex);
            POOL_LIQUIDITY_CACHE[t] = bestLiq;
        }
        return bestPrice;
    }
    {
        std::lock_guard<std::mutex> l(g_pairCacheMutex);
        if (!PAIR_CACHE.count(t)) NEGATIVE_PAIR_UNTIL[t] = time(nullptr) + NEGATIVE_PAIR_TTL;
    }
    return 0;
}

uint64_t getPriceNanos(const std::string& token) {
    bool thinPoolSeen = false;
    std::string a=toLower(token);
    { std::lock_guard<std::mutex> l(cacheMutex); if (PRICE_NANOS_CACHE.count(a)&&time(nullptr)-PRICE_NANOS_CACHE[a].second<PRICE_TTL) return PRICE_NANOS_CACHE[a].first; }
    if (uint64_t own = priceFromPoolReserves(a)) {
        { std::lock_guard<std::mutex> l(cacheMutex); PRICE_NANOS_CACHE[a]={own,time(nullptr)}; }
        saveTokenPrice(a, own);
        savePriceHistory(a, own);
        bump(0);
        return own;
    }

    double p=0;
    auto r=http("https://api.dexscreener.com/latest/dex/tokens/"+a);
    try {
        auto j=json::parse(r);
        if (j.contains("pairs") && j["pairs"].is_array()) {
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
                if (liq < MIN_POOL_LIQUIDITY_USD) { thinPoolSeen = true; continue; }
                if (liq > bestLiquidity) { bestLiquidity = liq; p = price; }
            }
            if (bestLiquidity >= 0.0) {
                std::lock_guard<std::mutex> l(cacheMutex);
                POOL_LIQUIDITY_CACHE[a] = bestLiquidity;
            }
        }
    } catch (...) {}
    if (p > 1e12) {
        std::cerr << "[PRICE] отброшена бессмысленная цена " << p << " для " << a << std::endl;
        p = 0;
    }
    if (p==0) {
        const std::string platform = chainCtx().coingeckoPlatform.empty()
                                   ? std::string("binance-smart-chain") : chainCtx().coingeckoPlatform;
        auto r2=http("https://api.coingecko.com/api/v3/simple/token_price/"+platform+"?contract_addresses="+a+"&vs_currencies=usd");
        try { auto j2=json::parse(r2); if (j2.contains(a)&&j2[a].contains("usd")&&j2[a]["usd"].is_number()) {
            double cg = j2[a]["usd"].get<double>();
            if (std::isfinite(cg) && cg > 0.0) p = cg;
        } } catch (...) {} }
    constexpr double MAX_SANE_PRICE_USD = 1e6;
    uint64_t n = (std::isfinite(p) && p > 0.0 && p < MAX_SANE_PRICE_USD)
               ? static_cast<uint64_t>(p * 1000000000.0) : 0;
    if (n>0) {
        { std::lock_guard<std::mutex> l(cacheMutex); PRICE_NANOS_CACHE[a]={n,time(nullptr)}; }
        saveTokenPrice(a,n);
        savePriceHistory(a,n);
    }
    else {
        std::lock_guard<std::mutex> l(cacheMutex);
        if (PRICE_NANOS_CACHE.count(a) && PRICE_NANOS_CACHE[a].first > 0) {
            if (thinPoolSeen) bump(1);
            else {
                bump(2);
                std::cerr << "[PRICE] Stale cache: " << a << std::endl;
            }
            return PRICE_NANOS_CACHE[a].first;
        }
    }
    return n;
}

void ensureNativePrice() {
    const std::string w = toLower(chainCtx().wrappedNative);
    if (w.empty()) return;
    if (cachedNativePriceNanos() > 0) return;

    double p = 0.0;
    auto r = http("https://api.dexscreener.com/latest/dex/tokens/" + w);
    try {
        auto j = json::parse(r, nullptr, false);
        if (j.is_object() && j.contains("pairs") && j["pairs"].is_array()) {
            for (const auto& pair : j["pairs"]) {
                if (!pair.is_object()) continue;
                if (pair.value("chainId", "") != chainCtx().dexscreenerChainId) continue;
                if (!pair.contains("priceUsd") || !pair["priceUsd"].is_string()) continue;
                try {
                    double price = std::stod(pair["priceUsd"].get<std::string>());
                    if (std::isfinite(price) && price > 0.0 && price < 1e6) {
                        p = price;
                        break;
                    }
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
        std::cout << "[PRICE] Native price loaded: $" << p << std::endl;
    } else {
        std::cerr << "[PRICE] Failed to load native price — pool pricing limited" << std::endl;
    }
}
