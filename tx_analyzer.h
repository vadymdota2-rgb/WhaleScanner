#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <memory>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include "json.hpp"
#include "utils.h"

using json = nlohmann::json;
using boost::multiprecision::cpp_int;

// ============================================================================
// 1. DATA MODELS
// ============================================================================

enum class TxType {
    UNKNOWN,
    SWAP,
    TRANSFER,
    LP_ADD,
    LP_REMOVE,
    WRAP,
    UNWRAP,
    BRIDGE,
    NFT_TRADE,
    STAKE,
    UNSTAKE,
    APPROVE,
    REVOKE,
    FAILED,
    MINT,
    BURN,
    MULTICALL_MIXED,
    GAS_REFUND,
    CONTRACT_CALL  // generic contract interaction
};

enum class ConfidenceLevel {
    GUESS = 0,
    LOW = 1,
    MEDIUM = 2,
    HIGH = 3,
    CERTAIN = 4
};

struct TokenFlow {
    std::string token;
    std::string from;
    std::string to;
    cpp_int amount;
    bool isNative = false;
    bool viaPermit2 = false;
    bool isInternal = false;  // from trace, not log
};

struct SwapLeg {
    std::string pool;
    std::string tokenIn;
    std::string tokenOut;
    cpp_int amountIn;
    cpp_int amountOut;
    std::string protocol;
    int legIndex = 0;
};

struct RouteStep {
    std::string tokenIn;
    std::string tokenOut;
    std::string protocol;
    cpp_int amountIn;
    cpp_int amountOut;
    double estimatedSlippageBps = 0.0;  // 0 = unknown
};

struct FeeInfo {
    bool isFeeOnTransfer = false;
    cpp_int expectedAmount;
    cpp_int actualAmount;
    double feeBps = 0.0;  // basis points
};

struct TxResultV2 {
    bool valid = false;
    bool reverted = false;
    TxType type = TxType::UNKNOWN;
    std::string venue;
    std::string tokenAddr;
    std::string counterAddr;
    cpp_int rawAmount = 0;
    cpp_int counterAmount = 0;
    cpp_int usdNanos = 0;
    bool isBuy = false;

    // Extended
    std::vector<TokenFlow> allFlows;
    std::vector<SwapLeg> swapLegs;
    std::vector<RouteStep> route;
    std::set<std::string> venuesDetected;
    std::set<std::string> protocols;
    std::set<std::string> touchedTokens;
    std::optional<FeeInfo> feeInfo;

    // Native flow
    cpp_int nativeIn = 0;
    cpp_int nativeOut = 0;
    cpp_int nativeWrapAmount = 0;
    cpp_int nativeUnwrapAmount = 0;

    // Gas
    cpp_int gasUsed = 0;
    cpp_int gasPrice = 0;
    cpp_int gasCostNative = 0;

    // Signals
    bool hasSwapEvent = false;
    bool isUniversalRouter = false;
    bool isGenericMulticall = false;
    bool hasPermit2Signal = false;
    bool dexActivityDetected = false;
    bool lpMintOrBurnSeen = false;
    bool lpV3EventSeen = false;
    bool lpPoolIdentitySeen = false;
    bool isBridge = false;
    bool isNftTrade = false;
    bool isStaking = false;
    bool isAccountAbstraction = false;
    bool isIntentBased = false;  // UniswapX, CoW, 1inch Fusion
    bool hasWrapBeforeSwap = false;
    bool hasSweepAfterSwap = false;
    bool isFeeOnTransfer = false;
    bool isMultiHop = false;
    int swapLegCount = 0;

    ConfidenceLevel confidence = ConfidenceLevel::GUESS;
    std::string unknownReason;
    std::string realSender;  // for ERC-4337: the smart wallet, not EntryPoint

    // Helpers
    bool isSwap() const { return type == TxType::SWAP; }
    bool isLP() const { return type == TxType::LP_ADD || type == TxType::LP_REMOVE; }
    bool hasTwoSidedFlow() const;
    double confidenceScore() const { return static_cast<double>(confidence); }
    std::string typeString() const;
    std::string confidenceString() const;
};

// ============================================================================
// 2. CHAIN CONTEXT
// ============================================================================

struct ChainContext {
    std::string displayName;
    std::string explorerUrl;
    std::string explorerName;
    std::string nativeSymbol;
    std::string nativeMarker;
    std::string wrappedNative;
    std::set<std::string> stablecoins;
    std::set<std::string> baseAssets;
    std::map<std::string, std::string> routers;

    // Protocol contracts
    std::set<std::string> permit2Contracts;
    std::set<std::string> bridgeContracts;
    std::set<std::string> nftMarketplaces;
    std::set<std::string> stakingContracts;
    std::set<std::string> entryPoints;  // ERC-4337
    std::set<std::string> intentReactors; // UniswapX, CoW, etc

    bool isBaseAsset(const std::string& a) const;
    bool isStablecoin(const std::string& a) const;
    std::string lookupRouterLabel(const std::string& addr) const;
    bool isPermit2(const std::string& addr) const;
    bool isBridge(const std::string& addr) const;
    bool isNftMarketplace(const std::string& addr) const;
    bool isStaking(const std::string& addr) const;
    bool isEntryPoint(const std::string& addr) const;
    bool isIntentReactor(const std::string& addr) const;
};

ChainContext makeBscContext();
ChainContext makeEthereumContext();
ChainContext makeBaseContext();
ChainContext makeArbitrumContext();

// ============================================================================
// 3. PRICE / TOKEN INTERFACES
// ============================================================================

struct IPriceOracle {
    virtual ~IPriceOracle() = default;
    virtual uint64_t getPriceNanos(const std::string& tokenAddr) const = 0;
    virtual bool hasPrice(const std::string& tokenAddr) const { return getPriceNanos(tokenAddr) > 0; }
};

struct ITokenRegistry {
    virtual ~ITokenRegistry() = default;
    virtual int getDecimals(const std::string& tokenAddr) const = 0;
    virtual std::string getSymbol(const std::string& tokenAddr) const = 0;
    virtual bool isKnown(const std::string& tokenAddr) const { return getDecimals(tokenAddr) > 0; }
};

// ============================================================================
// 4. ANALYZER
// ============================================================================

struct AnalysisInput {
    json tx;
    json receipt;
    std::optional<json> trace;
    const ChainContext& chain;
    const IPriceOracle* prices = nullptr;
    const ITokenRegistry* tokens = nullptr;
};

TxResultV2 analyzeTxV2(const AnalysisInput& input, const std::string& wallet);

// Batch analysis for performance
std::vector<TxResultV2> analyzeTxBatch(
    const std::vector<AnalysisInput>& inputs,
    const std::vector<std::string>& wallets);

// ============================================================================
// 5. FORMATTING UTILS
// ============================================================================

cpp_int parseUint256(const std::string& h);
cpp_int hexToCppInt(const std::string& h);
std::string formatAmount(const cpp_int& raw, int dec);
cpp_int calcUsdNanos(const cpp_int& raw, int dec, uint64_t pn);
std::string formatUsd(const cpp_int& n);
cpp_int calcUnitPriceNanos(const cpp_int& usdNanos, const cpp_int& rawAmount, int dec);
std::string formatPriceUsd(const cpp_int& n);

// ============================================================================
// 6. LEGACY COMPATIBILITY (for existing main.cpp)
// ============================================================================

struct TxResult {
    bool valid = false;
    bool isSwap = false;
    std::string venue;
    std::string tokenAddr;
    std::string counterAddr;
    cpp_int rawAmount = 0;
    cpp_int counterAmount = 0;
    cpp_int usdNanos = 0;
    bool isBuy = false;
    bool hasSwapEvent = false;
    bool isUniversalRouter = false;
    bool isGenericMulticall = false;
    bool hasPermit2Signal = false;
    bool dexActivityDetected = false;
    bool lpMintOrBurnSeen = false;
    bool lpV3EventSeen = false;
    bool lpPoolIdentitySeen = false;
    std::string unknownReason;
};

extern const std::string WBNB_ADDR;
extern const std::string NATIVE_BNB_MARKER;

const ChainContext& chainCtx();
void setChainContext(const ChainContext& ctx);

TxResult analyzeTx(const json& tx, const json& receipt, const std::string& wa);
