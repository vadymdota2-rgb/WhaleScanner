#pragma once

#include <string>
#include <set>
#include <map>
#include <vector>
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

    // Calldata decoder diagnostics. These fields do not change the public
    // analyzeTx(...) entry point and are safe for optional statistics.
    bool calldataDecoded = false;
    bool calldataSwap = false;
    bool calldataMatched = false;
    bool calldataRecovered = false;
    std::string decodedSelector;
    std::string decodedFunction;
    std::string decodedTokenIn;
    std::string decodedTokenOut;

    std::string unknownReason;
};

enum class RouterType {
    UNKNOWN,
    V2,
    V3,
    V4,
    UNIVERSAL,
    AGGREGATOR,
    BRIDGE,
    STAKING,
    NFT
};

struct ProtocolInfo {
    std::string protocol;
    std::string version;
    RouterType routerType = RouterType::UNKNOWN;
    bool supportsV2 = false;
    bool supportsV3 = false;
    bool supportsV4 = false;
    bool supportsPermit2 = false;
    bool supportsMulticall = false;
    bool supportsUniversalRouter = false;
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

    // Backward-compatible registry used by the rest of the project.
    std::map<std::string, std::string> routers;

    // Extended registries for new analyzer pipeline.
    std::map<std::string, ProtocolInfo> protocols;
    std::set<std::string> aggregators;
    std::set<std::string> bridges;
    std::set<std::string> staking;
    std::set<std::string> permit2;
    std::set<std::string> multicall;
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

TxResult analyzeTx(
    const nlohmann::json& tx,
    const nlohmann::json& receipt,
    const std::string& walletAddress
);
