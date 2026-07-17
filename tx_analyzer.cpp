#include "tx_analyzer.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

using json = nlohmann::json;

// Provided by the existing bot.
uint64_t getPriceNanos(const std::string& token);
int getDecimals(const std::string& address);

namespace {

constexpr std::string_view ZERO_ADDRESS =
    "0x0000000000000000000000000000000000000000";
constexpr std::string_view DEAD_ADDRESS =
    "0x000000000000000000000000000000000000dead";

constexpr std::string_view ERC20_TRANSFER =
    "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
constexpr std::string_view ERC20_APPROVAL =
    "0x8c5be1e5ebec7d5bd14f71427d1e84f3dd0314c0f7b2291e5b200ac8c7c3b925";
constexpr std::string_view V2_SWAP =
    "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822";
constexpr std::string_view V3_SWAP =
    "0xc42079f94a6350d7e6235f29174924f928cc2ac818eb64fed8004e115fbcca67";
constexpr std::string_view PANCAKE_V3_SWAP =
    "0x19b47279256b2a23a1665c810c8d55a1758940ee09377d4f8d26497a3577dc83";
constexpr std::string_view WRAPPED_DEPOSIT =
    "0xe1fffcc4923d04b559f4d29a8bfc6cda04eb5b0d3c460751c2402c5c5cc9109c";
constexpr std::string_view WRAPPED_WITHDRAWAL =
    "0x7fcf532c15f0a6db0bd6d0e038bea71d30d808c7d98cb3bf7268a95bf5081b65";
constexpr std::string_view V3_INCREASE_LIQUIDITY =
    "0x3067048beee31b25b2f1681f88dac838c8bba36af25bfb2b7cf7473a5847e35f";
constexpr std::string_view V3_DECREASE_LIQUIDITY =
    "0x26f6a048be9138f2c0ce266f322cb99228e8d619ae2bff30c67f8dcf9d2377b4";
constexpr std::string_view V3_COLLECT =
    "0x40d0efd1a53d60ecbf40971b9daf7dc90178c3aadc7aab1765632738fa8b8f01";
constexpr std::string_view V2_BURN =
    "0xdccd412f0b1252819cb1fd330b93224ca42612892bb3f4f789976e6d81936496";

struct RawLog {
    std::string address;
    std::string topic0;
    std::vector<std::string> topics;
    std::string data;
    std::size_t index = 0;
};

struct UniversalRouterSignals {
    bool present = false;
    bool swap = false;
    bool wrap = false;
    bool unwrap = false;
    bool permit2 = false;
    bool transfer = false;
    bool v4 = false;
    std::size_t commandCount = 0;
};

struct IndexedLogs {
    std::vector<RawLog> transfers;
    std::vector<RawLog> approvals;
    std::vector<RawLog> swaps;
    std::vector<RawLog> wrappedEvents;
    std::vector<RawLog> v3Liquidity;
    std::vector<RawLog> v2Burns;
    std::set<std::string> swapPools;
};

struct FlowBook {
    std::map<std::string, cpp_int> net;
    std::map<std::string, std::set<std::string>> incomingSources;
    std::map<std::string, std::set<std::string>> outgoingDestinations;
    std::set<std::string> mintedTokens;
    std::set<std::string> burnedTokens;

    cpp_int txNativeOut = 0;
    cpp_int traceNativeIn = 0;
    cpp_int traceNativeInternalOut = 0;
    cpp_int walletWrapped = 0;
    cpp_int walletUnwrapped = 0;

    void receive(
        const std::string& token,
        const cpp_int& amount,
        const std::string& source
    ) {
        if (amount <= 0) return;
        net[token] += amount;
        incomingSources[token].insert(source);
        if (source == ZERO_ADDRESS) mintedTokens.insert(token);
    }

    void send(
        const std::string& token,
        const cpp_int& amount,
        const std::string& destination
    ) {
        if (amount <= 0) return;
        net[token] -= amount;
        outgoingDestinations[token].insert(destination);
        if (destination == ZERO_ADDRESS || destination == DEAD_ADDRESS) {
            burnedTokens.insert(token);
        }
    }
};

std::string lower(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );
    return value;
}

std::string normalizedAddress(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) ||
        object[key].is_null() || !object[key].is_string()) {
        return {};
    }
    return lower(object[key].get<std::string>());
}

std::string topicAddress(const std::string& topic) {
    if (topic.size() != 66 || topic.rfind("0x", 0) != 0) return {};
    return "0x" + lower(topic.substr(26));
}

bool isSwapTopic(const std::string& topic0) {
    return topic0 == V2_SWAP ||
           topic0 == V3_SWAP ||
           topic0 == PANCAKE_V3_SWAP;
}

std::string inputSelector(const std::string& input) {
    if (input.size() < 10 || input.rfind("0x", 0) != 0) return {};
    return lower(input.substr(0, 10));
}

bool selectorIs(
    const std::string& input,
    std::initializer_list<std::string_view> selectors
) {
    const std::string actual = inputSelector(input);
    return std::any_of(
        selectors.begin(),
        selectors.end(),
        [&](std::string_view candidate) {
            return actual == candidate;
        }
    );
}

std::optional<uint64_t> abiWordToU64(
    std::string_view body,
    std::size_t hexOffset
) {
    if (hexOffset + 64 > body.size()) return std::nullopt;

    auto value = parseUint256Opt(body.substr(hexOffset, 64));
    if (!value || *value > std::numeric_limits<uint64_t>::max()) {
        return std::nullopt;
    }
    return value->convert_to<uint64_t>();
}

UniversalRouterSignals parseUniversalRouter(const std::string& input) {
    UniversalRouterSignals signals;

    const std::string selector = inputSelector(input);
    if (selector != "0x3593564c" && selector != "0x24856bc3") {
        return signals;
    }

    if (input.size() < 2 + 8 + 64) return signals;
    const std::string_view body(input.data() + 2, input.size() - 2);

    const auto commandsOffset = abiWordToU64(body, 8);
    if (!commandsOffset) return signals;

    const std::size_t lengthPosition =
        8 + static_cast<std::size_t>(*commandsOffset) * 2;
    const auto length = abiWordToU64(body, lengthPosition);
    if (!length || *length == 0 || *length > 512) return signals;

    const std::size_t commandsPosition = lengthPosition + 64;
    if (commandsPosition + static_cast<std::size_t>(*length) * 2 >
        body.size()) {
        return signals;
    }

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    signals.present = true;
    signals.commandCount = static_cast<std::size_t>(*length);

    for (uint64_t i = 0; i < *length; ++i) {
        const int high = nibble(body[commandsPosition + i * 2]);
        const int low = nibble(body[commandsPosition + i * 2 + 1]);
        if (high < 0 || low < 0) continue;

        /*
         * 0x3f preserves current command IDs while removing flag bits.
         * Unknown future commands are ignored, not misclassified.
         */
        const uint8_t command =
            static_cast<uint8_t>((high << 4) | low) & 0x3f;

        switch (command) {
            case 0x00:
            case 0x01:
            case 0x08:
            case 0x09:
                signals.swap = true;
                break;
            case 0x10:
                signals.swap = true;
                signals.v4 = true;
                break;
            case 0x0b:
                signals.wrap = true;
                break;
            case 0x0c:
                signals.unwrap = true;
                break;
            case 0x02:
            case 0x03:
            case 0x0a:
            case 0x0d:
                signals.permit2 = true;
                break;
            case 0x04:
            case 0x05:
            case 0x06:
                signals.transfer = true;
                break;
            default:
                break;
        }
    }

    return signals;
}

bool receiptFailed(const json& receipt) {
    if (!receipt.is_object() || !receipt.contains("status")) return false;

    const auto& status = receipt["status"];
    if (status.is_string()) return lower(status.get<std::string>()) == "0x0";
    if (status.is_number_integer()) return status.get<int>() == 0;
    return false;
}

IndexedLogs indexLogs(const json& receiptLogs, const ChainContext& chain) {
    IndexedLogs indexed;
    if (!receiptLogs.is_array()) return indexed;

    std::size_t index = 0;
    for (const auto& value : receiptLogs) {
        if (!value.is_object() ||
            !value.contains("topics") ||
            !value["topics"].is_array() ||
            value["topics"].empty() ||
            !value["topics"][0].is_string()) {
            ++index;
            continue;
        }

        RawLog log;
        log.address = normalizedAddress(value, "address");
        log.topic0 = lower(value["topics"][0].get<std::string>());
        log.data = value.value("data", "0x");
        log.index = index++;

        for (const auto& topic : value["topics"]) {
            if (topic.is_string()) {
                log.topics.push_back(
                    lower(topic.get<std::string>())
                );
            }
        }

        if (log.topic0 == ERC20_TRANSFER) {
            indexed.transfers.push_back(log);
        } else if (log.topic0 == ERC20_APPROVAL) {
            indexed.approvals.push_back(log);
        } else if (isSwapTopic(log.topic0)) {
            indexed.swaps.push_back(log);
            if (!log.address.empty()) indexed.swapPools.insert(log.address);
        } else if (
            log.address == chain.wrappedNative &&
            (log.topic0 == WRAPPED_DEPOSIT ||
             log.topic0 == WRAPPED_WITHDRAWAL)
        ) {
            indexed.wrappedEvents.push_back(log);
        } else if (
            log.topic0 == V3_INCREASE_LIQUIDITY ||
            log.topic0 == V3_DECREASE_LIQUIDITY ||
            log.topic0 == V3_COLLECT
        ) {
            indexed.v3Liquidity.push_back(log);
        } else if (log.topic0 == V2_BURN) {
            indexed.v2Burns.push_back(log);
        }
    }

    return indexed;
}

void walkTrace(
    const json& node,
    const std::string& wallet,
    FlowBook& flows,
    bool root
) {
    if (!node.is_object()) return;

    if (!root) {
        const std::string from = normalizedAddress(node, "from");
        const std::string to = normalizedAddress(node, "to");

        if (!from.empty() && !to.empty() &&
            node.contains("value") && node["value"].is_string()) {
            const auto value =
                parseUint256Opt(node["value"].get<std::string>());
            if (value && *value > 0) {
                if (to == wallet && from != wallet) {
                    flows.traceNativeIn += *value;
                }
                if (from == wallet && to != wallet) {
                    flows.traceNativeInternalOut += *value;
                }
            }
        }
    }

    if (node.contains("calls") && node["calls"].is_array()) {
        for (const auto& call : node["calls"]) {
            walkTrace(call, wallet, flows, false);
        }
    }
}

FlowBook reconstructFlows(
    const json& tx,
    const IndexedLogs& indexed,
    const ChainContext& chain,
    const std::string& wallet,
    const json* trace
) {
    FlowBook flows;

    for (const auto& log : indexed.transfers) {
        if (log.topics.size() != 3) continue;

        const std::string from = topicAddress(log.topics[1]);
        const std::string to = topicAddress(log.topics[2]);
        if (from.empty() || to.empty()) continue;

        const auto amount = parseUint256Opt(log.data);
        if (!amount || *amount == 0) continue;

        if (to == wallet) {
            flows.receive(log.address, *amount, from);
        }
        if (from == wallet) {
            flows.send(log.address, *amount, to);
        }
    }

    /*
     * Deposit(address,uint256) / Withdrawal(address,uint256) are only
     * attributed when the indexed account is the watched wallet.
     */
    for (const auto& log : indexed.wrappedEvents) {
        if (log.topics.size() < 2) continue;
        if (topicAddress(log.topics[1]) != wallet) continue;

        const auto amount = parseUint256Opt(log.data);
        if (!amount) continue;

        if (log.topic0 == WRAPPED_DEPOSIT) {
            flows.walletWrapped += *amount;
        } else {
            flows.walletUnwrapped += *amount;
        }
    }

    if (normalizedAddress(tx, "from") == wallet &&
        tx.contains("value") && tx["value"].is_string()) {
        const auto value =
            parseUint256Opt(tx["value"].get<std::string>());
        if (value) flows.txNativeOut = *value;
    }

    if (trace != nullptr && trace->is_object()) {
        walkTrace(*trace, wallet, flows, true);
    }

    return flows;
}

bool anyPositive(const std::map<std::string, cpp_int>& net) {
    return std::any_of(
        net.begin(),
        net.end(),
        [](const auto& item) {
            return item.second > 0;
        }
    );
}

bool anyNegative(const std::map<std::string, cpp_int>& net) {
    return std::any_of(
        net.begin(),
        net.end(),
        [](const auto& item) {
            return item.second < 0;
        }
    );
}

cpp_int magnitude(cpp_int value) {
    return value < 0 ? -value : value;
}

std::string strongestToken(
    const std::map<std::string, cpp_int>& net,
    const ChainContext& chain,
    bool requireNonBase,
    std::optional<bool> positive
) {
    std::string best;
    cpp_int bestMagnitude = -1;

    for (const auto& [token, amount] : net) {
        if (amount == 0) continue;
        if (requireNonBase && chain.baseAssets.count(token) > 0) continue;

        if (positive.has_value()) {
            if (*positive && amount <= 0) continue;
            if (!*positive && amount >= 0) continue;
        }

        const cpp_int currentMagnitude = magnitude(amount);
        if (currentMagnitude > bestMagnitude) {
            best = token;
            bestMagnitude = currentMagnitude;
        }
    }

    return best;
}

cpp_int tokenMagnitude(
    const std::map<std::string, cpp_int>& net,
    const std::string& token
) {
    const auto it = net.find(token);
    if (it == net.end()) return 0;
    return magnitude(it->second);
}

bool tokenFlowTouchesPool(
    const FlowBook& flows,
    const std::string& token,
    bool incoming,
    const std::set<std::string>& pools
) {
    const auto& sourceMap =
        incoming ? flows.incomingSources : flows.outgoingDestinations;
    const auto it = sourceMap.find(token);
    if (it == sourceMap.end()) return false;

    return std::any_of(
        it->second.begin(),
        it->second.end(),
        [&](const std::string& address) {
            return pools.count(address) > 0;
        }
    );
}

std::string venueFromEvidence(
    const ChainContext& chain,
    const std::string& txTo,
    const UniversalRouterSignals& universal,
    const IndexedLogs& indexed
) {
    const auto router = chain.routers.find(txTo);
    if (router != chain.routers.end()) return router->second;

    if (universal.present) {
        if (universal.v4) return "Universal Router V4";
        return "Universal Router";
    }

    if (!indexed.swaps.empty()) {
        const std::string& topic = indexed.swaps.front().topic0;
        if (topic == V2_SWAP) return "Unknown V2 Pool";
        if (topic == V3_SWAP || topic == PANCAKE_V3_SWAP) {
            return "Unknown V3 Pool";
        }
    }

    return {};
}

void addEvidence(TxResult& result, std::string evidence) {
    result.evidence.push_back(std::move(evidence));
}

void setUsdValue(TxResult& result, const ChainContext& chain) {
    if (result.tokenAddr.empty() || result.rawAmount <= 0) return;

    const bool native = result.tokenAddr == chain.nativeMarker;
    const std::string pricedToken =
        native ? chain.wrappedNative : result.tokenAddr;
    const int decimals = native ? 18 : getDecimals(result.tokenAddr);
    const uint64_t price = getPriceNanos(pricedToken);

    result.usdNanos =
        calcUsdNanos(result.rawAmount, decimals, price);
}

TxResult analyzeInternal(
    const json& tx,
    const json& receipt,
    const json* trace,
    const std::string& walletAddress
) {
    TxResult result;
    const ChainContext& chain = chainCtx();
    const std::string wallet = lower(walletAddress);

    if (wallet.size() != 42 || wallet.rfind("0x", 0) != 0) {
        result.unknownReason = "INVALID_WALLET_ADDRESS";
        return result;
    }

    if (!receipt.is_object() ||
        !receipt.contains("logs") ||
        !receipt["logs"].is_array()) {
        result.unknownReason = "MISSING_RECEIPT_LOGS";
        return result;
    }

    result.valid = true;

    if (receiptFailed(receipt)) {
        result.failed = true;
        result.classification = "FAILED";
        result.confidence = "HIGH";
        result.unknownReason.clear();
        return result;
    }

    const std::string txFrom = normalizedAddress(tx, "from");
    const std::string txTo = normalizedAddress(tx, "to");
    const std::string txInput = tx.value("input", "0x");

    result.walletWasSender = txFrom == wallet;
    result.nativeTraceUsed = trace != nullptr;

    const UniversalRouterSignals universal =
        result.walletWasSender
            ? parseUniversalRouter(txInput)
            : UniversalRouterSignals{};

    const IndexedLogs indexed =
        indexLogs(receipt["logs"], chain);

    const FlowBook flows =
        reconstructFlows(tx, indexed, chain, wallet, trace);

    result.hasSwapEvent = !indexed.swaps.empty();
    result.isUniversalRouter = universal.present;
    result.isGenericMulticall =
        inputSelector(txInput) == "0xac9650d8";
    result.hasPermit2Signal =
        universal.permit2 ||
        chain.permit2Contracts.count(txTo) > 0;
    result.lpMintOrBurnSeen =
        !flows.mintedTokens.empty() ||
        !flows.burnedTokens.empty() ||
        !indexed.v2Burns.empty();
    result.lpV3EventSeen =
        !indexed.v3Liquidity.empty();

    result.venue =
        venueFromEvidence(chain, txTo, universal, indexed);

    const bool knownRouter = chain.routers.count(txTo) > 0;
    const bool routerSignal =
        knownRouter ||
        universal.present ||
        result.isGenericMulticall;

    result.dexActivityDetected =
        result.hasSwapEvent ||
        routerSignal ||
        result.hasPermit2Signal;

    if (result.hasSwapEvent) addEvidence(result, "SWAP_EVENT");
    if (knownRouter) addEvidence(result, "KNOWN_ROUTER");
    if (universal.present) addEvidence(result, "UNIVERSAL_ROUTER");
    if (result.hasPermit2Signal) addEvidence(result, "PERMIT2");
    if (trace != nullptr) addEvidence(result, "NATIVE_TRACE");

    /*
     * Pool identity means that a wallet token transfer directly touched
     * one of the contracts that emitted a swap event.
     */
    for (const auto& [token, amount] : flows.net) {
        if (amount > 0 &&
            tokenFlowTouchesPool(
                flows,
                token,
                true,
                indexed.swapPools
            )) {
            result.lpPoolIdentitySeen = true;
            break;
        }
        if (amount < 0 &&
            tokenFlowTouchesPool(
                flows,
                token,
                false,
                indexed.swapPools
            )) {
            result.lpPoolIdentitySeen = true;
            break;
        }
    }

    const bool hasTokenIn = anyPositive(flows.net);
    const bool hasTokenOut = anyNegative(flows.net);
    const bool hasNativeIn = flows.traceNativeIn > 0;
    const bool hasNativeOut =
        flows.txNativeOut > 0 ||
        flows.traceNativeInternalOut > 0;
    const bool anyIn = hasTokenIn || hasNativeIn;
    const bool anyOut = hasTokenOut || hasNativeOut;

    /*
     * Pure wallet-owned wrap/unwrap.
     * Token Transfer events emitted by some wrapped-native contracts are
     * tolerated only when they affect wrapped native itself.
     */
    bool onlyWrappedTokenFlow = true;
    for (const auto& [token, amount] : flows.net) {
        if (amount != 0 && token != chain.wrappedNative) {
            onlyWrappedTokenFlow = false;
            break;
        }
    }

    if (flows.walletWrapped > 0 &&
        flows.walletUnwrapped == 0 &&
        onlyWrappedTokenFlow &&
        !result.hasSwapEvent) {
        result.classification = "WRAP";
        result.confidence = "HIGH";
        result.tokenAddr = chain.wrappedNative;
        result.rawAmount = flows.walletWrapped;
        result.isBuy = true;
        result.venue = "Wrap";
        addEvidence(result, "WALLET_OWNED_DEPOSIT");
        setUsdValue(result, chain);
        return result;
    }

    if (flows.walletUnwrapped > 0 &&
        flows.walletWrapped == 0 &&
        onlyWrappedTokenFlow &&
        !result.hasSwapEvent) {
        result.classification = "UNWRAP";
        result.confidence = "HIGH";
        result.tokenAddr = chain.wrappedNative;
        result.rawAmount = flows.walletUnwrapped;
        result.isBuy = false;
        result.venue = "Unwrap";
        addEvidence(result, "WALLET_OWNED_WITHDRAWAL");
        setUsdValue(result, chain);
        return result;
    }

    if (flows.net.empty() &&
        !indexed.approvals.empty() &&
        flows.txNativeOut == 0) {
        result.classification = "APPROVE";
        result.confidence = "HIGH";
        result.venue = "Approval";
        addEvidence(result, "APPROVAL_EVENT");
        return result;
    }

    /*
     * LP detection is gated by transaction intent plus liquidity evidence.
     * A random liquidity event elsewhere in a multicall is not enough.
     */
    const bool lpAddSelector = selectorIs(
        txInput,
        {
            "0xe8e33700", // addLiquidity
            "0xf305d719", // addLiquidityETH
            "0x88316456", // mint
            "0xfc6f7865"  // increaseLiquidity
        }
    );
    const bool lpRemoveSelector = selectorIs(
        txInput,
        {
            "0xbaa2abde", // removeLiquidity
            "0x02751cec", // removeLiquidityETH
            "0x0c49ccbe", // decreaseLiquidity
            "0x4f1eb3d8"  // burn
        }
    );

    int incomingTokenCount = 0;
    int outgoingTokenCount = 0;
    for (const auto& [token, amount] : flows.net) {
        (void)token;
        if (amount > 0) ++incomingTokenCount;
        if (amount < 0) ++outgoingTokenCount;
    }

    const bool lpAddEvidence =
        lpAddSelector &&
        (
            result.lpV3EventSeen ||
            result.lpMintOrBurnSeen ||
            outgoingTokenCount >= 2
        );
    const bool lpRemoveEvidence =
        lpRemoveSelector &&
        (
            result.lpV3EventSeen ||
            result.lpMintOrBurnSeen ||
            incomingTokenCount >= 2
        );

    if (lpAddEvidence || lpRemoveEvidence) {
        result.classification =
            lpAddEvidence ? "LP_ADD" : "LP_REMOVE";
        result.confidence =
            (result.lpV3EventSeen ||
             result.lpMintOrBurnSeen)
                ? "HIGH"
                : "MEDIUM";
        result.venue =
            lpAddEvidence
                ? "Add Liquidity"
                : "Remove Liquidity";

        result.tokenAddr =
            strongestToken(
                flows.net,
                chain,
                true,
                std::nullopt
            );
        if (result.tokenAddr.empty()) {
            result.tokenAddr =
                strongestToken(
                    flows.net,
                    chain,
                    false,
                    std::nullopt
                );
        }
        result.rawAmount =
            tokenMagnitude(flows.net, result.tokenAddr);
        addEvidence(result, "LP_SELECTOR_AND_FLOW");
        setUsdValue(result, chain);
        return result;
    }

    bool baseIn = hasNativeIn;
    bool baseOut = hasNativeOut;

    for (const auto& [token, amount] : flows.net) {
        if (chain.baseAssets.count(token) == 0) continue;
        if (amount > 0) baseIn = true;
        if (amount < 0) baseOut = true;
    }

    /*
     * A swap requires wallet flow on both sides AND protocol evidence.
     * This avoids classifying a transfer as a swap merely because another
     * internal call emitted a Swap event.
     */
    const bool twoSidedWalletFlow = anyIn && anyOut;
    const bool protocolEvidence =
        result.hasSwapEvent ||
        routerSignal ||
        universal.swap;
    const bool swapCandidate =
        twoSidedWalletFlow &&
        protocolEvidence;

    if (swapCandidate) {
        const std::string receivedNonBase =
            strongestToken(
                flows.net,
                chain,
                true,
                true
            );
        const std::string sentNonBase =
            strongestToken(
                flows.net,
                chain,
                true,
                false
            );

        if (!receivedNonBase.empty() && baseOut) {
            result.isSwap = true;
            result.isBuy = true;
            result.classification = "BUY";
            result.tokenAddr = receivedNonBase;
            result.rawAmount =
                tokenMagnitude(
                    flows.net,
                    receivedNonBase
                );

            result.counterAddr =
                strongestToken(
                    flows.net,
                    chain,
                    false,
                    false
                );
            if (result.counterAddr.empty() && hasNativeOut) {
                result.counterAddr = chain.nativeMarker;
                result.counterAmount =
                    flows.txNativeOut > 0
                        ? flows.txNativeOut
                        : flows.traceNativeInternalOut;
            }
        } else if (!sentNonBase.empty() && baseIn) {
            result.isSwap = true;
            result.isBuy = false;
            result.classification = "SELL";
            result.tokenAddr = sentNonBase;
            result.rawAmount =
                tokenMagnitude(
                    flows.net,
                    sentNonBase
                );

            result.counterAddr =
                strongestToken(
                    flows.net,
                    chain,
                    false,
                    true
                );
            if (result.counterAddr.empty() && hasNativeIn) {
                result.counterAddr = chain.nativeMarker;
                result.counterAmount = flows.traceNativeIn;
            }
        } else {
            /*
             * Non-base to non-base exchange. Keep it as a swap, but mark
             * direction as "received primary token".
             */
            const std::string received =
                strongestToken(
                    flows.net,
                    chain,
                    false,
                    true
                );
            const std::string sent =
                strongestToken(
                    flows.net,
                    chain,
                    false,
                    false
                );

            if (!received.empty() && !sent.empty()) {
                result.isSwap = true;
                result.isBuy = true;
                result.tokenToToken = true;
                result.classification = "TOKEN_TO_TOKEN_SWAP";
                result.tokenAddr = received;
                result.rawAmount =
                    tokenMagnitude(flows.net, received);
                result.counterAddr = sent;
                result.counterAmount =
                    tokenMagnitude(flows.net, sent);
                result.unknownReason =
                    "TOKEN_TO_TOKEN_DIRECTION";
            }
        }

        if (
            !result.counterAddr.empty() &&
            result.counterAmount == 0 &&
            result.counterAddr != chain.nativeMarker
        ) {
            result.counterAmount =
                tokenMagnitude(
                    flows.net,
                    result.counterAddr
                );
        }

        if (result.isSwap) {
            const bool primaryTouchesPool =
                tokenFlowTouchesPool(
                    flows,
                    result.tokenAddr,
                    result.isBuy,
                    indexed.swapPools
                );

            if (
                result.hasSwapEvent &&
                routerSignal &&
                !result.tokenAddr.empty() &&
                !result.counterAddr.empty()
            ) {
                result.confidence = "HIGH";
            } else if (
                result.hasSwapEvent ||
                routerSignal ||
                primaryTouchesPool
            ) {
                result.confidence = "MEDIUM";
            } else {
                result.confidence = "LOW";
            }

            if (result.tokenAddr.empty()) {
                result.unknownReason = "NO_PRIMARY_FLOW";
            } else if (
                result.counterAddr.empty() ||
                result.counterAmount <= 0
            ) {
                result.unknownReason = "NO_COUNTER_FLOW";
            } else if (
                result.venue.empty() &&
                result.hasSwapEvent
            ) {
                result.unknownReason = "UNKNOWN_ROUTER";
            }

            setUsdValue(result, chain);
            return result;
        }
    }

    /*
     * No confident swap. Preserve genuine transfers instead of forcing
     * them into BUY/SELL.
     */
    result.classification = "TRANSFER";
    result.confidence = "HIGH";

    result.tokenAddr =
        strongestToken(
            flows.net,
            chain,
            false,
            std::nullopt
        );

    if (!result.tokenAddr.empty()) {
        result.rawAmount =
            tokenMagnitude(
                flows.net,
                result.tokenAddr
            );
        result.isBuy =
            flows.net.at(result.tokenAddr) > 0;
    } else if (flows.txNativeOut > 0) {
        result.tokenAddr = chain.nativeMarker;
        result.rawAmount = flows.txNativeOut;
        result.isBuy = false;
    } else if (flows.traceNativeIn > 0) {
        result.tokenAddr = chain.nativeMarker;
        result.rawAmount = flows.traceNativeIn;
        result.isBuy = true;
    } else {
        result.classification = "UNKNOWN";
        result.confidence = "LOW";

        if (result.hasSwapEvent) {
            result.unknownReason =
                "SWAP_EVENT_NOT_LINKED_TO_WALLET";
        } else if (routerSignal) {
            result.unknownReason =
                "ROUTER_CALL_WITHOUT_WALLET_FLOW";
        } else if (result.hasPermit2Signal) {
            result.unknownReason =
                "PERMIT2_WITHOUT_WALLET_FLOW";
        } else {
            result.unknownReason =
                "NO_WALLET_FLOW";
        }
    }

    /*
     * If there is DEX evidence but one side is missing, expose the real
     * reason to coverage statistics without inventing a swap.
     */
    if (
        result.classification == "TRANSFER" &&
        result.dexActivityDetected &&
        !twoSidedWalletFlow
    ) {
        result.confidence = "MEDIUM";
        result.unknownReason = "NO_COUNTER_FLOW";
    }

    setUsdValue(result, chain);
    return result;
}

} // namespace

const std::string WBNB_ADDR =
    "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c";
const std::string NATIVE_BNB_MARKER = "native:bnb";

namespace {
ChainContext currentChain = makeBscContext();
}

const ChainContext& chainCtx() {
    return currentChain;
}

void setChainContext(const ChainContext& context) {
    currentChain = context;
}

bool isBaseAsset(const std::string& address) {
    const std::string normalized = lower(address);
    return normalized == currentChain.nativeMarker ||
           currentChain.baseAssets.count(normalized) > 0;
}

bool isStablecoin(const std::string& address) {
    return currentChain.stablecoins.count(lower(address)) > 0;
}

std::string lookupRouterLabel(const std::string& address) {
    const auto it = currentChain.routers.find(lower(address));
    return it == currentChain.routers.end()
        ? std::string{}
        : it->second;
}

std::optional<cpp_int> parseUint256Opt(std::string_view hex) {
    if (
        hex.size() >= 2 &&
        hex[0] == '0' &&
        (hex[1] == 'x' || hex[1] == 'X')
    ) {
        hex.remove_prefix(2);
    }

    if (hex.empty() || hex.size() > 64) return std::nullopt;

    cpp_int result = 0;
    for (const char c : hex) {
        int nibble = -1;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;

        if (nibble < 0) return std::nullopt;
        result = (result << 4) | nibble;
    }

    return result;
}

std::optional<cpp_int> parseInt256Opt(std::string_view hex) {
    auto value = parseUint256Opt(hex);
    if (!value) return std::nullopt;

    const cpp_int signBit = cpp_int(1) << 255;
    const cpp_int modulus = cpp_int(1) << 256;

    if ((*value & signBit) != 0) {
        return *value - modulus;
    }
    return *value;
}

cpp_int parseUint256(const std::string& hex) {
    const auto parsed = parseUint256Opt(hex);
    return parsed.value_or(cpp_int(0));
}

cpp_int hexToCppInt(const std::string& hex) {
    return parseUint256(hex);
}

std::string formatAmount(const cpp_int& raw, int decimals) {
    if (decimals < 0) decimals = 0;

    cpp_int divisor = 1;
    for (int i = 0; i < decimals; ++i) divisor *= 10;

    const cpp_int integer = divisor == 0 ? raw : raw / divisor;
    cpp_int fraction = divisor == 0 ? 0 : raw % divisor;
    if (fraction < 0) fraction = -fraction;

    std::string fractionText = fraction.convert_to<std::string>();
    while (
        static_cast<int>(fractionText.size()) < decimals
    ) {
        fractionText.insert(fractionText.begin(), '0');
    }

    if (fractionText.size() > 2) {
        fractionText.resize(2);
    }
    while (fractionText.size() < 2) {
        fractionText.push_back('0');
    }

    return integer.convert_to<std::string>() +
           "." +
           fractionText;
}

cpp_int calcUsdNanos(
    const cpp_int& raw,
    int decimals,
    uint64_t priceNanos
) {
    if (priceNanos == 0 || decimals < 0) return 0;

    cpp_int divisor = 1;
    for (int i = 0; i < decimals; ++i) divisor *= 10;

    return (raw * priceNanos) / divisor;
}

std::string formatUsd(const cpp_int& nanos) {
    const bool negative = nanos < 0;
    cpp_int absolute = negative ? -nanos : nanos;

    std::string text = absolute.convert_to<std::string>();
    while (text.size() < 10) text.insert(text.begin(), '0');

    const std::string dollars =
        text.substr(0, text.size() - 9);
    const std::string cents =
        text.substr(text.size() - 9, 2);

    return std::string(negative ? "-$" : "$") +
           (dollars.empty() ? "0" : dollars) +
           "." +
           cents;
}

cpp_int calcUnitPriceNanos(
    const cpp_int& usdNanos,
    const cpp_int& rawAmount,
    int decimals
) {
    if (rawAmount <= 0 || decimals < 0) return 0;

    cpp_int divisor = 1;
    for (int i = 0; i < decimals; ++i) divisor *= 10;

    return (usdNanos * divisor) / rawAmount;
}

std::string formatPriceUsd(const cpp_int& nanos) {
    const bool negative = nanos < 0;
    cpp_int absolute = negative ? -nanos : nanos;

    std::string text = absolute.convert_to<std::string>();
    while (text.size() < 10) text.insert(text.begin(), '0');

    std::string dollars =
        text.substr(0, text.size() - 9);
    std::string fraction =
        text.substr(text.size() - 9);

    const std::size_t lastNonZero =
        fraction.find_last_not_of('0');
    std::size_t keep =
        lastNonZero == std::string::npos
            ? 2
            : std::max<std::size_t>(2, lastNonZero + 1);
    fraction.resize(keep);

    return std::string(negative ? "-$" : "$") +
           (dollars.empty() ? "0" : dollars) +
           "." +
           fraction;
}

static ChainContext makeBaseChain(
    std::string displayName,
    std::string explorerUrl,
    std::string explorerName,
    std::string nativeSymbol,
    std::string nativeMarker,
    std::string wrappedNative
) {
    ChainContext context;
    context.displayName = std::move(displayName);
    context.explorerUrl = std::move(explorerUrl);
    context.explorerName = std::move(explorerName);
    context.nativeSymbol = std::move(nativeSymbol);
    context.nativeMarker = std::move(nativeMarker);
    context.wrappedNative = lower(std::move(wrappedNative));
    context.baseAssets.insert(context.wrappedNative);
    context.permit2Contracts.insert(
        "0x000000000022d473030f116ddee9f6b43ac78ba3"
    );
    return context;
}

ChainContext makeBscContext() {
    ChainContext context = makeBaseChain(
        "BNB Smart Chain",
        "https://bscscan.com",
        "BscScan",
        "BNB",
        NATIVE_BNB_MARKER,
        WBNB_ADDR
    );

    context.stablecoins = {
        "0x55d398326f99059ff775485246999027b3197955",
        "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d",
        "0xe9e7cea3dedca5984780bafc599bd69add087d56"
    };
    context.baseAssets.insert(
        context.stablecoins.begin(),
        context.stablecoins.end()
    );
    context.baseAssets.insert(
        "0xc5f0f7b66764f6ec8c8dff7ba683102295e16409"
    );

    context.routers = {
        {"0x10ed43c718714eb63d5aa57b78b54704e256024e",
         "PancakeSwap V2"},
        {"0x13f4ea83d0bd40e75c8222255bc855a974568dd4",
         "PancakeSwap V3 Smart Router"},
        {"0x1b81d678ffb9c0263b24a97847620c99d213eb14",
         "PancakeSwap V3 Swap Router"},
        {"0x1a0a18ac4becddbd6389559687d1a73d8927e416",
         "PancakeSwap Universal Router"},
        {"0xd9c500dff816a1da21a48a732d3498bf09dc9aeb",
         "PancakeSwap Universal Router 2"},
        {"0x5dc88340e1c5c6366864ee415d6034cadd1a9897",
         "Uniswap Universal Router"},
        {"0xec8b0f7ffe3ae75d7ffab09429e3675bb63503e4",
         "Uniswap Universal Router"},
        {"0x1906c1d672b88cd1b9ac7593301ca990f94eae07",
         "Uniswap V4 Universal Router"},
        {"0x1111111254eeb25477b68fb85ed929f73a960582",
         "1inch"},
        {"0x9333c74bdd1e118634fe5664aca7a9710b108bab",
         "OKX DEX"},
        {"0x6015126d7d23648c2e4466693b8deab005ffaba8",
         "OKX DEX"},
        {"0x6131b5fae19ea4f9d964eac0408e4408b66337b5",
         "KyberSwap"},
        {"0xdf1a1b60f2d438842916c0adc43748768353ec25",
         "KyberSwap"},
        {"0x6352a56caadc4f1e25cd6c75970fa768a3304e64",
         "OpenOcean"},
        {"0x3a6d8ca21d1cf76f653a67577fa0d27453350dd8",
         "BiSwap"},
        {"0xcf0febd3f17cef5b47b0cd257acf6025c5bff3b7",
         "ApeSwap"},
        {"0xcde540d7eafe93ac5fe6233bee57e1270d3e330f",
         "BakerySwap"},
        {"0x9f138be5aa5cc442ea7cc7d18cd9e30593ed90b9",
         "Odos"},
        {"0x8f8dd7db1bda5ed3da8c9daf3bfa471c12d58486",
         "DODO"},
        {"0x114f84658c99aa6ea62e3160a87a16deaf7efe83",
         "WOOFi"},
        {"0xcef5be73ae943b77f9bc08859367d923c030a269",
         "WOOFi"}
    };

    return context;
}

ChainContext makeEthereumContext() {
    ChainContext context = makeBaseChain(
        "Ethereum",
        "https://etherscan.io",
        "Etherscan",
        "ETH",
        "native:eth",
        "0xc02aaa39b223fe8d0a0e5c4f27ead9083c756cc2"
    );

    context.stablecoins = {
        "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
        "0xdac17f958d2ee523a2206206994597c13d831ec7",
        "0x6b175474e89094c44da98b954eedeac495271d0f"
    };
    context.baseAssets.insert(
        context.stablecoins.begin(),
        context.stablecoins.end()
    );

    context.routers = {
        {"0x7a250d5630b4cf539739df2c5dacb4c659f2488d",
         "Uniswap V2"},
        {"0xe592427a0aece92de3edee1f18e0157c05861564",
         "Uniswap V3"},
        {"0x68b3465833fb72a70ecdf485e0e4c7bd8665fc45",
         "Uniswap V3 Router 2"},
        {"0xef1c6e67703c7bd7107eed8303fbe6ec2554bf6b",
         "Uniswap Universal Router"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad",
         "Uniswap Universal Router"},
        {"0x66a9893cc07d91d95644aedd05d03f95e1dba8af",
         "Uniswap V4 Universal Router"}
    };

    context.positionManagers = {
        "0xc36442b4a4522e871399cd717abdd847ab11fe88"
    };

    return context;
}

ChainContext makeBaseContext() {
    ChainContext context = makeBaseChain(
        "Base",
        "https://basescan.org",
        "BaseScan",
        "ETH",
        "native:eth",
        "0x4200000000000000000000000000000000000006"
    );

    context.stablecoins = {
        "0x833589fcd6edb6e08f4c7c32d4f71b54bda02913",
        "0xfde4c96c8593536e31f229ea8f37b2ada2699bb2",
        "0x50c5725949a6f0c72e6c4a641f24049a917db0cb"
    };
    context.baseAssets.insert(
        context.stablecoins.begin(),
        context.stablecoins.end()
    );

    context.routers = {
        {"0x198ef79f1f515f02dfe9e3115ed9fc07183f02fc",
         "Uniswap Universal Router"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad",
         "Uniswap Universal Router"},
        {"0x6ff5693b99212da76ad316178a184ab56d299b43",
         "Uniswap V4 Universal Router"}
    };

    return context;
}

ChainContext makeArbitrumContext() {
    ChainContext context = makeBaseChain(
        "Arbitrum One",
        "https://arbiscan.io",
        "Arbiscan",
        "ETH",
        "native:eth",
        "0x82af49447d8a07e3bd95bd0d56f35241523fbab1"
    );

    context.stablecoins = {
        "0xaf88d065e77c8cc2239327c5edb3a432268e5831",
        "0xfd086bc7cd5c481dcc9c85ebe478a1c0b69fcbb9",
        "0xda10009cbd5d07dd0cecc66161fc93d7c9000da1"
    };
    context.baseAssets.insert(
        context.stablecoins.begin(),
        context.stablecoins.end()
    );

    context.routers = {
        {"0x4c60051384bd2d3c01bfc845cf5f4b44bcbe9de5",
         "Uniswap Universal Router"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad",
         "Uniswap Universal Router"},
        {"0xe592427a0aece92de3edee1f18e0157c05861564",
         "Uniswap V3"},
        {"0x68b3465833fb72a70ecdf485e0e4c7bd8665fc45",
         "Uniswap V3 Router 2"}
    };

    return context;
}

TxResult analyzeTx(
    const json& tx,
    const json& receipt,
    const std::string& wallet
) {
    return analyzeInternal(
        tx,
        receipt,
        nullptr,
        wallet
    );
}

TxResult analyzeTxWithTrace(
    const json& tx,
    const json& receipt,
    const json& trace,
    const std::string& wallet
) {
    return analyzeInternal(
        tx,
        receipt,
        &trace,
        wallet
    );
}
