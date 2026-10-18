#include "token_prices.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

constexpr time_t PRICE_TTL = 600;          // обычные токены: 10 мин (меньше RPC)
constexpr time_t PRICE_TTL_MAJOR = 900;    // major (WBNB/BTCB/стейблы…): 15 мин
constexpr time_t NATIVE_PRICE_TTL = 900;   // 15 мин (как major, реже RPC на BNB)
constexpr long long PRICE_HISTORY_STEP_SEC = 3600;
constexpr long long PRICE_HISTORY_TTL_SEC  = 90LL * 86400LL;
// Пул мельче десяти тысяч долларов ценой считать нельзя: одна сделка в нём
// двигает курс в разы, и этот курс потом растекается по всем расчётам.
constexpr double MIN_POOL_LIQUIDITY_USD = 10000.0;
constexpr double SPIKE_RATIO = 0.40;             // >40% vs кэш без глубокой liq → отбой
constexpr double SPIKE_TRUST_LIQ_USD = 50000.0;

std::mutex cacheMutex;
std::map<std::string, std::string> TOKEN_SYMBOLS;
std::map<std::string, int> TOKEN_DECIMALS;
std::map<std::string, std::pair<uint64_t, time_t>> PRICE_NANOS_CACHE;


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
constexpr time_t CLEAN_PRICE_RAM_EVERY = 15 * 60;
constexpr time_t CLEAN_HIST_EVERY = 6 * 3600;

}

std::string sanitizeSymbol(const std::string& sym) {
    std::string clean;
    size_t bad = 0;
    for (size_t i = 0; i < sym.size(); ) {
        const unsigned char c = static_cast<unsigned char>(sym[i]);
        if (c < 0x20 || c == 0x7F) { i++; bad++; continue; }
        if (c < 0x80) { clean += static_cast<char>(c); i++; continue; }

        int need; uint32_t cp, lo;
        if      ((c >> 5) == 0x06) { need = 1; cp = c & 0x1F; lo = 0x80; }
        else if ((c >> 4) == 0x0E) { need = 2; cp = c & 0x0F; lo = 0x800; }
        else if ((c >> 3) == 0x1E) { need = 3; cp = c & 0x07; lo = 0x10000; }
        else { i++; bad++; continue; }

        if (i + static_cast<size_t>(need) >= sym.size()) { i++; bad++; continue; }
        bool ok = true;
        for (int k = 1; k <= need; k++) {
            const unsigned char b = static_cast<unsigned char>(sym[i + static_cast<size_t>(k)]);
            if ((b >> 6) != 0x02) { ok = false; break; }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (!ok || cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            i++; bad++; continue;
        }
        clean.append(sym, i, static_cast<size_t>(need) + 1);
        i += static_cast<size_t>(need) + 1;
    }
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    while (!clean.empty() && clean.front() == ' ') clean.erase(clean.begin());
    if (bad * 2 > sym.size()) clean.clear();
    if (clean.size() > 16) clean = truncateUtf8(clean, 16);
    return clean;
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
            std::string clean = sanitizeSymbol(sym);
            if (!clean.empty() && clean != "UNKNOWN") TOKEN_SYMBOLS[a] = std::move(clean);
        }
        if (d > 0) TOKEN_DECIMALS[a] = d;
        if (pn > 0) PRICE_NANOS_CACHE[a] = {pn, ts};
    }
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

void cleanupTokenPricesPeriodic() {
    static time_t lastPriceRam = 0, lastHist = 0;
    const time_t now = time(nullptr);

    if (now - lastPriceRam >= CLEAN_PRICE_RAM_EVERY) {
        cleanupPriceRam();
        lastPriceRam = now;
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

/* Цена токена за час: первая в часе плюс верх и низ за тот же час.
 *
 * Бот опрашивает цену по многу раз за час, а в базе оставалась одна точка —
 * первая, и остальное выбрасывалось. По одной точке не видно, что цена
 * задевала стоп и вернулась: разбор сигнала считал «никуда не пошёл», хотя
 * на деле его выбило, и история выходила лучше правды.
 *
 * price_nanos не трогаем: на нём построены ряды оракула, и подмена его на
 * последнюю цену часа сдвинула бы все признаки разом — обученное перестало
 * бы отвечать применённому. Верх и низ дописываются рядом и идут только в
 * разбор исходов, где такой симметрии не требуется. */
void savePriceHistory(const std::string& a, uint64_t pn) {
    if (!pn) return;
    const long long slot = (static_cast<long long>(time(nullptr)) / PRICE_HISTORY_STEP_SEC)
                         * PRICE_HISTORY_STEP_SEC;
    const auto v = static_cast<sqlite3_int64>(pn);
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "INSERT INTO token_price_history(address,ts,price_nanos,hi_nanos,lo_nanos) "
        "VALUES(?,?,?,?,?) "
        "ON CONFLICT(address,ts) DO UPDATE SET "
        "  hi_nanos=MAX(CASE WHEN hi_nanos>0 THEN hi_nanos ELSE price_nanos END, excluded.hi_nanos),"
        "  lo_nanos=MIN(CASE WHEN lo_nanos>0 THEN lo_nanos ELSE price_nanos END, excluded.lo_nanos)")) return;
    sqlite3_bind_text(s, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, slot);
    sqlite3_bind_int64(s, 3, v);
    sqlite3_bind_int64(s, 4, v);
    sqlite3_bind_int64(s, 5, v);
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
    if (!pn) return;
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT INTO token_cache(address,price_nanos,price_ts) VALUES(?,?,?) ON CONFLICT(address) DO UPDATE SET price_nanos=excluded.price_nanos, price_ts=excluded.price_ts")) return;
    sqlite3_bind_text(s,1,a.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,pn); sqlite3_bind_int64(s,3,time(nullptr));
    sqlite3_step(s); sqlite3_finalize(s);
}

int getDecimals(const std::string& addr) {
    std::string a=toLower(addr); { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_DECIMALS.count(a)) return TOKEN_DECIMALS[a]; }
    json r;
    for (int attempt = 0; attempt < 2; attempt++) {
        r = rpc("eth_call",{{{"to",addr},{"data","0x313ce567"}},"latest"});
        if (r.is_string()) break;
    }
    // Не ответил RPC — возвращаем -1, «не знаю», а не 18.
    //
    // Прежний откат на восемнадцать молчал о своей догадке, и у токена с
    // шестью знаками объём сделки выходил в триллион раз больше настоящего.
    // Отсюда и брались PnL в тридцать миллионов: не рынок, а наша поправка.
    //
    // Минус единица, а не ноль: ноль — это законное число знаков, и по нему
    // не отличить неизвестность от токена без дробной части. Вдобавок все
    // проверки вокруг уже написаны как `dec < 0`, и -1 они ловят сами.
    if (!r.is_string()) { bump(3); return -1; }

    const std::string hex = r.get<std::string>();
    int d = -1;
    if (hex.length() >= 66) {
        try { d = std::stoi(hex.substr(2), nullptr, 16); } catch (...) { d = -1; }
    }
    if (d < 0 || d > 36) { bump(3); return -1; }

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
    return sanitizeSymbol(sym);
}

static std::string fetchDexScreenerSymbol(const std::string& a) {
    auto r = http("https://api.dexscreener.com/latest/dex/tokens/" + a);
    try {
        auto j = json::parse(r);
        if (!j.contains("pairs") || !j["pairs"].is_array()) return "";
        const std::string wantChain = chainCtx().dexscreenerChainId;
        std::string bestSym;
        double bestLiq = -1.0;
        for (const auto& pair : j["pairs"]) {
            if (!pair.is_object()) continue;
            if (!wantChain.empty() && pair.value("chainId", std::string()) != wantChain) continue;
            double liq = 0.0;
            if (pair.contains("liquidity") && pair["liquidity"].is_object() &&
                pair["liquidity"].contains("usd") && pair["liquidity"]["usd"].is_number())
                liq = pair["liquidity"]["usd"].get<double>();
            std::string sym;
            if (pair.contains("baseToken") && pair["baseToken"].is_object()) {
                std::string ba = toLower(pair["baseToken"].value("address", ""));
                if (ba == a) sym = pair["baseToken"].value("symbol", "");
            }
            if (sym.empty() && pair.contains("quoteToken") && pair["quoteToken"].is_object()) {
                std::string qa = toLower(pair["quoteToken"].value("address", ""));
                if (qa == a) sym = pair["quoteToken"].value("symbol", "");
            }
            if (sym.empty()) continue;
            const std::string clean = sanitizeSymbol(sym);
            if (clean.empty()) continue;
            if (liq >= bestLiq) { bestLiq = liq; bestSym = clean; }
        }
        return bestSym;
    } catch (...) { return ""; }
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

    if (sym.empty()) {
        sym = fetchDexScreenerSymbol(a);
    }
    if (sym.empty()) return "UNKNOWN";

    {
        std::lock_guard<std::mutex> l(cacheMutex);
        TOKEN_SYMBOLS[a] = sym;
    }
    saveTokenMetadata(a, sym, 0);
    return sym;
}

static uint64_t cachedNativePriceNanos() {
    const std::string w = toLower(chainCtx().wrappedNative);
    std::lock_guard<std::mutex> l(cacheMutex);
    auto it = PRICE_NANOS_CACHE.find(w);
    if (it == PRICE_NANOS_CACHE.end()) return 0;
    if (time(nullptr) - it->second.second > NATIVE_PRICE_TTL) return 0;
    return it->second.first;
}

uint64_t nativePriceCachedNanos() { return cachedNativePriceNanos(); }

int decimalsCached(const std::string& token) {
    std::lock_guard<std::mutex> l(cacheMutex);
    auto it = TOKEN_DECIMALS.find(toLower(token));
    return it == TOKEN_DECIMALS.end() ? -1 : it->second;
}

uint64_t priceCachedNanos(const std::string& token, long long maxAgeSec) {
    std::lock_guard<std::mutex> l(cacheMutex);
    auto it = PRICE_NANOS_CACHE.find(toLower(token));
    if (it == PRICE_NANOS_CACHE.end()) return 0;
    return time(nullptr) - it->second.second <= maxAgeSec ? it->second.first : 0;
}

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
        if (!quoteV2Pair(w, base, 1000000000ULL, pair, tokenIsZero, px, liq)) continue;
        if (liq < MIN_POOL_LIQUIDITY_USD) continue;
        if (liq > bestLiq) {
            bestLiq = liq;
            bestPrice = px;
        }
    }
    return bestPrice;
}

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

uint64_t getPriceNanosEx(const std::string& token, PriceSource* sourceOut) {
    constexpr double MAX_SANE_PRICE_USD = 1e6;
    if (sourceOut) *sourceOut = PriceSource::None;

    bool thinPoolSeen = false;
    std::string a = toLower(token);
    // Стейблкоин — доллар. С рынка его цена бралась по чужой паре и бывала
    // дикой (USDT по $6,44): переводы USDT в алерты не попадали, и этого не
    // было видно, пока не появились заводы на биржи и выводы с бирж.
    if (chainCtx().stablecoins.count(a)) {
        if (sourceOut) *sourceOut = PriceSource::Cache;
        return 1000000000ULL;
    }
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

    double dexP = 0, dexLiq = 0;
    bool haveDex = false;
    {
        bool dexThin = false;
        haveDex = fetchDexScreenerPrice(a, dexP, dexLiq, dexThin);
        if (dexThin) thinPoolSeen = true;
        if (haveDex && dexP > 1e12) { dexP = 0; haveDex = false; }
        if (haveDex && !(dexP > 0.0 && dexP < MAX_SANE_PRICE_USD)) {
            haveDex = false;
            dexP = 0;
        }
    }

    // Свой разбор резервов пула отключён: он читает любой пул, каким бы
    // мелким тот ни был, и в нём же ошибается на нестандартных парах. Цену
    // берём только у DexScreener — он сам выбирает пул поглубже и отдаёт
    // ликвидность, по которой видно, можно ли этой цене верить.

    uint64_t n = 0;
    PriceSource src = PriceSource::None;

    if (haveDex) {
        n = static_cast<uint64_t>(dexP * 1e9);
        src = PriceSource::DexScreener;
    }

    const double trustLiq = haveDex ? dexLiq : 0.0;
    if (n > 0 && prevCached > 0 && trustLiq < SPIKE_TRUST_LIQ_USD) {
        const double prev = static_cast<double>(prevCached);
        const double cur  = static_cast<double>(n);
        const double rel = std::fabs(cur - prev) / std::max(prev, cur);
        if (rel > SPIKE_RATIO) {
            std::cerr << "[PRICE] spike " << (rel * 100.0) << "% " << a
                      << " keep cache (liq=$" << trustLiq << ")\n";
            n = prevCached;
            src = PriceSource::Cache;
            bump(5);
        }
    }


    if (n > 0) {
        { std::lock_guard<std::mutex> l(cacheMutex); PRICE_NANOS_CACHE[a] = {n, time(nullptr)}; }
        saveTokenPrice(a, n);
        savePriceHistory(a, n);
        if (src == PriceSource::DexScreener) bump(6);
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

    if (p > 0.0) {
        uint64_t n = static_cast<uint64_t>(p * 1e9);
        {
            std::lock_guard<std::mutex> l(cacheMutex);
            PRICE_NANOS_CACHE[w] = {n, time(nullptr)};
        }
        saveTokenPrice(w, n);
        std::cout << "[PRICE] Native price loaded (DexScreener): $" << p << std::endl;
    } else {
        std::cerr << "[PRICE] Failed to load native price — pool pricing limited" << std::endl;
    }
}
