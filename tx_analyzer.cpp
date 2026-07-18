#include "tx_analyzer.h"
#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cmath>
#include "utils.h"

using json = nlohmann::json;

// Forward declarations
uint64_t getPriceNanos(const std::string& token);
int getDecimals(const std::string& addr);
std::string getSymbol(const std::string& addr);
json rpc(const std::string& method, json params, int maxRetries = 3);

// ... (keep existing helper functions: parseUint256, hexToCppInt, formatAmount, etc.) ...

namespace {

// Enhanced balance tracking
struct BalanceChange {
    std::string token;
    cpp_int before;
    cpp_int after;
    cpp_int delta() const { return after - before; }
};

// Improved flow tracking with confidence scoring
struct FlowWithConfidence {
    cpp_int amount;
    bool isSwapRelated = false;
    bool isDirectTransfer = false;
    double confidence = 0.0;
};

// Enhanced router detection
struct RouterInfo {
    bool isKnown = false;
    std::string label;
    bool isAggregator = false;
    bool supportsPermit2 = false;
};

class EnhancedTxAnalyzer {
private:
    const std::string wallet;
    const std::string& txHash;
    
    // State
    std::map<std::string, cpp_int> netFlow;
    std::map<std::string, cpp_int> grossIn;
    std::map<std::string, cpp_int> grossOut;
    std::map<std::string, std::vector<FlowEdge>> graph;
    std::vector<std::string> tokenOrder;
    std::set<std::string> swapPools;
    std::set<std::string> knownRouterAddresses;
    
    // Enhanced detection flags
    bool hasSwapEvent = false;
    bool hasNativeTransfer = false;
    cpp_int nativeDelta = 0;
    
    // Balance verification
    std::map<std::string, BalanceChange> balanceChanges;
    bool balancesVerified = false;

public:
    EnhancedTxAnalyzer(const std::string& w, const std::string& hash) 
        : wallet(w), txHash(hash) {}
    
    TxResult analyze(const json& tx, const json& receipt) {
        TxResult result;
        
        // Step 1: Basic validation
        if (!validateInputs(tx, receipt, result)) {
            return result;
        }
        
        // Step 2: Extract all token flows
        extractTokenFlows(receipt);
        
        // Step 3: Extract native currency flow
        extractNativeFlow(tx);
        
        // Step 4: Detect swap events
        detectSwapEvents(receipt);
        
        // Step 5: Get balance changes for verification
        verifyBalances();
        
        // Step 6: Classify transaction
        classifyTransaction(tx, receipt, result);
        
        // Step 7: Calculate financials
        if (result.valid) {
            calculateFinancials(result);
        }
        
        return result;
    }

private:
    bool validateInputs(const json& tx, const json& receipt, TxResult& result) {
        if (!receipt.is_object() || !receipt.contains("logs") || !receipt["logs"].is_array()) {
            result.unknownReason = "INVALID_RECEIPT";
            return false;
        }
        
        if (!receiptSucceeded(receipt)) {
            result.unknownReason = "TX_REVERTED";
            return false;
        }
        
        if (wallet.empty()) {
            result.unknownReason = "NO_WALLET";
            return false;
        }
        
        return true;
    }
    
    void extractTokenFlows(const json& receipt) {
        const std::string zero = "0x0000000000000000000000000000000000000000";
        const std::string dead = "0x000000000000000000000000000000000000dead";
        
        auto touch = [&](const std::string& token) {
            if (!netFlow.count(token)) {
                netFlow[token] = 0;
                grossIn[token] = 0;
                grossOut[token] = 0;
                tokenOrder.push_back(token);
            }
        };
        
        for (const auto& log : receipt["logs"]) {
            if (!log.is_object() || !log.contains("topics") ||
                !log["topics"].is_array() || log["topics"].empty()) {
                continue;
            }
            
            const std::string topic0 = toLower(log["topics"][0].get<std::string>());
            const std::string logAddr = log.contains("address") && log["address"].is_string()
                ? toLower(log["address"].get<std::string>()) : "";
            
            // Skip non-transfer events
            if (topic0 != ERC20_TRANSFER_TOPIC || logAddr.empty() ||
                log["topics"].size() != 3) {
                continue;
            }
            
            const std::string from = topicAddress(log["topics"][1].get<std::string>());
            const std::string to = topicAddress(log["topics"][2].get<std::string>());
            
            if (!log.contains("data") || !log["data"].is_string()) continue;
            
            const cpp_int amount = parseUint256(log["data"].get<std::string>());
            if (from.empty() || to.empty() || amount <= 0) continue;
            
            // Record in flow graph
            graph[logAddr].push_back({from, to, amount});
            
            // Skip if wallet not involved
            if (from != wallet && to != wallet) continue;
            
            touch(logAddr);
            
            if (to == wallet) {
                netFlow[logAddr] += amount;
                grossIn[logAddr] += amount;
            }
            
            if (from == wallet) {
                netFlow[logAddr] -= amount;
                grossOut[logAddr] += amount;
            }
        }
    }
    
    void extractNativeFlow(const json& tx) {
        // Extract native currency transfers from transaction
        if (!tx.contains("value") || !tx["value"].is_string()) {
            return;
        }
        
        cpp_int txValue = hexToCppInt(tx["value"].get<std::string>());
        
        if (tx.contains("from") && tx["from"].is_string() &&
            toLower(tx["from"].get<std::string>()) == wallet) {
            // Wallet sent native currency
            nativeDelta -= txValue;
            hasNativeTransfer = true;
        }
        
        // Check for incoming native transfers via internal transactions
        // This would require trace API, but we can estimate from balance change
        if (balancesVerified && balanceChanges.count(chainCtx().nativeMarker)) {
            nativeDelta = balanceChanges[chainCtx().nativeMarker].delta();
            if (nativeDelta != 0) {
                hasNativeTransfer = true;
            }
        }
    }
    
    void detectSwapEvents(const json& receipt) {
        static const std::set<std::string> SWAP_TOPICS = {
            "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822", // V2 Swap
            "0xc42079f94a6350d7e6235f29174924f928cc2ac818eb64fed8004e115fbcca67", // V3 Swap
            "0x19b47279256b2a23a1665c810c8d55a1758940ee09377d4f8d26497a3577dc83", // V3 Swap (alt)
        };
        
        for (const auto& log : receipt["logs"]) {
            if (!log.is_object() || !log.contains("topics") || 
                !log["topics"].is_array() || log["topics"].empty()) {
                continue;
            }
            
            const std::string topic0 = toLower(log["topics"][0].get<std::string>());
            
            if (SWAP_TOPICS.count(topic0)) {
                hasSwapEvent = true;
                if (log.contains("address") && log["address"].is_string()) {
                    swapPools.insert(toLower(log["address"].get<std::string>()));
                }
            }
        }
    }
    
    void verifyBalances() {
        // Try to get balance changes via eth_getBalance and balanceOf calls
        // This is optional but significantly improves accuracy
        
        if (txHash.empty()) return;
        
        try {
            // Get native balance change
            auto receipt = rpc("eth_getTransactionReceipt", {txHash});
            if (receipt.is_object() && receipt.contains("blockNumber")) {
                std::string blockHex = receipt["blockNumber"].get<std::string>();
                std::string prevBlockHex = "0x" + 
                    (hexToCppInt(blockHex) - 1).convert_to<std::string>();
                
                // Get balances before and after for wallet
                auto balBefore = rpc("eth_getBalance", {wallet, prevBlockHex});
                auto balAfter = rpc("eth_getBalance", {wallet, blockHex});
                
                if (balBefore.is_string() && balAfter.is_string()) {
                    cpp_int before = hexToCppInt(balBefore.get<std::string>());
                    cpp_int after = hexToCppInt(balAfter.get<std::string>());
                    balanceChanges[chainCtx().nativeMarker] = {chainCtx().nativeMarker, before, after};
                    balancesVerified = true;
                }
            }
            
            // For ERC20 tokens, we could call balanceOf at specific blocks
            // but this would require archive node access
            // Instead, rely on transfer events which are reliable for ERC20
            
        } catch (...) {
            // Balance verification is optional, continue without it
        }
    }
    
    RouterInfo identifyRouter(const std::string& address) {
        RouterInfo info;
        
        auto it = chainCtx().routers.find(toLower(address));
        if (it != chainCtx().routers.end()) {
            info.isKnown = true;
            info.label = it->second;
            
            // Mark aggregators
            static const std::set<std::string> aggregators = {
                "0x1111111254eeb25477b68fb85ed929f73a960582", // 1inch
                "0x6131b5fae19ea4f9d964eac0408e4408b66337b5", // KyberSwap
                "0x6352a56caadc4f1e25cd6c75970fa768a3304e64", // OpenOcean
            };
            
            info.isAggregator = aggregators.count(toLower(address)) > 0;
            info.supportsPermit2 = info.label.find("Universal Router") != std::string::npos;
        }
        
        return info;
    }
    
    bool isFlowSwapRelated(const std::string& token) {
        // Check if a token flow is related to a swap operation
        if (hasSwapEvent && swapPools.count(token) == 0) {
            return true; // Token flow to wallet during swap = swap related
        }
        
        // Check if flow goes through known routers
        for (const auto& edge : graph[token]) {
            RouterInfo router = identifyRouter(edge.from);
            if (router.isKnown || identifyRouter(edge.to).isKnown) {
                return true;
            }
        }
        
        return false;
    }
    
    FlowWithConfidence analyzeFlow(const std::string& token, const cpp_int& amount) {
        FlowWithConfidence flow;
        flow.amount = absInt(amount);
        
        // Check if this flow is swap-related
        flow.isSwapRelated = isFlowSwapRelated(token);
        
        // Check if it's a direct transfer (no intermediate contracts)
        flow.isDirectTransfer = !flow.isSwapRelated && !hasSwapEvent;
        
        // Calculate confidence score
        flow.confidence = 0.5; // Base confidence
        
        if (hasSwapEvent && flow.isSwapRelated) {
            flow.confidence = 0.9; // High confidence for swap
        }
        
        if (balancesVerified) {
            flow.confidence += 0.1; // Bonus for verified balances
        }
        
        // Reduce confidence if token has unusual decimals
        int decimals = getDecimals(token);
        if (decimals < 0 || decimals > 18) {
            flow.confidence -= 0.1;
        }
        
        return flow;
    }
    
    void classifyTransaction(const json& tx, const json& receipt, TxResult& result) {
        // Skip if no wallet flow
        if (tokenOrder.empty() || 
            std::all_of(tokenOrder.begin(), tokenOrder.end(), 
                       [&](const std::string& t) { return netFlow[t] == 0; })) {
            result.valid = false;
            result.unknownReason = "NO_WALLET_FLOW";
            return;
        }
        
        result.valid = true;
        
        // Separate incoming and outgoing tokens
        std::vector<std::string> incoming, outgoing;
        for (const auto& token : tokenOrder) {
            if (netFlow[token] > 0) incoming.push_back(token);
            else if (netFlow[token] < 0) outgoing.push_back(token);
        }
        
        // Case 1: Pure wrap/unwrap
        if (detectWrapUnwrap(incoming, outgoing, result)) {
            return;
        }
        
        // Case 2: Swap detection
        if (detectSwap(incoming, outgoing, tx, result)) {
            return;
        }
        
        // Case 3: LP operations
        if (detectLiquidityOperation(incoming, outgoing, receipt, result)) {
            return;
        }
        
        // Case 4: Simple transfer
        classifyAsTransfer(result);
    }
    
    bool detectWrapUnwrap(const std::vector<std::string>& incoming,
                         const std::vector<std::string>& outgoing,
                         TxResult& result) {
        const std::string& wrapped = chainCtx().wrappedNative;
        
        // Wrapping: wallet sends native, receives wrapped
        if (incoming.size() == 1 && outgoing.empty() && 
            incoming[0] == wrapped && hasNativeTransfer && nativeDelta < 0) {
            result.venue = "Wrap";
            result.isSwap = false;
            result.tokenAddr = wrapped;
            result.rawAmount = netFlow[wrapped];
            result.isBuy = true;
            return true;
        }
        
        // Unwrapping: wallet sends wrapped, receives native
        if (outgoing.size() == 1 && incoming.empty() &&
            outgoing[0] == wrapped && hasNativeTransfer && nativeDelta > 0) {
            result.venue = "Unwrap";
            result.isSwap = false;
            result.tokenAddr = wrapped;
            result.rawAmount = absInt(netFlow[wrapped]);
            result.isBuy = false;
            return true;
        }
        
        return false;
    }
    
    bool detectSwap(const std::vector<std::string>& incoming,
                   const std::vector<std::string>& outgoing,
                   const json& tx,
                   TxResult& result) {
        // Need at least one incoming and one outgoing (or native transfer)
        if (incoming.empty() && outgoing.empty()) return false;
        
        bool hasSwapPattern = false;
        std::string mainToken, counterToken;
        cpp_int mainAmount, counterAmount;
        bool isBuy = false;
        
        // Check for classic swap pattern: one token in, one token out
        if (incoming.size() == 1 && outgoing.size() >= 1) {
            // BUY: received token A, sent token B (or native)
            mainToken = incoming[0];
            mainAmount = netFlow[mainToken];
            isBuy = true;
            hasSwapPattern = true;
            
            // Find the counter asset
            if (!outgoing.empty()) {
                counterToken = outgoing[0];
                counterAmount = absInt(netFlow[counterToken]);
            } else if (hasNativeTransfer && nativeDelta < 0) {
                counterToken = chainCtx().nativeMarker;
                counterAmount = absInt(nativeDelta);
            }
        }
        else if (outgoing.size() == 1 && incoming.size() >= 1) {
            // SELL: sent token A, received token B (or native)
            mainToken = outgoing[0];
            mainAmount = absInt(netFlow[mainToken]);
            isBuy = false;
            hasSwapPattern = true;
            
            if (!incoming.empty()) {
                counterToken = incoming[0];
                counterAmount = netFlow[counterToken];
            } else if (hasNativeTransfer && nativeDelta > 0) {
                counterToken = chainCtx().nativeMarker;
                counterAmount = nativeDelta;
            }
        }
        else if (incoming.size() >= 1 && outgoing.size() >= 1 && hasSwapEvent) {
            // Multiple tokens swapped (aggregator)
            // Find the most valuable token as main
            mainToken = findMostValuableToken(incoming.empty() ? outgoing : incoming);
            mainAmount = absInt(netFlow[mainToken]);
            isBuy = netFlow[mainToken] > 0;
            hasSwapPattern = true;
            
            // Counter is the most valuable on the other side
            auto& counterList = isBuy ? outgoing : incoming;
            if (!counterList.empty()) {
                counterToken = findMostValuableToken(counterList);
                counterAmount = absInt(netFlow[counterToken]);
            }
        }
        
        if (hasSwapPattern) {
            result.isSwap = true;
            result.tokenAddr = mainToken;
            result.rawAmount = mainAmount;
            result.isBuy = isBuy;
            
            if (!counterToken.empty()) {
                result.counterAddr = counterToken;
                result.counterAmount = counterAmount;
            }
            
            // Determine venue
            std::string txTo = tx.contains("to") && tx["to"].is_string() 
                ? toLower(tx["to"].get<std::string>()) : "";
            RouterInfo router = identifyRouter(txTo);
            
            if (router.isKnown) {
                result.venue = router.label;
            } else if (!swapPools.empty()) {
                result.venue = "DEX Pool";
            } else {
                result.venue = "Swap";
            }
            
            return true;
        }
        
        return false;
    }
    
    bool detectLiquidityOperation(const std::vector<std::string>& incoming,
                                 const std::vector<std::string>& outgoing,
                                 const json& receipt,
                                 TxResult& result) {
        // LP operations typically involve 2+ tokens in the same direction
        // Plus LP token in the opposite direction
        
        // Check for LP-specific events
        bool hasLPEvent = false;
        for (const auto& log : receipt["logs"]) {
            if (!log.is_object() || !log.contains("topics") || 
                !log["topics"].is_array() || log["topics"].empty()) {
                continue;
            }
            
            const std::string topic0 = toLower(log["topics"][0].get<std::string>());
            
            static const std::set<std::string> LP_TOPICS = {
                "0x4c209b5fc8ad50758f13e2e1088ba56a560dff690a1c6fef26394f4c03821c4f", // V2 Mint
                "0xdccd412f0b1252819cb1fd330b93224ca42612892bb3f4f789976e6d81936496", // V2 Burn
                "0x3067048beee31b25b2f1681f88dac838c8bba36af25bfb2b7cf7473a5847e35f", // V3 Increase
                "0x26f6a048ee9138f2c0ce266f322cb99228e8d619ae2bff30c67f8dcf9d2377b4", // V3 Decrease
            };
            
            if (LP_TOPICS.count(topic0)) {
                hasLPEvent = true;
                break;
            }
        }
        
        if (!hasLPEvent) return false;
        
        // Add liquidity: sending 2+ tokens, receiving LP token
        if (outgoing.size() >= 2 && incoming.size() == 1) {
            result.venue = "Add Liquidity";
            result.isSwap = false;
            result.tokenAddr = incoming[0]; // LP token
            result.rawAmount = netFlow[incoming[0]];
            result.isBuy = true;
            return true;
        }
        
        // Remove liquidity: sending LP token, receiving 2+ tokens
        if (incoming.size() >= 2 && outgoing.size() == 1) {
            result.venue = "Remove Liquidity";
            result.isSwap = false;
            result.tokenAddr = outgoing[0]; // LP token
            result.rawAmount = absInt(netFlow[outgoing[0]]);
            result.isBuy = false;
            return true;
        }
        
        return false;
    }
    
    void classifyAsTransfer(TxResult& result) {
        // Find the most significant flow
        std::string bestToken;
        cpp_int bestAmount = 0;
        
        for (const auto& token : tokenOrder) {
            cpp_int absAmount = absInt(netFlow[token]);
            if (absAmount > bestAmount) {
                bestAmount = absAmount;
                bestToken = token;
            }
        }
        
        if (!bestToken.empty()) {
            result.venue = "Transfer";
            result.isSwap = false;
            result.tokenAddr = bestToken;
            result.rawAmount = bestAmount;
            result.isBuy = netFlow[bestToken] > 0;
        }
    }
    
    std::string findMostValuableToken(const std::vector<std::string>& tokens) {
        std::string best;
        cpp_int bestValue = 0;
        
        for (const auto& token : tokens) {
            cpp_int amount = absInt(netFlow[token]);
            int decimals = getDecimals(token);
            if (decimals < 0) decimals = 18;
            
            uint64_t price = getPriceNanos(token);
            cpp_int value = price > 0 ? calcUsdNanos(amount, decimals, price) : amount;
            
            if (value > bestValue) {
                bestValue = value;
                best = token;
            }
        }
        
        return best;
    }
    
    void calculateFinancials(TxResult& result) {
        if (result.tokenAddr.empty()) return;
        
        int decimals = getDecimals(result.tokenAddr);
        if (decimals < 0) decimals = 18;
        
        uint64_t price = getPriceNanos(result.tokenAddr);
        result.usdNanos = calcUsdNanos(result.rawAmount, decimals, price);
        
        // Calculate counter value if available
        if (!result.counterAddr.empty() && result.counterAmount > 0) {
            int counterDecimals = result.counterAddr == chainCtx().nativeMarker 
                ? 18 : getDecimals(result.counterAddr);
            if (counterDecimals < 0) counterDecimals = 18;
            
            uint64_t counterPrice = result.counterAddr == chainCtx().nativeMarker
                ? getPriceNanos(chainCtx().wrappedNative)
                : getPriceNanos(result.counterAddr);
                
            if (counterPrice > 0) {
                result.counterUsdNanos = calcUsdNanos(
                    result.counterAmount, counterDecimals, counterPrice);
            }
        }
    }
};

} // anonymous namespace

// Main analysis function
TxResult analyzeTx(const json& tx, const json& receipt, const std::string& walletArg) {
    const std::string wallet = toLower(walletArg);
    const std::string txHash = tx.contains("hash") && tx["hash"].is_string()
        ? tx["hash"].get<std::string>() : "";
    
    EnhancedTxAnalyzer analyzer(wallet, txHash);
    return analyzer.analyze(tx, receipt);
}
