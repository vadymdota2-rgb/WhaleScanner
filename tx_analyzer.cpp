#include "tx_analyzer.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "utils.h"

using json = nlohmann::json;

// Implemented by main.cpp.
uint64_t getPriceNanos(const std::string& token);
int getDecimals(const std::string& address);
std::string getSymbol(const std::string& address);

const std::string WBNB_ADDR =
    "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c";
const std::string NATIVE_BNB_MARKER = "native:bnb";

namespace {

const std::string ZERO_ADDRESS =
    "0x0000000000000000000000000000000000000000";
const std::string DEAD_ADDRESS =
    "0x000000000000000000000000000000000000dead";

const std::string ERC20_TRANSFER_TOPIC =
    "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
const std::string ERC20_APPROVAL_TOPIC =
    "0x8c5be1e5ebec7d5bd14f71427d1e84f3dd0314c0f7b2291e5b200ac8c7c3b925";

const std::string V2_SWAP_TOPIC =
    "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822";
const std::string V3_SWAP_TOPIC =
    "0xc42079f94a6350d7e6235f29174924f928cc2ac818eb64fed8004e115fbcca67";
const std::string PANCAKE_V3_SWAP_TOPIC =
    "0x19b47279256b2a23a1665c810c8d55a1758940ee09377d4f8d26497a3577dc83";

const std::string WRAPPED_DEPOSIT_TOPIC =
    "0xe1fffcc4923d04b559f4d29a8bfc6cda04eb5b0d3c460751c2402c5c5cc9109c";
const std::string WRAPPED_WITHDRAW_TOPIC =
    "0x7fcf532c15f0a6db0bd6d0e038bea71d30d808c7d98cb3bf7268a95bf5081b65";

const std::string V3_INCREASE_LIQUIDITY_TOPIC =
    "0x3067048beee31b25b2f1681f88dac838c8bba36af25bfb2b7cf7473a5847e35f";
const std::string V3_DECREASE_LIQUIDITY_TOPIC =
    "0x26f6a048ee9138f2c0ce266f322cb99228e8d619ae2bff30c67f8dcf9d2377b4";
const std::string V3_COLLECT_TOPIC =
    "0x40d0efd1a53d60ecbf40971b9daf7dc90178c3aadc7aab1765632738fa8b8f01";

ChainContext g_chain = makeBscContext();

std::optional<cpp_int> parseHex(
    std::string_view value,
    size_t maxDigits = 64
) {
    if (value.size() >= 2 &&
        value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
    }

    if (value.empty() || value.size() > maxDigits)
        return std::nullopt;

    cpp_int result = 0;

    for (const char c : value) {
        int digit = -1;

        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return std::nullopt;

        result <<= 4;
        result |= digit;
    }

    return result;
}

cpp_int tenPow(int decimals) {
    if (decimals < 0)
        decimals = 0;
    if (decimals > 255)
        decimals = 255;

    cpp_int value = 1;
    for (int i = 0; i < decimals; ++i)
        value *= 10;

    return value;
}

std::string addressFromTopic(const std::string& topic) {
    if (topic.size() != 66 ||
        topic[0] != '0' ||
        (topic[1] != 'x' && topic[1] != 'X')) {
        return {};
    }

    return "0x" + toLower(topic.substr(26, 40));
}

std::string txString(
    const json& object,
    const char* key
) {
    if (!object.is_object() ||
        !object.contains(key) ||
        !object[key].is_string()) {
        return {};
    }

    return object[key].get<std::string>();
}

std::string selectorOf(const std::string& input) {
    if (input.size() < 10 ||
        input[0] != '0' ||
        (input[1] != 'x' && input[1] != 'X')) {
        return {};
    }

    return toLower(input.substr(0, 10));
}

bool isSwapTopic(const std::string& topic) {
    return topic == V2_SWAP_TOPIC ||
           topic == V3_SWAP_TOPIC ||
           topic == PANCAKE_V3_SWAP_TOPIC;
}

bool isUniversalRouterSelector(const std::string& selector) {
    return selector == "0x3593564c" ||
           selector == "0x24856bc3";
}

bool isGenericMulticallSelectorValue(const std::string& selector) {
    return selector == "0xac9650d8" || // Uniswap multicall(bytes[])
           selector == "0x5ae401dc" || // multicall(uint256,bytes[])
           selector == "0x1f0464d1" || // multicall(bytes32,bytes[])
           selector == "0x82ad56cb" || // Multicall3 aggregate3
           selector == "0xbce38bd7" || // Multicall3 aggregate3Value
           selector == "0x252dba42" || // aggregate
           selector == "0xbce38bd7";
}

bool isSwapSelector(const std::string& selector) {
    static const std::set<std::string> selectors = {
        // V2 routers
        "0x38ed1739", "0x8803dbee", "0x7ff36ab5",
        "0x18cbafe5", "0xfb3bdb41", "0x4a25d94a",
        "0x5c11d795", "0xb6f9de95", "0x791ac947",
        "0x472b43f3", "0x42712a67",

        // V3 routers
        "0x414bf389", "0xc04b8d59", "0xb858183f",
        "0x5023b4df", "0x04e45aaf", "0xdb3e2198",
        "0x42712a67",

        // Universal router
        "0x3593564c", "0x24856bc3",

        // Common aggregators
        "0x12aa3caf", // 1inch swap
        "0x0502b1c5", // unoswap
        "0xe449022e", // unoswapTo
        "0x2e95b6c8",
        "0x7c025200", // 1inch legacy swap
        "0x90411a32",
        "0x54e3f31b",
        "0x6af479b2",
        "0x2213bc0b",
        "0x83bd37f9",
        "0x89a15321",
        "0x2298207a",
        "0xa94e78ef",

        // DODO / WOOFi / generic swap names
        "0x1e9d36b7",
        "0x7b939232",
        "0x5f575529",
        "0xe0e189a0",
        "0x84bd6d29"
    };

    return selectors.count(selector) > 0;
}

bool isAddLiquiditySelector(const std::string& selector) {
    static const std::set<std::string> selectors = {
        "0xe8e33700", // addLiquidity
        "0xf305d719", // addLiquidityETH
        "0x88316456", // V3 position manager mint
        "0xfc6f7865"  // increaseLiquidity
    };
    return selectors.count(selector) > 0;
}

bool isRemoveLiquiditySelector(const std::string& selector) {
    static const std::set<std::string> selectors = {
        "0xbaa2abde", "0x02751cec", "0xaf2979eb",
        "0x5b0d5984", "0xded9382a", "0x2195995c",
        "0x0c49ccbe"  // decreaseLiquidity
    };
    return selectors.count(selector) > 0;
}

bool isApproveSelector(const std::string& selector) {
    return selector == "0x095ea7b3" ||
           selector == "0xa22cb465";
}

bool isWrapSelector(const std::string& selector) {
    return selector == "0xd0e30db0";
}

bool isUnwrapSelector(const std::string& selector) {
    return selector == "0x2e1a7d4d";
}

struct UniversalRouterCommands {
    bool present = false;
    bool hasSwap = false;
    bool hasV2Swap = false;
    bool hasV3Swap = false;
    bool hasV4Swap = false;
    bool hasWrap = false;
    bool hasUnwrap = false;
    bool hasSweep = false;
    bool hasPermit2 = false;
};

int hexNibble(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<uint64_t> readAbiWordU64(
    const std::string& inputWithout0x,
    size_t hexOffset
) {
    if (hexOffset > inputWithout0x.size() ||
        inputWithout0x.size() - hexOffset < 64) {
        return std::nullopt;
    }

    uint64_t value = 0;
    const size_t begin = hexOffset + 48;

    for (size_t i = begin; i < hexOffset + 64; ++i) {
        const int nibble = hexNibble(inputWithout0x[i]);
        if (nibble < 0)
            return std::nullopt;

        value = (value << 4) |
                static_cast<uint64_t>(nibble);
    }

    return value;
}

UniversalRouterCommands decodeUniversalRouter(
    const std::string& input
) {
    UniversalRouterCommands commands;
    const std::string selector = selectorOf(input);

    if (!isUniversalRouterSelector(selector))
        return commands;

    commands.present = true;

    if (input.size() < 10 + 64)
        return commands;

    const std::string data = input.substr(2);

    const auto commandsOffset = readAbiWordU64(data, 8);
    if (!commandsOffset)
        return commands;

    if (*commandsOffset >
        (std::numeric_limits<size_t>::max() - 8) / 2) {
        return commands;
    }

    const size_t lengthPosition =
        8 + static_cast<size_t>(*commandsOffset) * 2;

    const auto length = readAbiWordU64(data, lengthPosition);
    if (!length || *length == 0 || *length > 512)
        return commands;

    const size_t commandDataPosition =
        lengthPosition + 64;

    if (commandDataPosition > data.size() ||
        *length > (data.size() - commandDataPosition) / 2) {
        return commands;
    }

    for (uint64_t i = 0; i < *length; ++i) {
        const size_t position =
            commandDataPosition +
            static_cast<size_t>(i) * 2;

        const int high = hexNibble(data[position]);
        const int low = hexNibble(data[position + 1]);

        if (high < 0 || low < 0)
            return UniversalRouterCommands{};

        const unsigned byte =
            static_cast<unsigned>((high << 4) | low);

        // Bit 7 is allow-revert. The command id is in the low 6 bits.
        const unsigned command = byte & 0x3fU;

        switch (command) {
            case 0x00: // V3_SWAP_EXACT_IN
            case 0x01: // V3_SWAP_EXACT_OUT
                commands.hasSwap = true;
                commands.hasV3Swap = true;
                break;

            case 0x08: // V2_SWAP_EXACT_IN
            case 0x09: // V2_SWAP_EXACT_OUT
                commands.hasSwap = true;
                commands.hasV2Swap = true;
                break;

            case 0x10: // V4_SWAP
                commands.hasSwap = true;
                commands.hasV4Swap = true;
                break;

            case 0x0b:
                commands.hasWrap = true;
                break;

            case 0x0c:
                commands.hasUnwrap = true;
                break;

            case 0x04:
                commands.hasSweep = true;
                break;

            case 0x02:
            case 0x03:
            case 0x0a:
            case 0x0d:
                commands.hasPermit2 = true;
                break;

            default:
                break;
        }
    }

    return commands;
}

struct TokenFlow {
    cpp_int net = 0;
    cpp_int incoming = 0;
    cpp_int outgoing = 0;
    std::set<std::string> sources;
    std::set<std::string> destinations;
    bool minted = false;
    bool burned = false;
};

struct ReceiptState {
    std::map<std::string, TokenFlow> flows;
    std::vector<std::string> tokenOrder;

    std::set<std::string> swapPools;
    bool hasSwapEvent = false;

    bool hasApproval = false;
    bool hasWalletTransfer = false;

    cpp_int walletWrapped = 0;
    cpp_int walletUnwrapped = 0;
    cpp_int transactionWrapped = 0;
    cpp_int transactionUnwrapped = 0;

    bool v3Increase = false;
    bool v3Decrease = false;
    bool v3Collect = false;
};

void touchToken(
    ReceiptState& state,
    const std::string& token
) {
    if (state.flows.find(token) == state.flows.end()) {
        state.flows.emplace(token, TokenFlow{});
        state.tokenOrder.push_back(token);
    }
}

void indexReceipt(
    ReceiptState& state,
    const json& receipt,
    const std::string& wallet
) {
    for (const auto& log : receipt["logs"]) {
        if (!log.is_object() ||
            !log.contains("topics") ||
            !log["topics"].is_array() ||
            log["topics"].empty() ||
            !log["topics"][0].is_string()) {
            continue;
        }

        const std::string topic0 =
            toLower(log["topics"][0].get<std::string>());

        const std::string emitter =
            toLower(txString(log, "address"));

        if (isSwapTopic(topic0)) {
            state.hasSwapEvent = true;
            if (!emitter.empty())
                state.swapPools.insert(emitter);
            continue;
        }

        if (topic0 == V3_INCREASE_LIQUIDITY_TOPIC) {
            state.v3Increase = true;
            continue;
        }

        if (topic0 == V3_DECREASE_LIQUIDITY_TOPIC) {
            state.v3Decrease = true;
            continue;
        }

        if (topic0 == V3_COLLECT_TOPIC) {
            state.v3Collect = true;
            continue;
        }

        if (emitter == g_chain.wrappedNative &&
            (topic0 == WRAPPED_DEPOSIT_TOPIC ||
             topic0 == WRAPPED_WITHDRAW_TOPIC)) {
            const auto amount = parseHex(txString(log, "data"));
            if (!amount || *amount <= 0)
                continue;

            std::string account;
            if (log["topics"].size() >= 2 &&
                log["topics"][1].is_string()) {
                account = addressFromTopic(
                    log["topics"][1].get<std::string>()
                );
            }

            if (topic0 == WRAPPED_DEPOSIT_TOPIC) {
                state.transactionWrapped += *amount;
                if (account == wallet)
                    state.walletWrapped += *amount;
            } else {
                state.transactionUnwrapped += *amount;
                if (account == wallet)
                    state.walletUnwrapped += *amount;
            }
            continue;
        }

        if (topic0 == ERC20_APPROVAL_TOPIC) {
            if (log["topics"].size() >= 3 &&
                log["topics"][1].is_string()) {
                const std::string owner = addressFromTopic(
                    log["topics"][1].get<std::string>()
                );
                if (owner == wallet)
                    state.hasApproval = true;
            }
            continue;
        }

        if (topic0 != ERC20_TRANSFER_TOPIC ||
            log["topics"].size() != 3 ||
            !log["topics"][1].is_string() ||
            !log["topics"][2].is_string()) {
            continue;
        }

        const std::string from = addressFromTopic(
            log["topics"][1].get<std::string>()
        );
        const std::string to = addressFromTopic(
            log["topics"][2].get<std::string>()
        );

        if (from != wallet && to != wallet)
            continue;

        const auto amount = parseHex(txString(log, "data"));
        if (!amount || *amount <= 0 || emitter.empty())
            continue;

        touchToken(state, emitter);
        TokenFlow& flow = state.flows[emitter];
        state.hasWalletTransfer = true;

        if (to == wallet) {
            flow.net += *amount;
            flow.incoming += *amount;
            flow.sources.insert(from);
            flow.minted = from == ZERO_ADDRESS;
        }

        if (from == wallet) {
            flow.net -= *amount;
            flow.outgoing += *amount;
            flow.destinations.insert(to);
            flow.burned =
                to == ZERO_ADDRESS ||
                to == DEAD_ADDRESS;
        }
    }
}

cpp_int absolute(const cpp_int& value) {
    return value < 0 ? -value : value;
}

bool sourceOrDestinationIsPool(
    const TokenFlow& flow,
    const std::set<std::string>& pools
) {
    for (const auto& source : flow.sources) {
        if (pools.count(source))
            return true;
    }

    for (const auto& destination : flow.destinations) {
        if (pools.count(destination))
            return true;
    }

    return false;
}

std::string selectorVenue(
    const std::string& selector
) {
    if (isUniversalRouterSelector(selector))
        return "Universal Router";

    static const std::set<std::string> v2 = {
        "0x38ed1739", "0x8803dbee", "0x7ff36ab5",
        "0x18cbafe5", "0xfb3bdb41", "0x4a25d94a",
        "0x5c11d795", "0xb6f9de95", "0x791ac947"
    };

    static const std::set<std::string> v3 = {
        "0x414bf389", "0xc04b8d59", "0xb858183f",
        "0x5023b4df", "0x04e45aaf", "0xdb3e2198"
    };

    if (v2.count(selector))
        return "V2 Router";
    if (v3.count(selector))
        return "V3 Router";
    if (isSwapSelector(selector))
        return "DEX Aggregator";

    return {};
}

std::string detectVenue(
    const std::string& transactionTarget,
    const std::string& selector,
    const UniversalRouterCommands& universal,
    const ReceiptState& receipt
) {
    std::string venue = lookupRouterLabel(transactionTarget);
    if (!venue.empty())
        return venue;

    venue = selectorVenue(selector);
    if (!venue.empty()) {
        if (universal.hasV4Swap)
            return "Universal Router (V4)";
        if (universal.hasV3Swap)
            return "Universal Router (V3)";
        if (universal.hasV2Swap)
            return "Universal Router (V2)";
        return venue;
    }

    for (const auto& pool : receipt.swapPools) {
        venue = lookupRouterLabel(pool);
        if (!venue.empty())
            return venue;
    }

    if (receipt.hasSwapEvent)
        return "DEX Pool";

    return {};
}

struct Candidate {
    std::string token;
    cpp_int net = 0;
    cpp_int magnitude = 0;
    bool poolLinked = false;
};

std::optional<Candidate> choosePrimaryNonBase(
    const ReceiptState& receipt,
    bool baseIncoming,
    bool baseOutgoing,
    bool nativeIncoming,
    bool nativeOutgoing
) {
    std::optional<Candidate> best;
    int bestScore = std::numeric_limits<int>::min();

    for (const auto& token : receipt.tokenOrder) {
        if (isBaseAsset(token))
            continue;

        const TokenFlow& flow = receipt.flows.at(token);
        if (flow.net == 0)
            continue;

        const bool coherent =
            (flow.net > 0 && (baseOutgoing || nativeOutgoing)) ||
            (flow.net < 0 && (baseIncoming || nativeIncoming));

        const bool poolLinked =
            sourceOrDestinationIsPool(
                flow,
                receipt.swapPools
            );

        int score = 0;
        if (coherent) score += 100;
        if (poolLinked) score += 40;
        if (!flow.minted && !flow.burned) score += 10;

        if (!best ||
            score > bestScore ||
            (score == bestScore &&
             absolute(flow.net) > best->magnitude)) {
            best = Candidate{
                token,
                flow.net,
                absolute(flow.net),
                poolLinked
            };
            bestScore = score;
        }
    }

    return best;
}

std::optional<Candidate> chooseLargestFlow(
    const ReceiptState& receipt
) {
    std::optional<Candidate> best;

    for (const auto& token : receipt.tokenOrder) {
        const TokenFlow& flow = receipt.flows.at(token);
        if (flow.net == 0)
            continue;

        const cpp_int magnitude = absolute(flow.net);
        if (!best || magnitude > best->magnitude) {
            best = Candidate{
                token,
                flow.net,
                magnitude,
                sourceOrDestinationIsPool(
                    flow,
                    receipt.swapPools
                )
            };
        }
    }

    return best;
}

void setCounterFromWalletFlows(
    TxResult& result,
    const ReceiptState& receipt,
    bool nativeOutgoing,
    const cpp_int& nativeOut,
    bool nativeIncoming,
    const cpp_int& nativeIn
) {
    std::string bestToken;
    cpp_int bestAmount = 0;

    for (const auto& token : receipt.tokenOrder) {
        if (token == result.tokenAddr)
            continue;

        const TokenFlow& flow = receipt.flows.at(token);
        const bool correctDirection =
            result.isBuy
            ? flow.net < 0
            : flow.net > 0;

        if (!correctDirection)
            continue;

        // Prefer quote assets, but still support token-to-token swaps.
        cpp_int scoreAmount = absolute(flow.net);
        const bool better =
            bestToken.empty() ||
            (isBaseAsset(token) &&
             !isBaseAsset(bestToken)) ||
            (isBaseAsset(token) ==
             isBaseAsset(bestToken) &&
             scoreAmount > bestAmount);

        if (better) {
            bestToken = token;
            bestAmount = scoreAmount;
        }
    }

    if (!bestToken.empty()) {
        result.counterAddr = bestToken;
        result.counterAmount = bestAmount;
        return;
    }

    if (result.isBuy && nativeOutgoing && nativeOut > 0) {
        result.counterAddr = g_chain.nativeMarker;
        result.counterAmount = nativeOut;
        return;
    }

    if (!result.isBuy && nativeIncoming && nativeIn > 0) {
        result.counterAddr = g_chain.nativeMarker;
        result.counterAmount = nativeIn;
    }
}

bool looksLikeLpToken(
    const std::string& token,
    const TokenFlow& flow,
    const ReceiptState& receipt
) {
    if (flow.minted || flow.burned)
        return true;

    if (receipt.swapPools.count(token))
        return true;

    return false;
}

void classifyLiquidity(
    TxResult& result,
    const ReceiptState& receipt,
    bool selectorAdd,
    bool selectorRemove
) {
    bool outgoingBase = false;
    bool outgoingNonBase = false;
    bool incomingBase = false;
    bool incomingNonBase = false;
    bool mintedLp = false;
    bool burnedLp = false;

    for (const auto& token : receipt.tokenOrder) {
        const TokenFlow& flow = receipt.flows.at(token);

        if (isBaseAsset(token)) {
            outgoingBase = outgoingBase || flow.net < 0;
            incomingBase = incomingBase || flow.net > 0;
        } else {
            outgoingNonBase = outgoingNonBase || flow.net < 0;
            incomingNonBase = incomingNonBase || flow.net > 0;
        }

        if (flow.net > 0 &&
            looksLikeLpToken(token, flow, receipt)) {
            mintedLp = true;
        }

        if (flow.net < 0 &&
            looksLikeLpToken(token, flow, receipt)) {
            burnedLp = true;
        }
    }

    const bool add =
        receipt.v3Increase ||
        selectorAdd ||
        (mintedLp &&
         (outgoingBase || outgoingNonBase));

    const bool remove =
        receipt.v3Decrease ||
        receipt.v3Collect ||
        selectorRemove ||
        (burnedLp &&
         (incomingBase || incomingNonBase));

    if (!add && !remove)
        return;

    result.isSwap = false;
    result.venue = add && !remove
        ? "Add Liquidity"
        : "Remove Liquidity";

    std::optional<Candidate> primary;

    for (const auto& token : receipt.tokenOrder) {
        const TokenFlow& flow = receipt.flows.at(token);
        if (flow.net == 0)
            continue;

        if (add && flow.net < 0 && !isBaseAsset(token)) {
            const cpp_int magnitude = absolute(flow.net);
            if (!primary || magnitude > primary->magnitude) {
                primary = Candidate{
                    token,
                    flow.net,
                    magnitude,
                    false
                };
            }
        }

        if (!add && flow.net > 0 && !isBaseAsset(token)) {
            const cpp_int magnitude = absolute(flow.net);
            if (!primary || magnitude > primary->magnitude) {
                primary = Candidate{
                    token,
                    flow.net,
                    magnitude,
                    false
                };
            }
        }
    }

    if (!primary)
        primary = chooseLargestFlow(receipt);

    if (primary) {
        result.tokenAddr = primary->token;
        result.rawAmount = primary->magnitude;
        result.isBuy = primary->net > 0;
    }
}

void calculateUsd(TxResult& result) {
    if (result.tokenAddr.empty() ||
        result.rawAmount <= 0) {
        result.usdNanos = 0;
        return;
    }

    const bool native =
        result.tokenAddr == g_chain.nativeMarker;

    const std::string priceToken =
        native
        ? g_chain.wrappedNative
        : result.tokenAddr;

    int decimals = native
        ? 18
        : getDecimals(result.tokenAddr);

    if (decimals < 0 || decimals > 255)
        decimals = 18;

    result.usdNanos = calcUsdNanos(
        result.rawAmount,
        decimals,
        getPriceNanos(priceToken)
    );
}

} // namespace

cpp_int parseUint256(const std::string& hex) {
    return parseHex(hex, 64).value_or(cpp_int(0));
}

cpp_int hexToCppInt(const std::string& hex) {
    return parseHex(hex, 256).value_or(cpp_int(0));
}

std::string formatAmount(
    const cpp_int& raw,
    int decimals
) {
    const bool negative = raw < 0;
    const cpp_int amount = negative ? -raw : raw;
    const cpp_int divisor = tenPow(decimals);

    const cpp_int integerPartValue = amount / divisor;
    const cpp_int fractionPartValue = amount % divisor;

    std::string integerPart =
        integerPartValue.convert_to<std::string>();

    if (decimals <= 0)
        return (negative ? "-" : "") +
               integerPart + ".00";

    std::string fractionPart =
        fractionPartValue.convert_to<std::string>();

    while (fractionPart.size() <
           static_cast<size_t>(decimals)) {
        fractionPart.insert(
            fractionPart.begin(),
            '0'
        );
    }

    if (fractionPart.size() > 2)
        fractionPart.resize(2);
    while (fractionPart.size() < 2)
        fractionPart.push_back('0');

    return (negative ? "-" : "") +
           integerPart + "." + fractionPart;
}

cpp_int calcUsdNanos(
    const cpp_int& raw,
    int decimals,
    uint64_t priceNanos
) {
    if (raw <= 0 || priceNanos == 0)
        return 0;

    return (raw * priceNanos) /
           tenPow(decimals);
}

std::string formatUsd(const cpp_int& nanos) {
    const bool negative = nanos < 0;
    cpp_int absoluteValue =
        negative ? -nanos : nanos;

    const cpp_int oneDollar = 1000000000;
    const cpp_int dollarsValue =
        absoluteValue / oneDollar;
    const cpp_int centsValue =
        (absoluteValue % oneDollar) / 10000000;

    std::string cents =
        centsValue.convert_to<std::string>();
    while (cents.size() < 2)
        cents.insert(cents.begin(), '0');

    return std::string(negative ? "-$" : "$") +
           dollarsValue.convert_to<std::string>() +
           "." + cents;
}

cpp_int calcUnitPriceNanos(
    const cpp_int& usdNanos,
    const cpp_int& rawAmount,
    int decimals
) {
    if (usdNanos <= 0 || rawAmount <= 0)
        return 0;

    return (usdNanos * tenPow(decimals)) /
           rawAmount;
}

std::string formatPriceUsd(const cpp_int& nanos) {
    const bool negative = nanos < 0;
    cpp_int absoluteValue =
        negative ? -nanos : nanos;

    const cpp_int oneDollar = 1000000000;
    const cpp_int dollarsValue =
        absoluteValue / oneDollar;
    cpp_int fractionValue =
        absoluteValue % oneDollar;

    std::string fraction =
        fractionValue.convert_to<std::string>();
    while (fraction.size() < 9)
        fraction.insert(fraction.begin(), '0');

    while (fraction.size() > 2 &&
           fraction.back() == '0') {
        fraction.pop_back();
    }

    return std::string(negative ? "-$" : "$") +
           dollarsValue.convert_to<std::string>() +
           "." + fraction;
}

const ChainContext& chainCtx() {
    return g_chain;
}

void setChainContext(const ChainContext& context) {
    g_chain = context;
}

bool isBaseAsset(const std::string& address) {
    const std::string normalized = toLower(address);
    return normalized == g_chain.nativeMarker ||
           g_chain.baseAssets.count(normalized) > 0;
}

bool isStablecoin(const std::string& address) {
    return g_chain.stablecoins.count(
        toLower(address)
    ) > 0;
}

std::string lookupRouterLabel(
    const std::string& address
) {
    const auto found = g_chain.routers.find(
        toLower(address)
    );

    return found == g_chain.routers.end()
        ? std::string()
        : found->second;
}

ChainContext makeBscContext() {
    ChainContext context;
    context.displayName = "BNB Smart Chain";
    context.explorerUrl = "https://bscscan.com";
    context.explorerName = "BscScan";
    context.nativeSymbol = "BNB";
    context.nativeMarker = "native:bnb";
    context.wrappedNative = WBNB_ADDR;

    context.stablecoins = {
        "0x55d398326f99059ff775485246999027b3197955",
        "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d",
        "0xe9e7cea3dedca5984780bafc599bd69add087d56"
    };

    context.baseAssets = context.stablecoins;
    context.baseAssets.insert(context.wrappedNative);
    context.baseAssets.insert(
        "0xc5f0f7b66764f6ec8c8dff7ba683102295e16409"
    );

    context.routers = {
        {"0x10ed43c718714eb63d5aa57b78b54704e256024e", "PancakeSwap V2"},
        {"0x13f4ea83d0bd40e75c8222255bc855a974568dd4", "PancakeSwap V3 Smart Router"},
        {"0x1b81d678ffb9c0263b24a97847620c99d213eb14", "PancakeSwap V3 Swap Router"},
        {"0x1a0a18ac4becddbd6389559687d1a73d8927e416", "PancakeSwap Universal Router"},
        {"0xd9c500dff816a1da21a48a732d3498bf09dc9aeb", "PancakeSwap Universal Router 2"},
        {"0x5dc88340e1c5c6366864ee415d6034cadd1a9897", "Uniswap Universal Router"},
        {"0xec8b0f7ffe3ae75d7ffab09429e3675bb63503e4", "Uniswap Universal Router"},
        {"0x1906c1d672b88cd1b9ac7593301ca990f94eae07", "Uniswap V4 Universal Router"},
        {"0x1111111254eeb25477b68fb85ed929f73a960582", "1inch"},
        {"0x9333c74bdd1e118634fe5664aca7a9710b108bab", "OKX DEX"},
        {"0x6015126d7d23648c2e4466693b8deab005ffaba8", "OKX DEX"},
        {"0x6131b5fae19ea4f9d964eac0408e4408b66337b5", "KyberSwap"},
        {"0xdf1a1b60f2d438842916c0adc43748768353ec25", "KyberSwap"},
        {"0x6352a56caadc4f1e25cd6c75970fa768a3304e64", "OpenOcean"},
        {"0x3a6d8ca21d1cf76f653a67577fa0d27453350dd8", "BiSwap"},
        {"0xcf0febd3f17cef5b47b0cd257acf6025c5bff3b7", "ApeSwap"},
        {"0xcde540d7eafe93ac5fe6233bee57e1270d3e330f", "BakerySwap"},
        {"0x19609b03c976cca288fbdae5c21d4290e9a4add7", "Wombat Exchange"},
        {"0x9f138be5aa5cc442ea7cc7d18cd9e30593ed90b9", "Odos"},
        {"0x8f8dd7db1bda5ed3da8c9daf3bfa471c12d58486", "DODO"},
        {"0x114f84658c99aa6ea62e3160a87a16deaf7efe83", "WOOFi"},
        {"0xcef5be73ae943b77f9bc08859367d923c030a269", "WOOFi"}
    };

    context.permit2Contracts = {
        "0x31c2f6fcff4f8759b3bd5bf0e1084a055615c102"
    };

    context.bridges = {
        "0x47a1437d6714f2544ebc7d4e0e95c8b9248f1e2b",
        "0x5427fefa711eff984124bfbb1ab6fbf5e3da1820"
    };

    return context;
}

ChainContext makeEthereumContext() {
    ChainContext context;
    context.displayName = "Ethereum";
    context.explorerUrl = "https://etherscan.io";
    context.explorerName = "Etherscan";
    context.nativeSymbol = "ETH";
    context.nativeMarker = "native:eth";
    context.wrappedNative =
        "0xc02aaa39b223fe8d0a0e5c4f27ead9083c756cc2";

    context.stablecoins = {
        "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
        "0xdac17f958d2ee523a2206206994597c13d831ec7",
        "0x6b175474e89094c44da98b954eedeac495271d0f"
    };

    context.baseAssets = context.stablecoins;
    context.baseAssets.insert(context.wrappedNative);

    context.routers = {
        {"0x7a250d5630b4cf539739df2c5dacb4c659f2488d", "Uniswap V2"},
        {"0xe592427a0aece92de3edee1f18e0157c05861564", "Uniswap V3"},
        {"0x68b3465833fb72a70ecdf485e0e4c7bd8665fc45", "Uniswap V3 Router 2"},
        {"0xef1c6e67703c7bd7107eed8303fbe6ec2554bf6b", "Uniswap Universal Router"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap Universal Router"},
        {"0x66a9893cc07d91d95644aedd05d03f95e1dba8af", "Uniswap V4 Universal Router"},
        {"0x1111111254eeb25477b68fb85ed929f73a960582", "1inch"}
    };

    context.permit2Contracts = {
        "0x000000000022d473030f116ddee9f6b43ac78ba3"
    };

    return context;
}

ChainContext makeBaseContext() {
    ChainContext context;
    context.displayName = "Base";
    context.explorerUrl = "https://basescan.org";
    context.explorerName = "BaseScan";
    context.nativeSymbol = "ETH";
    context.nativeMarker = "native:eth";
    context.wrappedNative =
        "0x4200000000000000000000000000000000000006";

    context.stablecoins = {
        "0x833589fcd6edb6e08f4c7c32d4f71b54bda02913",
        "0xfde4c96c8593536e31f229ea8f37b2ada2699bb2",
        "0x50c5725949a6f0c72e6c4a641f24049a917db0cb"
    };

    context.baseAssets = context.stablecoins;
    context.baseAssets.insert(context.wrappedNative);

    context.routers = {
        {"0x198ef79f1f515f02dfe9e3115ed9fc07183f02fc", "Uniswap Universal Router"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap Universal Router"},
        {"0x6ff5693b99212da76ad316178a184ab56d299b43", "Uniswap V4 Universal Router"}
    };

    context.permit2Contracts = {
        "0x000000000022d473030f116ddee9f6b43ac78ba3"
    };

    return context;
}

ChainContext makeArbitrumContext() {
    ChainContext context;
    context.displayName = "Arbitrum One";
    context.explorerUrl = "https://arbiscan.io";
    context.explorerName = "Arbiscan";
    context.nativeSymbol = "ETH";
    context.nativeMarker = "native:eth";
    context.wrappedNative =
        "0x82af49447d8a07e3bd95bd0d56f35241523fbab1";

    context.stablecoins = {
        "0xaf88d065e77c8cc2239327c5edb3a432268e5831",
        "0xfd086bc7cd5c481dcc9c85ebe478a1c0b69fcbb9",
        "0xda10009cbd5d07dd0cecc66161fc93d7c9000da1"
    };

    context.baseAssets = context.stablecoins;
    context.baseAssets.insert(context.wrappedNative);

    context.routers = {
        {"0x4c60051384bd2d3c01bfc845cf5f4b44bcbe9de5", "Uniswap Universal Router"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap Universal Router"},
        {"0xe592427a0aece92de3edee1f18e0157c05861564", "Uniswap V3"},
        {"0x68b3465833fb72a70ecdf485e0e4c7bd8665fc45", "Uniswap V3 Router 2"},
        {"0x1111111254eeb25477b68fb85ed929f73a960582", "1inch"}
    };

    context.permit2Contracts = {
        "0x000000000022d473030f116ddee9f6b43ac78ba3"
    };

    return context;
}

TxResult analyzeTx(
    const json& tx,
    const json& receipt,
    const std::string& walletAddress
) {
    TxResult result;

    if (!receipt.is_object() ||
        !receipt.contains("logs") ||
        !receipt["logs"].is_array()) {
        return result;
    }

    const std::string wallet =
        toLower(walletAddress);

    if (wallet.empty())
        return result;

    const std::string transactionFrom =
        toLower(txString(tx, "from"));
    const std::string transactionTarget =
        toLower(txString(tx, "to"));
    const std::string input =
        txString(tx, "input");
    const std::string selector =
        selectorOf(input);

    const bool walletIsSender =
        transactionFrom == wallet;

    cpp_int nativeOut = 0;
    if (walletIsSender) {
        nativeOut = hexToCppInt(
            txString(tx, "value")
        );
    }

    ReceiptState state;
    indexReceipt(state, receipt, wallet);

    const UniversalRouterCommands universal =
        decodeUniversalRouter(input);

    const bool knownRouter =
        !lookupRouterLabel(transactionTarget).empty();

    const bool selectorSwap =
        isSwapSelector(selector);

    const bool multicall =
        isGenericMulticallSelectorValue(selector);

    const bool permit2Target =
        g_chain.permit2Contracts.count(
            transactionTarget
        ) > 0;

    const bool dexSignal =
        knownRouter ||
        selectorSwap ||
        universal.hasSwap ||
        state.hasSwapEvent ||
        (multicall && !state.swapPools.empty());

    result.hasSwapEvent = state.hasSwapEvent;
    result.isUniversalRouter = universal.present;
    result.isGenericMulticall = multicall;
    result.hasPermit2Signal =
        universal.hasPermit2 ||
        permit2Target;
    result.dexActivityDetected =
        dexSignal ||
        universal.present ||
        multicall;

    result.lpMintOrBurnSeen = false;
    for (const auto& token : state.tokenOrder) {
        const TokenFlow& flow = state.flows.at(token);
        if (flow.minted || flow.burned) {
            result.lpMintOrBurnSeen = true;
            break;
        }
    }

    result.lpV3EventSeen =
        state.v3Increase ||
        state.v3Decrease ||
        state.v3Collect;

    result.lpPoolIdentitySeen = false;
    for (const auto& token : state.tokenOrder) {
        if (sourceOrDestinationIsPool(
                state.flows.at(token),
                state.swapPools)) {
            result.lpPoolIdentitySeen = true;
            break;
        }
    }

    // Failed transactions are valid observations but must not alert.
    if (receipt.contains("status") &&
        receipt["status"].is_string() &&
        toLower(receipt["status"].get<std::string>()) ==
            "0x0") {
        result.valid = false;
        return result;
    }

    // Exact direct wrap/unwrap. Router-internal wrapped-native events are
    // deliberately not classified as standalone wrap operations.
    if (walletIsSender &&
        transactionTarget == g_chain.wrappedNative &&
        !state.hasWalletTransfer) {
        if (isWrapSelector(selector) &&
            nativeOut > 0) {
            result.valid = true;
            result.venue = "Wrap";
            result.tokenAddr = g_chain.wrappedNative;
            result.rawAmount =
                state.walletWrapped > 0
                ? state.walletWrapped
                : nativeOut;
            result.isBuy = true;
            calculateUsd(result);
            return result;
        }

        if (isUnwrapSelector(selector) &&
            state.walletUnwrapped > 0) {
            result.valid = true;
            result.venue = "Unwrap";
            result.tokenAddr = g_chain.wrappedNative;
            result.rawAmount =
                state.walletUnwrapped;
            result.isBuy = false;
            calculateUsd(result);
            return result;
        }
    }

    if (!state.hasWalletTransfer) {
        // Approval calls are intentionally ignored by the alert pipeline.
        if (state.hasApproval ||
            isApproveSelector(selector)) {
            return result;
        }

        return result;
    }

    result.valid = true;
    result.venue = detectVenue(
        transactionTarget,
        selector,
        universal,
        state
    );

    classifyLiquidity(
        result,
        state,
        isAddLiquiditySelector(selector),
        isRemoveLiquiditySelector(selector)
    );

    if (result.venue == "Add Liquidity" ||
        result.venue == "Remove Liquidity") {
        calculateUsd(result);
        return result;
    }

    bool baseIncoming = false;
    bool baseOutgoing = false;

    for (const auto& token : state.tokenOrder) {
        if (!isBaseAsset(token))
            continue;

        const cpp_int net =
            state.flows.at(token).net;

        baseIncoming = baseIncoming || net > 0;
        baseOutgoing = baseOutgoing || net < 0;
    }

    const bool nativeOutgoing =
        walletIsSender &&
        nativeOut > 0 &&
        (dexSignal ||
         universal.hasWrap);

    // Without callTracer, a native payout is not directly present in the
    // receipt. A wrapped-native Withdrawal can safely recover the SELL side
    // only when the tracked wallet actually sent a non-base token and the
    // transaction has a strong DEX signal.
    bool hasOutgoingNonBase = false;
    for (const auto& token : state.tokenOrder) {
        if (!isBaseAsset(token) &&
            state.flows.at(token).net < 0) {
            hasOutgoingNonBase = true;
            break;
        }
    }

    cpp_int nativeIn = 0;
    if (state.walletUnwrapped > 0) {
        nativeIn = state.walletUnwrapped;
    } else if (walletIsSender &&
               hasOutgoingNonBase &&
               dexSignal &&
               state.transactionUnwrapped > 0) {
        nativeIn = state.transactionUnwrapped;
    }

    const bool nativeIncoming =
        nativeIn > 0;

    const std::optional<Candidate> primary =
        choosePrimaryNonBase(
            state,
            baseIncoming,
            baseOutgoing,
            nativeIncoming,
            nativeOutgoing
        );

    if (primary) {
        result.tokenAddr = primary->token;
        result.rawAmount = primary->magnitude;
        result.isBuy = primary->net > 0;

        const bool twoSided =
            (result.isBuy &&
             (baseOutgoing || nativeOutgoing)) ||
            (!result.isBuy &&
             (baseIncoming || nativeIncoming));

        const bool oneSidedBuyRecovery =
            result.isBuy &&
            dexSignal &&
            (nativeOutgoing ||
             universal.hasSwap ||
             state.hasSwapEvent);

        const bool oneSidedSellRecovery =
            !result.isBuy &&
            dexSignal &&
            (nativeIncoming ||
             universal.hasSwap ||
             state.hasSwapEvent);

        result.isSwap =
            twoSided ||
            oneSidedBuyRecovery ||
            oneSidedSellRecovery;

        if (result.isSwap) {
            setCounterFromWalletFlows(
                result,
                state,
                nativeOutgoing,
                nativeOut,
                nativeIncoming,
                nativeIn
            );
        }
    } else {
        const std::optional<Candidate> largest =
            chooseLargestFlow(state);

        if (largest) {
            result.tokenAddr = largest->token;
            result.rawAmount = largest->magnitude;
            result.isBuy = largest->net > 0;
        }

        // A transaction involving only quote assets is normally a transfer,
        // not a trading alert.
        result.isSwap = false;
    }

    if (result.isSwap) {
        if (result.venue.empty())
            result.venue = "DEX";

        if (result.counterAddr.empty() ||
            result.counterAmount <= 0) {
            result.unknownReason =
                "NO_COUNTER_FLOW";
        }
    } else if (result.dexActivityDetected &&
               result.venue.empty()) {
        result.unknownReason =
            state.hasSwapEvent
            ? "UNKNOWN_ROUTER"
            : "OTHER";
    }

    calculateUsd(result);
    return result;
}
