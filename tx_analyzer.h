#pragma once

#include <string>
#include <set>
#include <map>
#include <vector>
#include <cstdint>
#include <boost/multiprecision/cpp_int.hpp>
#include "json.hpp"

using boost::multiprecision::cpp_int;

enum class TxOperation {
    UNKNOWN,
    SWAP_EXACT_IN,
    SWAP_EXACT_OUT,
    WRAP_NATIVE,
    UNWRAP_NATIVE,
    ADD_LIQUIDITY,
    REMOVE_LIQUIDITY,
    TRANSFER,
    BRIDGE,
    STAKE,
    UNSTAKE,
    CLAIM,
    NFT
};

struct DecodedIntent {
    bool valid = false;
    bool swap = false;
    bool exactInput = false;
    bool exactOutput = false;
    bool multicall = false;
    bool universalRouter = false;
    bool permit2 = false;
    bool malformed = false;
    bool bridge = false;
    bool liquidity = false;
    bool aggregator = false;
    bool v4 = false;
    bool targetedMulticall = false;
    bool universalSubPlan = false;
    bool acrossBridge = false;
    bool v4ActionsDecoded = false;

    TxOperation operation = TxOperation::UNKNOWN;

    std::string selector;
    std::string functionName;
    std::string protocol;
    std::string router;
    std::string recipient;

    std::string tokenIn;
    std::string tokenOut;
    std::string secondaryToken;
    std::vector<std::string> path;

    cpp_int amountIn = 0;
    cpp_int amountOut = 0;
    cpp_int amountOutMin = 0;
    cpp_int amountInMax = 0;
    cpp_int nativeValue = 0;

    std::vector<DecodedIntent> children;
};

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

    bool calldataDecoded = false;
    bool calldataSwap = false;
    bool calldataMatched = false;
    bool calldataRecovered = false;
    bool permit2Decoded = false;
    bool liquidityDecoded = false;
    bool bridgeDecoded = false;
    bool aggregatorDecoded = false;
    bool v4Decoded = false;
    bool targetedMulticallDecoded = false;
    bool universalSubPlanDecoded = false;
    bool acrossBridgeDecoded = false;
    bool v4ActionsDecoded = false;

    bool transferGraphSeen = false;
    bool graphPathToPool = false;
    bool graphPathFromPool = false;
    bool graphRecovered = false;
    bool graphAmbiguous = false;

    bool walletSwapRelated = false;
    bool unrelatedSwapEvent = false;

    std::string decodedSelector;
    std::string decodedFunction;
    std::string decodedTokenIn;
    std::string decodedTokenOut;
    std::string unknownReason;
};

struct RouterInfo {
    std::string protocol;
    std::string version;
    std::string routerType;

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

    // Backward-compatible registry.
    std::map<std::string, std::string> routers;

    // Extended registries for future stages.
    std::map<std::string, RouterInfo> routerInfo;
    std::set<std::string> aggregators;
    std::set<std::string> bridges;
    std::set<std::string> staking;
    std::set<std::string> permit2Contracts;
    std::set<std::string> multicallContracts;

    // selector -> human-readable function name / operation hint.
    std::map<std::string, std::string> selectorNames;
    std::set<std::string> aggregatorSwapSelectors;
    std::set<std::string> bridgeSelectors;
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
bool isNativeAsset(const std::string& a);
bool isQuoteAsset(const std::string& a);
std::string lookupRouterLabel(const std::string& addr);

cpp_int parseUint256(const std::string& h);
cpp_int hexToCppInt(const std::string& h);
std::string formatAmount(const cpp_int& raw, int dec);
cpp_int calcUsdNanos(const cpp_int& raw, int dec, uint64_t pn);
std::string formatUsd(const cpp_int& n);
cpp_int calcUnitPriceNanos(const cpp_int& usdNanos, const cpp_int& rawAmount, int dec);
std::string formatPriceUsd(const cpp_int& n);

DecodedIntent decodeTransactionInput(const nlohmann::json& tx);

TxResult analyzeTx(
    const nlohmann::json& tx,
    const nlohmann::json& receipt,
    const std::string& walletAddress
);
