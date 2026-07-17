#pragma once

#include <string>
#include <set>
#include <map>
#include <cstdint>
#include <boost/multiprecision/cpp_int.hpp>
#include "json.hpp"

using boost::multiprecision::cpp_int;

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

TxResult analyzeTx(
    const nlohmann::json& tx,
    const nlohmann::json& receipt,
    const std::string& walletAddress
);
