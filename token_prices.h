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
    DexScreener = 3,
};

int         getDecimals(const std::string& token);
std::string getSymbol(const std::string& addr);
uint64_t    getPriceNanos(const std::string& token);
// Цена нативной монеты из кэша, без запросов к сети; 0 — кэш пуст или устарел.
uint64_t    nativePriceCachedNanos();
// Знаки и цена токена только из памяти, без запросов к сети: -1 и 0 — не знаем.
// Для разбора каждой транзакции блока, где ждать ответа сети нельзя.
int         decimalsCached(const std::string& token);
uint64_t    priceCachedNanos(const std::string& token, long long maxAgeSec);
/** То же + явный источник. source может быть nullptr. */
uint64_t    getPriceNanosEx(const std::string& token, PriceSource* source);
void        ensureNativePrice();

void loadTokenCache();
void saveTokenMetadata(const std::string& addr, const std::string& symbol, int decimals);
void saveTokenPrice(const std::string& addr, uint64_t priceNanos);
void savePriceHistory(const std::string& addr, uint64_t priceNanos);
void cleanupPriceHistory();
/** Единая точка: разные кэши чистятся с разным периодом. Звать из maintenance ~раз в минуту. */
void cleanupTokenPricesPeriodic();
