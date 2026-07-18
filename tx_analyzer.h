#pragma once

#include <string>
#include <set>
#include <map>
#include <cstdint>
#include <boost/multiprecision/cpp_int.hpp>
#include "json.hpp"

using boost::multiprecision::cpp_int;

struct TxResult {
    bool valid, isSwap, isBuy;
    cpp_int rawAmount, usdNanos;
    std::string tokenAddr;
    std::string venue;
    std::string counterAddr;
    cpp_int counterAmount;
    bool hasSwapEvent = false;
    bool isUniversalRouter = false;
    bool isGenericMulticall = false;
    bool hasPermit2Signal = false;
    bool dexActivityDetected = false;
    bool lpMintOrBurnSeen = false;
    bool lpPoolIdentitySeen = false;
    bool lpV3EventSeen = false;
    std::string unknownReason;
    std::string diagnosticReason;
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
};

const ChainContext& chainCtx();
void setChainContext(const ChainContext& ctx);
ChainContext makeBscContext();
ChainContext makeEthereumContext();
ChainContext makeBaseContext();
ChainContext makeArbitrumContext();

extern const std::string WBNB_ADDR;
extern const std::string NATIVE_BNB_MARKER;

bool isBaseAsset(const std::string& a);
bool isStablecoin(const std::string& a);
std::string lookupRouterLabel(const std::string& addr);

cpp_int parseUint256(const std::string& h);
cpp_int hexToCppInt(const std::string& h);
std::string formatAmount(const cpp_int& raw, int dec);
cpp_int calcUsdNanos(const cpp_int& raw, int dec, uint64_t pn);
std::string formatUsd(const cpp_int& n);
cpp_int calcUnitPriceNanos(const cpp_int& usdNanos, const cpp_int& rawAmount, int dec);
std::string formatPriceUsd(const cpp_int& n);

TxResult analyzeTx(const nlohmann::json& tx, const nlohmann::json& receipt, const std::string& wa);
