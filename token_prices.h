#pragma once

#include <cstdint>
#include <functional>
#include <string>

void setPriceStatHandler(std::function<void(int)> h);

int         getDecimals(const std::string& token);
std::string getSymbol(const std::string& addr);
uint64_t    getPriceNanos(const std::string& token);
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
void seedWalletTokensFromTrades();
