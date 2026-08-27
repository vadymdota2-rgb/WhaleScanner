#pragma once

#include <cstdint>
#include <functional>
#include <string>

/** kind: 0 pool · 1 thin · 2 stale fb · 3 meta-rpc · 4 div · 5 spike · 6 dex · 7 cg · 8 cache */
void setPriceStatHandler(std::function<void(int)> h);

/** Откуда взялась цена — для логов и ИИ. */
enum class PriceSource : int {
    None = 0,
    Cache = 1,
    Pool = 2,        // V2/V3 on-chain, цена из резервов
    DexScreener = 3,
    CoinGecko = 4  // unused, kept for stats enum stability
};

int         getDecimals(const std::string& token);
std::string getSymbol(const std::string& addr);
uint64_t    getPriceNanos(const std::string& token);
/** То же + явный источник. source может быть nullptr. */
uint64_t    getPriceNanosEx(const std::string& token, PriceSource* source);
double      getPoolLiquidityUsd(const std::string& token);
void        ensureNativePrice();

void loadTokenCache();
void loadPairCache();
void saveTokenMetadata(const std::string& addr, const std::string& symbol, int decimals);
void saveTokenPrice(const std::string& addr, uint64_t priceNanos);
void savePriceHistory(const std::string& addr, uint64_t priceNanos);
void cleanupPriceHistory();
void cleanupPairCache();
/** Единая точка: разные кэши чистятся с разным периодом. Звать из maintenance ~раз в минуту. */
void cleanupTokenPricesPeriodic();
