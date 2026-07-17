#pragma once

#include <boost/multiprecision/cpp_int.hpp>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"

using boost::multiprecision::cpp_int;

/*
 * Drop-in compatible result for the existing bot.
 * Existing fields are preserved. New diagnostics can be ignored by old code.
 */
struct TxResult {
    bool valid = false;
    bool isSwap = false;
    bool isBuy = false;

    cpp_int rawAmount = 0;
    cpp_int usdNanos = 0;
    std::string tokenAddr;
    std::string venue;
    std::string counterAddr;
    cpp_int counterAmount = 0;

    bool hasSwapEvent = false;
    bool isUniversalRouter = false;
    bool isGenericMulticall = false;
    bool hasPermit2Signal = false;
    bool dexActivityDetected = false;
    bool lpMintOrBurnSeen = false;
    bool lpPoolIdentitySeen = false;
    bool lpV3EventSeen = false;
    std::string unknownReason;

    // Additional production diagnostics.
    std::string classification;   // BUY, SELL, TRANSFER, WRAP, ...
    std::string confidence;       // HIGH, MEDIUM, LOW
    bool failed = false;
    bool walletWasSender = false;
    bool nativeTraceUsed = false;
    bool tokenToToken = false;
    std::vector<std::string> evidence;
};

struct ChainContext {
    std::string displayName;
    std::string explorerUrl;
    std::string explorerName;
    std::string nativeSymbol;
    std::string nativeMarker;
    std::string wrappedNative;

    std::set<std::string> baseAssets;
    std::set<std::string> stablecoins;
    std::map<std::string, std::string> routers;
    std::set<std::string> bridges;
    std::set<std::string> permit2Contracts;
    std::set<std::string> positionManagers;
    std::set<std::string> nftMarketplaces;
    std::set<std::string> stakingContracts;
};

const ChainContext& chainCtx();
void setChainContext(const ChainContext& ctx);

ChainContext makeBscContext();
ChainContext makeEthereumContext();
ChainContext makeBaseContext();
ChainContext makeArbitrumContext();

extern const std::string WBNB_ADDR;
extern const std::string NATIVE_BNB_MARKER;

bool isBaseAsset(const std::string& address);
bool isStablecoin(const std::string& address);
std::string lookupRouterLabel(const std::string& address);

std::optional<cpp_int> parseUint256Opt(std::string_view hex);
std::optional<cpp_int> parseInt256Opt(std::string_view hex);
cpp_int parseUint256(const std::string& hex);
cpp_int hexToCppInt(const std::string& hex);

std::string formatAmount(const cpp_int& raw, int decimals);
cpp_int calcUsdNanos(const cpp_int& raw, int decimals, uint64_t priceNanos);
std::string formatUsd(const cpp_int& nanos);
cpp_int calcUnitPriceNanos(
    const cpp_int& usdNanos,
    const cpp_int& rawAmount,
    int decimals
);
std::string formatPriceUsd(const cpp_int& nanos);

/*
 * Existing call site remains valid.
 * Receipt-only mode cannot always recover native-token inflows.
 */
TxResult analyzeTx(
    const nlohmann::json& tx,
    const nlohmann::json& receipt,
    const std::string& wallet
);

/*
 * Preferred mode. `trace` should be callTracer-like JSON with:
 * from, to, value, calls[].
 */
TxResult analyzeTxWithTrace(
    const nlohmann::json& tx,
    const nlohmann::json& receipt,
    const nlohmann::json& trace,
    const std::string& wallet
);
