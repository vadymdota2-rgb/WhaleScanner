#include "tx_analyzer.h"

#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <limits>
#include "utils.h"

using json = nlohmann::json;

uint64_t getPriceNanos(const std::string& token);
int getDecimals(const std::string& addr);
std::string getSymbol(const std::string& addr);

cpp_int hexNibbles(const std::string& h, size_t from, size_t count) {
    cpp_int r = 0;
    const size_t end = std::min(h.size(), from + count);
    for (size_t i = from; i < end; i++) {
        const char c = h[i];
        r <<= 4;
        if      (c >= '0' && c <= '9') r |= (c - '0');
        else if (c >= 'a' && c <= 'f') r |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') r |= (c - 'A' + 10);
    }
    return r;
}

cpp_int parseUint256(const std::string& h) {
    if (h.length() < 66) return 0;
    return hexNibbles(h, 2, 64);
}
cpp_int hexToCppInt(const std::string& h) {
    if (h.size() < 2 || h[0] != '0' || h[1] != 'x') return 0;
    return hexNibbles(h, 2, h.size() - 2);
}
std::string formatAmount(const cpp_int& raw, int dec) {
    if (dec < 0 || dec > 36) dec = 18;
    if (raw == 0) return "0.00";
    cpp_int d = 1;
    for (int i = 0; i < dec; i++) d *= 10;
    std::string ip = (raw / d).convert_to<std::string>();
    std::string fp = (raw % d).convert_to<std::string>();
    while (static_cast<int>(fp.length()) < dec) fp = "0" + fp;
    if (fp.length() > 2) fp = fp.substr(0, 2);
    while (fp.length() < 2) fp += "0";
    std::string grouped; int cnt=0;
    for (auto it=ip.rbegin(); it!=ip.rend(); ++it) {
        if (cnt && cnt%3==0 && *it>='0' && *it<='9') grouped.push_back(',');
        grouped.push_back(*it); cnt++;
    }
    std::reverse(grouped.begin(), grouped.end());
    return grouped+"."+fp;
}
cpp_int calcUsdNanos(const cpp_int& raw, int dec, uint64_t pn) { if (!pn) return 0; cpp_int d=1; for (int i=0;i<dec;i++) d*=10; return (raw*pn)/d; }
std::string formatUsd(const cpp_int& n) { std::string s=n.convert_to<std::string>(); while (s.length()<10) s="0"+s;
    std::string dl=s.substr(0,s.length()-9), ct=s.substr(s.length()-9,2); if (dl.empty()) dl="0"; return "$"+dl+"."+ct; }

cpp_int calcUnitPriceNanos(const cpp_int& usdNanos, const cpp_int& rawAmount, int dec) {
    if (rawAmount <= 0) return 0;
    cpp_int d = 1; for (int i = 0; i < dec; i++) d *= 10;
    return (usdNanos * d) / rawAmount;
}

std::string formatPriceUsd(const cpp_int& n) {
    bool neg = n < 0;
    cpp_int a = neg ? -n : n;
    std::string s = a.convert_to<std::string>();
    while (s.length() < 10) s = "0" + s;
    std::string dollarPart = s.substr(0, s.length() - 9);
    std::string fracPart = s.substr(s.length() - 9);
    if (dollarPart.empty()) dollarPart = "0";
    size_t lastNonZero = fracPart.find_last_not_of('0');
    size_t keep = (lastNonZero == std::string::npos) ? 0 : (lastNonZero + 1);
    if (keep < 2) keep = 2;
    fracPart = fracPart.substr(0, keep);
    return std::string(neg ? "-$" : "$") + dollarPart + "." + fracPart;
}

namespace {

struct UniversalRouterCommands {
    bool present = false;
    bool hasWrap = false, hasUnwrap = false;
    bool hasV2Swap = false, hasV3Swap = false;
    bool hasSweep = false, hasTransfer = false;
    bool hasPermit2 = false;
};

bool isGenericMulticallSelector(const std::string& input) {
    if (input.size() < 10 || input[0] != '0' || input[1] != 'x') return false;
    return input.substr(2, 8) == "ac9650d8";
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

uint64_t hexWordToU64(const std::string& hexNo0x, size_t hexPos) {
    if (hexNo0x.size() < hexPos + 64) return 0;
    for (size_t i = hexPos; i < hexPos + 48; i++)
        if (hexNo0x[i] != '0') return std::numeric_limits<uint64_t>::max();
    uint64_t v = 0;
    for (size_t i = hexPos + 48; i < hexPos + 64; i++) {
        int n = hexNibble(hexNo0x[i]);
        if (n < 0) return 0;
        v = (v << 4) | (uint64_t)n;
    }
    return v;
}

UniversalRouterCommands parseExecuteCommands(const std::string& input) {
    UniversalRouterCommands out;
    if (input.size() < 10 || input[0] != '0' || input[1] != 'x') return out;
    std::string selector = input.substr(2, 8);
    if (selector != "3593564c" && selector != "24856bc3") return out;

    std::string hexNo0x = input.substr(2);
    if (hexNo0x.size() < 8 + 64) return out;

    uint64_t offsetCommands = hexWordToU64(hexNo0x, 8);
    if (offsetCommands > hexNo0x.size()) return out;
    size_t lenHexPos = 8 + offsetCommands * 2;
    if (hexNo0x.size() < lenHexPos + 64) return out;

    uint64_t length = hexWordToU64(hexNo0x, lenHexPos);
    if (length == 0 || length > 64) return out;

    size_t dataHexPos = lenHexPos + 64;
    if (hexNo0x.size() < dataHexPos + length * 2) return out;

    out.present = true;
    for (uint64_t i = 0; i < length; i++) {
        int b = (hexNibble(hexNo0x[dataHexPos + i * 2]) << 4) | hexNibble(hexNo0x[dataHexPos + i * 2 + 1]);
        int cmd = b & 0x3f;
        if (cmd == 0x0b) out.hasWrap = true;
        else if (cmd == 0x0c) out.hasUnwrap = true;
        else if (cmd == 0x08 || cmd == 0x09) out.hasV2Swap = true;
        else if (cmd == 0x00 || cmd == 0x01) out.hasV3Swap = true;
        else if (cmd == 0x04) out.hasSweep = true;
        else if (cmd == 0x05) out.hasTransfer = true;
        else if (cmd == 0x02 || cmd == 0x03 || cmd == 0x0a || cmd == 0x0d) out.hasPermit2 = true;
    }
    return out;
}

}

struct FlowEdge {
    std::string from;
    std::string to;
    cpp_int amount;
    uint64_t logIndex = 0;
};

enum class V3LiquidityKind {
    Increase,
    Decrease,
    Collect
};

struct V3LiquidityEvent {
    std::string manager;
    std::string tokenId;
    V3LiquidityKind kind;
};

struct NftTransferEvidence {
    std::string manager;
    std::string tokenId;
    std::string from;
    std::string to;
};

cpp_int absInt(const cpp_int& v) {
    return v < 0 ? -v : v;
}

cpp_int pow10Int(int n) {
    cpp_int r = 1;
    for (int i = 0; i < n; ++i) r *= 10;
    return r;
}

struct FlowRank {
    cpp_int usdNanos = 0;
    cpp_int normalized18 = 0;
    cpp_int raw = 0;
    bool hasPrice = false;
};

FlowRank rankFlow(const std::string& token, const cpp_int& rawAmount) {
    FlowRank rank;
    rank.raw = absInt(rawAmount);

    int decimals = getDecimals(token);
    if (decimals < 0 || decimals > 36) decimals = 18;
    if (decimals <= 18) rank.normalized18 = rank.raw * pow10Int(18 - decimals);
    else rank.normalized18 = rank.raw / pow10Int(decimals - 18);

    const uint64_t priceNanos = getPriceNanos(token);
    if (priceNanos > 0) {
        rank.hasPrice = true;
        rank.usdNanos = calcUsdNanos(rank.raw, decimals, priceNanos);
    }
    return rank;
}

bool betterRank(const FlowRank& candidate, const FlowRank& current) {
    if (candidate.hasPrice != current.hasPrice) return candidate.hasPrice;
    if (candidate.hasPrice && candidate.usdNanos != current.usdNanos)
        return candidate.usdNanos > current.usdNanos;
    if (candidate.normalized18 != current.normalized18)
        return candidate.normalized18 > current.normalized18;
    return candidate.raw > current.raw;
}

bool receiptSucceeded(const json& receipt) {
    if (!receipt.contains("status")) return true;
    const auto& status = receipt["status"];
    if (status.is_string()) return hexToCppInt(status.get<std::string>()) != 0;
    if (status.is_number_integer()) return status.get<long long>() != 0;
    if (status.is_number_unsigned()) return status.get<unsigned long long>() != 0;
    return true;
}

std::string topicAddress(const std::string& topic) {
    if (topic.size() < 66) return "";
    return "0x" + toLower(topic.substr(topic.size() - 40));
}

std::string topicUint256Key(const std::string& topic) {
    if (topic.size() < 66 || topic[0] != '0' || topic[1] != 'x') return "";
    return "0x" + toLower(topic.substr(topic.size() - 64));
}

bool nftEvidenceMatches(
    const std::vector<NftTransferEvidence>& transfers,
    const V3LiquidityEvent& event,
    const std::string& wallet,
    bool incomingToWallet
) {
    for (const auto& nft : transfers) {
        if (nft.manager != event.manager || nft.tokenId != event.tokenId)
            continue;
        if (incomingToWallet && nft.to == wallet)
            return true;
        if (!incomingToWallet && nft.from == wallet)
            return true;
    }
    return false;
}

int walletFlowsWithCounterparty(
    const std::map<std::string, std::vector<FlowEdge>>& graph,
    const std::vector<std::string>& tokenOrder,
    const std::string& wallet,
    const std::string& counterparty,
    bool incoming
) {
    int count = 0;
    for (const auto& token : tokenOrder) {
        auto it = graph.find(token);
        if (it == graph.end()) continue;
        for (const auto& e : it->second) {
            if (incoming && e.to == wallet && e.from == counterparty) { ++count; break; }
            if (!incoming && e.from == wallet && e.to == counterparty) { ++count; break; }
        }
    }
    return count;
}

bool reachesAny(
    const std::map<std::string, std::vector<FlowEdge>>& graph,
    const std::string& token,
    const std::string& start,
    const std::set<std::string>& targets,
    int maxDepth = 8
) {
    auto it = graph.find(token);
    if (it == graph.end() || start.empty() || targets.empty()) return false;

    std::map<std::string, uint64_t> bestIdx;
    std::vector<std::tuple<std::string, int, uint64_t>> queue;
    queue.push_back({start, 0, 0});
    bestIdx[start] = 0;

    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const std::string node = std::get<0>(queue[qi]);
        const int depth = std::get<1>(queue[qi]);
        const uint64_t lastIdx = std::get<2>(queue[qi]);
        if (targets.count(node)) return true;
        if (depth >= maxDepth) continue;

        for (const auto& e : it->second) {
            if (e.from != node) continue;
            if (lastIdx > 0 && e.logIndex <= lastIdx) continue;
            auto b = bestIdx.find(e.to);
            if (b != bestIdx.end() && b->second <= e.logIndex) continue;
            bestIdx[e.to] = e.logIndex;
            queue.push_back({e.to, depth + 1, e.logIndex});
        }
    }
    return false;
}

bool flowLinkedToPool(
    const std::map<std::string, std::vector<FlowEdge>>& graph,
    const std::string& token,
    const std::string& wallet,
    const std::set<std::string>& pools,
    bool incoming
) {
    if (incoming) {
        for (const auto& pool : pools) {
            if (reachesAny(graph, token, pool, {wallet})) return true;
        }
        return false;
    }
    return reachesAny(graph, token, wallet, pools);
}

ChainContext g_chain;
const ChainContext& chainCtx() { return g_chain; }
void setChainContext(const ChainContext& ctx) { g_chain = ctx; }

cpp_int usdFromCounterAsset(const std::string& counterAddr, const cpp_int& counterAmount) {
    if (counterAddr.empty() || counterAmount <= 0) return 0;

    const bool isNative = (counterAddr == g_chain.nativeMarker) ||
                          (!g_chain.wrappedNative.empty() && counterAddr == g_chain.wrappedNative);
    if (isNative) {
        uint64_t np = getPriceNanos(g_chain.wrappedNative);
        if (!np) return 0;
        return calcUsdNanos(counterAmount, 18, np);
    }

    if (g_chain.stablecoins.count(counterAddr)) {
        const int dec = getDecimals(counterAddr);
        cpp_int d = 1; for (int k = 0; k < dec; ++k) d *= 10;
        return (counterAmount * cpp_int(1000000000)) / d;
    }
    return 0;
}

cpp_int gasCostUsdNanos(const json& receipt) {
    if (!receipt.is_object()) return 0;
    auto hexField = [&](const char* name) -> cpp_int {
        if (!receipt.contains(name) || !receipt[name].is_string()) return 0;
        return hexToCppInt(receipt[name].get<std::string>());
    };
    cpp_int used = hexField("gasUsed");
    cpp_int price = hexField("effectiveGasPrice");
    if (price <= 0) price = hexField("gasPrice");
    if (used <= 0 || price <= 0) return 0;

    uint64_t nativePrice = getPriceNanos(g_chain.wrappedNative);
    if (!nativePrice) return 0;
    return calcUsdNanos(used * price, 18, nativePrice);
}

bool isBaseAsset(const std::string& a) { return g_chain.baseAssets.count(toLower(a)) > 0; }
std::string lookupRouterLabel(const std::string& addr) {
    auto it = g_chain.routers.find(toLower(addr));
    return it != g_chain.routers.end() ? it->second : std::string();
}

namespace {

const std::set<std::string> SWAP_EVENT_TOPICS = {
    "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822",
    "0xc42079f94a6350d7e6235f29174924f928cc2ac818eb64fed8004e115fbcca67",
    "0x19b47279256b2a23a1665c810c8d55a1758940ee09377d4f8d26497a3577dc83",
    "0x04206ad2b7c0f463bff3dd4f33c5735b0f2957a351e4f79763a4fa9e775dd237",
};
const std::string ERC20_TRANSFER_TOPIC =
    "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
const std::string WBNB_DEPOSIT_TOPIC =
    "0xe1fffcc4923d04b559f4d29a8bfc6cda04eb5b0d3c460751c2402c5c5cc9109c";
const std::string WBNB_WITHDRAWAL_TOPIC =
    "0x7fcf532c15f0a6db0bd6d0e038bea71d30d808c7d98cb3bf7268a95bf5081b65";
const std::string V2_MINT_TOPIC =
    "0x4c209b5fc8ad50758f13e2e1088ba56a560dff690a1c6fef26394f4c03821c4f";
const std::string V2_BURN_TOPIC =
    "0xdccd412f0b1252819cb1fd330b93224ca42612892bb3f4f789976e6d81936496";
const std::string V3_INCREASE_LIQUIDITY_TOPIC =
    "0x3067048beee31b25b2f1681f88dac838c8bba36af25bfb2b7cf7473a5847e35f";
const std::string V3_DECREASE_LIQUIDITY_TOPIC =
    "0x26f6a048ee9138f2c0ce266f322cb99228e8d619ae2bff30c67f8dcf9d2377b4";
const std::string V3_COLLECT_TOPIC =
    "0x40d0efd1a53d60ecbf40971b9daf7dc90178c3aadc7aab1765632738fa8b8f01";

}

TxResult analyzeTx(const json& tx, const json& receipt, const std::string& walletArg) {
    TxResult r{};
    if (!receipt.is_object() || !receipt.contains("logs") || !receipt["logs"].is_array()) {
        r.unknownReason = "INVALID_RECEIPT";
        return r;
    }
    if (!receiptSucceeded(receipt)) {
        r.unknownReason = "TX_REVERTED";
        return r;
    }

    const std::string wallet = toLower(walletArg);
    const std::string zero = "0x0000000000000000000000000000000000000000";
    const std::string dead = "0x000000000000000000000000000000000000dead";

    std::map<std::string, cpp_int> netFlow;
    std::map<std::string, cpp_int> grossIn;
    std::map<std::string, cpp_int> grossOut;
    std::map<std::string, std::vector<FlowEdge>> graph;
    std::vector<std::string> tokenOrder;
    std::set<std::string> swapPools;
    std::set<std::string> mintedToWallet;
    std::set<std::string> burnedFromWallet;
    std::set<std::string> v2MintPools;
    std::set<std::string> v2BurnPools;
    std::vector<V3LiquidityEvent> v3LiquidityEvents;
    std::vector<NftTransferEvidence> nftTransfers;

    auto touch = [&](const std::string& token) {
        if (!netFlow.count(token)) {
            netFlow[token] = 0;
            grossIn[token] = 0;
            grossOut[token] = 0;
            tokenOrder.push_back(token);
        }
    };

    bool hasSwap = false;
    bool v3Increase = false;
    bool v3Decrease = false;
    bool v3Collect = false;
    cpp_int wrappedForWallet = 0;
    cpp_int unwrappedForWallet = 0;
    std::map<std::string, cpp_int> wrappedByDst;
    std::map<std::string, cpp_int> unwrappedByDst;

    uint64_t logSeq = 0;
    for (const auto& log : receipt["logs"]) {
        ++logSeq;
        if (!log.is_object() || !log.contains("topics") ||
            !log["topics"].is_array() || log["topics"].empty() ||
            !log["topics"][0].is_string())
            continue;

        const std::string topic0 = toLower(log["topics"][0].get<std::string>());
        const std::string logAddr =
            (log.contains("address") && log["address"].is_string())
                ? toLower(log["address"].get<std::string>())
                : "";

        if (SWAP_EVENT_TOPICS.count(topic0)) {
            hasSwap = true;
            if (!logAddr.empty()) swapPools.insert(logAddr);
            continue;
        }

        if (topic0 == V2_MINT_TOPIC) {
            if (!logAddr.empty()) v2MintPools.insert(logAddr);
            continue;
        }
        if (topic0 == V2_BURN_TOPIC) {
            if (!logAddr.empty()) v2BurnPools.insert(logAddr);
            continue;
        }

        if (topic0 == V3_INCREASE_LIQUIDITY_TOPIC ||
            topic0 == V3_DECREASE_LIQUIDITY_TOPIC ||
            topic0 == V3_COLLECT_TOPIC) {
            if (topic0 == V3_INCREASE_LIQUIDITY_TOPIC) v3Increase = true;
            if (topic0 == V3_DECREASE_LIQUIDITY_TOPIC) v3Decrease = true;
            if (topic0 == V3_COLLECT_TOPIC) v3Collect = true;

            if (!logAddr.empty() &&
                log["topics"].size() >= 2 &&
                log["topics"][1].is_string()) {
                const std::string tokenId =
                    topicUint256Key(log["topics"][1].get<std::string>());
                if (!tokenId.empty()) {
                    V3LiquidityKind kind = V3LiquidityKind::Collect;
                    if (topic0 == V3_INCREASE_LIQUIDITY_TOPIC)
                        kind = V3LiquidityKind::Increase;
                    else if (topic0 == V3_DECREASE_LIQUIDITY_TOPIC)
                        kind = V3LiquidityKind::Decrease;
                    v3LiquidityEvents.push_back({logAddr, tokenId, kind});
                }
            }
            continue;
        }

        if (logAddr == g_chain.wrappedNative &&
            (topic0 == WBNB_DEPOSIT_TOPIC || topic0 == WBNB_WITHDRAWAL_TOPIC)) {
            if (log["topics"].size() >= 2 && log["topics"][1].is_string() &&
                log.contains("data") && log["data"].is_string()) {
                const std::string dst = topicAddress(log["topics"][1].get<std::string>());
                cpp_int amount = parseUint256(log["data"].get<std::string>());
                if (topic0 == WBNB_DEPOSIT_TOPIC) wrappedByDst[dst] += amount;
                else unwrappedByDst[dst] += amount;
                if (dst == wallet) {
                    if (topic0 == WBNB_DEPOSIT_TOPIC) wrappedForWallet += amount;
                    else unwrappedForWallet += amount;
                }
            }
            continue;
        }

        if (topic0 == ERC20_TRANSFER_TOPIC && !logAddr.empty() &&
            log["topics"].size() == 4 &&
            log["topics"][1].is_string() &&
            log["topics"][2].is_string() &&
            log["topics"][3].is_string()) {
            const std::string nftFrom = topicAddress(log["topics"][1].get<std::string>());
            const std::string nftTo = topicAddress(log["topics"][2].get<std::string>());
            const std::string tokenId =
                topicUint256Key(log["topics"][3].get<std::string>());
            if (!nftFrom.empty() && !nftTo.empty() && !tokenId.empty())
                nftTransfers.push_back({logAddr, tokenId, nftFrom, nftTo});
            continue;
        }

        if (topic0 != ERC20_TRANSFER_TOPIC || logAddr.empty() ||
            log["topics"].size() != 3 ||
            !log["topics"][1].is_string() || !log["topics"][2].is_string() ||
            !log.contains("data") || !log["data"].is_string())
            continue;

        const std::string from = topicAddress(log["topics"][1].get<std::string>());
        const std::string to = topicAddress(log["topics"][2].get<std::string>());
        const cpp_int amount = parseUint256(log["data"].get<std::string>());
        if (from.empty() || to.empty() || amount <= 0) continue;

        graph[logAddr].push_back({from, to, amount, logSeq});

        if (from != wallet && to != wallet) continue;

        touch(logAddr);
        if (to == wallet) {
            netFlow[logAddr] += amount;
            grossIn[logAddr] += amount;
            if (from == zero) mintedToWallet.insert(logAddr);
        }
        if (from == wallet) {
            netFlow[logAddr] -= amount;
            grossOut[logAddr] += amount;
            if (to == zero || to == dead) burnedFromWallet.insert(logAddr);
        }
    }

    std::string txTo;
    bool walletIsSender = false;
    cpp_int nativeOut = 0;
    UniversalRouterCommands urCmds;
    bool genericMulticall = false;

    if (tx.is_object()) {
        if (tx.contains("to") && tx["to"].is_string())
            txTo = toLower(tx["to"].get<std::string>());

        if (tx.contains("from") && tx["from"].is_string() &&
            toLower(tx["from"].get<std::string>()) == wallet) {
            walletIsSender = true;
            if (tx.contains("value") && tx["value"].is_string())
                nativeOut = hexToCppInt(tx["value"].get<std::string>());
        }

        if (walletIsSender && tx.contains("input") && tx["input"].is_string()) {
            const std::string input = tx["input"].get<std::string>();
            urCmds = parseExecuteCommands(input);
            genericMulticall = isGenericMulticallSelector(input);
        }
    }

    const bool knownRouter =
        (!txTo.empty() && !lookupRouterLabel(txTo).empty()) ||
        (urCmds.present && (urCmds.hasV2Swap || urCmds.hasV3Swap));
    const bool universalRouterSwap = urCmds.present && (urCmds.hasV2Swap || urCmds.hasV3Swap);
    const bool dexSignal = hasSwap || knownRouter || universalRouterSwap;

    r.hasSwapEvent = hasSwap;
    r.isUniversalRouter = urCmds.present;
    r.isGenericMulticall = genericMulticall;
    r.hasPermit2Signal = urCmds.hasPermit2;

    r.dexActivityDetected = hasSwap;
    r.erc20MintOrBurnSeen = !mintedToWallet.empty() || !burnedFromWallet.empty();
    r.lpV3EventSeen = v3Increase || v3Decrease || v3Collect;
    r.lpPoolIdentitySeen = !v2MintPools.empty() || !v2BurnPools.empty();

    bool hasNonWrappedFlow = false;
    for (const auto& token : tokenOrder) {
        if (token != g_chain.wrappedNative && netFlow[token] != 0) {
            hasNonWrappedFlow = true;
            break;
        }
    }

    if (!hasSwap && !hasNonWrappedFlow &&
        (wrappedForWallet > 0 || unwrappedForWallet > 0)) {
        r.valid = true;
        r.isSwap = false;
        r.tokenAddr = g_chain.wrappedNative;

        if (wrappedForWallet >= unwrappedForWallet) {
            r.venue = "Wrap";
            r.rawAmount = wrappedForWallet;
            r.isBuy = true;
        } else {
            r.venue = "Unwrap";
            r.rawAmount = unwrappedForWallet;
            r.isBuy = false;
        }

        const int dec = getDecimals(r.tokenAddr);
        r.usdNanos = calcUsdNanos(r.rawAmount, dec, getPriceNanos(r.tokenAddr));
        return r;
    }

    bool hasWalletFlow = false;
    for (const auto& token : tokenOrder) {
        if (netFlow[token] != 0) {
            hasWalletFlow = true;
            break;
        }
    }

    if (!hasWalletFlow) {
        if (dexSignal) {
            r.valid = true;
            r.diagnosticReason = hasSwap
                ? "SWAP_EVENT_WITHOUT_WALLET_FLOW"
                : "DEX_SIGNAL_WITHOUT_WALLET_FLOW";
            r.venue = "DEX interaction";
            std::map<std::string, std::map<std::string, cpp_int>> perAddr;
            for (const auto& [token, edges] : graph)
                for (const auto& e : edges) {
                    if (!e.to.empty()) perAddr[e.to][token] += e.amount;
                    if (!e.from.empty()) perAddr[e.from][token] -= e.amount;
                }

            std::string vaultAddr;
            int vaultCandidates = 0;
            for (const auto& [addr, flows] : perAddr) {
                if (addr == wallet || addr == "0x0000000000000000000000000000000000000000" ||
                    swapPools.count(addr) || g_chain.knownPoolInfra.count(addr)) continue;
                bool hasPos = false, hasNeg = false;
                for (const auto& [token, amt] : flows) {
                    if (amt > 0) hasPos = true;
                    if (amt < 0) hasNeg = true;
                }
                if (hasPos && hasNeg) { vaultAddr = addr; ++vaultCandidates; }
            }

            bool vaultAttributed = false;
            if (vaultCandidates == 1 && !vaultAddr.empty()) {
                const auto& vFlow = perAddr.at(vaultAddr);
                std::string vMainToken; cpp_int vMainNet = 0; cpp_int vMainAbs = 0;
                FlowRank vMainRank; bool vMainSet = false;
                for (const auto& [token, amt] : vFlow) {
                    if (isBaseAsset(token) || amt == 0) continue;
                    const FlowRank candidateRank = rankFlow(token, amt);
                    if (!vMainSet || betterRank(candidateRank, vMainRank)) {
                        vMainSet = true; vMainToken = token; vMainNet = amt;
                        vMainAbs = absInt(amt); vMainRank = candidateRank;
                    }
                }
                if (vMainSet) {
                    std::string vCounter; cpp_int vCounterAbs = 0; FlowRank vCounterRank; bool vCounterSet = false;
                    for (const auto& [token, amt] : vFlow) {
                        if (token == vMainToken || amt == 0) continue;
                        const bool opposite = (vMainNet > 0 && amt < 0) || (vMainNet < 0 && amt > 0);
                        if (!opposite) continue;
                        const FlowRank candidateRank = rankFlow(token, amt);
                        if (!vCounterSet || betterRank(candidateRank, vCounterRank)) {
                            vCounterSet = true; vCounter = token; vCounterAbs = absInt(amt); vCounterRank = candidateRank;
                        }
                    }
                    std::set<std::string> outCounterparties, inCounterparties;
                    if (vCounterSet) {
                        const std::string& outToken = (vMainNet > 0) ? vCounter : vMainToken;
                        const std::string& inToken  = (vMainNet > 0) ? vMainToken : vCounter;
                        auto outIt = graph.find(outToken);
                        if (outIt != graph.end())
                            for (const auto& e : outIt->second)
                                if (e.from == vaultAddr) outCounterparties.insert(e.to);
                        auto inIt = graph.find(inToken);
                        if (inIt != graph.end())
                            for (const auto& e : inIt->second)
                                if (e.to == vaultAddr) inCounterparties.insert(e.from);
                    }
                    bool sameCounterparty = false;
                    for (const auto& c : outCounterparties)
                        if (inCounterparties.count(c)) { sameCounterparty = true; break; }
                    if (vCounterSet && sameCounterparty) {
                        r.isSwap = true;
                        r.dexActivityDetected = true;
                        r.tokenAddr = vMainToken;
                        r.rawAmount = vMainAbs;
                        r.isBuy = vMainNet > 0;
                        r.counterAddr = vCounter;
                        r.counterAmount = vCounterAbs;
                        r.venue = "Bot Trade";
                        r.diagnosticReason = "VAULT_FLOW_ATTRIBUTED";
                        r.usdNanos = usdFromCounterAsset(r.counterAddr, r.counterAmount);
                        if (r.usdNanos <= 0) {
                            const int dec = getDecimals(r.tokenAddr);
                            r.usdNanos = calcUsdNanos(r.rawAmount, dec, getPriceNanos(r.tokenAddr));
                        }
                        r.gasUsdNanos = gasCostUsdNanos(receipt);
                        vaultAttributed = true;
                    }
                }
            }

            if (!vaultAttributed) {
                std::string out;
                int listed = 0;
                for (const auto& [token, edges] : graph) {
                    std::string bestAddr; cpp_int bestNet = 0;
                    for (const auto& [addr, flows] : perAddr) {
                        if (addr == wallet || addr == "0x0000000000000000000000000000000000000000" ||
                            swapPools.count(addr) || addr == txTo || g_chain.knownPoolInfra.count(addr)) continue;
                        auto it = flows.find(token);
                        if (it != flows.end() && it->second > bestNet) { bestNet = it->second; bestAddr = addr; }
                    }
                    if (!bestAddr.empty() && listed < 5) {
                        if (!out.empty()) out += ";";
                        out += token + ":" + bestAddr + ":" + bestNet.convert_to<std::string>();
                        ++listed;
                    }
                }
                r.flowBeneficiaries = out;
            }
        } else {
            r.unknownReason = "NO_WALLET_FLOW";
        }
        return r;
    }
    r.valid = true;

    int outgoingAssets = 0;
    int incomingAssets = 0;
    for (const auto& token : tokenOrder) {
        if (netFlow[token] < 0) ++outgoingAssets;
        if (netFlow[token] > 0) ++incomingAssets;
    }

    bool v2AddLinked = false;
    for (const auto& pool : v2MintPools) {
        auto poolNetIt = netFlow.find(pool);
        const cpp_int poolNet = (poolNetIt == netFlow.end()) ? cpp_int(0) : poolNetIt->second;
        if (poolNet <= 0 && !mintedToWallet.count(pool)) continue;
        int linkedOutgoing = 0;
        for (const auto& token : tokenOrder) {
            if (token == pool || netFlow[token] >= 0) continue;
            if (flowLinkedToPool(graph, token, wallet, {pool}, false))
                ++linkedOutgoing;
        }
        if (linkedOutgoing >= 2) { v2AddLinked = true; break; }
    }

    bool v2RemoveLinked = false;
    for (const auto& pool : v2BurnPools) {
        auto poolNetIt = netFlow.find(pool);
        const cpp_int poolNet = (poolNetIt == netFlow.end()) ? cpp_int(0) : poolNetIt->second;
        if (poolNet >= 0 && !burnedFromWallet.count(pool)) continue;
        int linkedIncoming = 0;
        for (const auto& token : tokenOrder) {
            if (token == pool || netFlow[token] <= 0) continue;
            if (flowLinkedToPool(graph, token, wallet, {pool}, true))
                ++linkedIncoming;
        }
        if (linkedIncoming >= 2) { v2RemoveLinked = true; break; }
    }

    bool v3AddLinked = false;
    bool v3RemoveLinked = false;

    bool v3CollectLinked = false;
    std::set<std::string> decreasedTokenIds;
    for (const auto& event : v3LiquidityEvents)
        if (event.kind == V3LiquidityKind::Decrease) decreasedTokenIds.insert(event.tokenId);

    for (const auto& event : v3LiquidityEvents) {
        if (event.kind == V3LiquidityKind::Increase) {
            const bool nftToWallet =
                nftEvidenceMatches(nftTransfers, event, wallet, true);
            const bool managerFlow =
                outgoingAssets >= 2 &&
                walletFlowsWithCounterparty(graph, tokenOrder, wallet, event.manager, false) >= 2;
            if (!hasSwap && outgoingAssets >= 2 && (nftToWallet || managerFlow))
                v3AddLinked = true;
        } else {
            const bool nftFromWallet =
                nftEvidenceMatches(nftTransfers, event, wallet, false);
            const bool managerFlow =
                incomingAssets >= 2 &&
                walletFlowsWithCounterparty(graph, tokenOrder, wallet, event.manager, true) >= 2;
            const bool linked = !hasSwap && incomingAssets >= 2 && (nftFromWallet || managerFlow);
            if (!linked) continue;
            if (event.kind == V3LiquidityKind::Decrease || decreasedTokenIds.count(event.tokenId))
                v3RemoveLinked = true;
            else
                v3CollectLinked = true;
        }
    }

    const bool lpAdd = v3AddLinked || v2AddLinked;
    const bool lpRemove = v3RemoveLinked || v2RemoveLinked;

    if (lpAdd || lpRemove) {
        r.dexActivityDetected = true;
        r.isSwap = false;
        r.venue = lpAdd ? "Add Liquidity" : "Remove Liquidity";

        FlowRank bestRank;
        bool haveBest = false;
        for (const auto& token : tokenOrder) {
            if (netFlow[token] == 0) continue;
            const FlowRank candidate = rankFlow(token, netFlow[token]);
            if (!haveBest || betterRank(candidate, bestRank)) {
                haveBest = true;
                bestRank = candidate;
                r.tokenAddr = token;
                r.rawAmount = absInt(netFlow[token]);
                r.isBuy = netFlow[token] > 0;
            }
        }

        if (!r.tokenAddr.empty()) {
            const int dec = getDecimals(r.tokenAddr);
            r.usdNanos = calcUsdNanos(
                r.rawAmount, dec, getPriceNanos(r.tokenAddr)
            );
        }
        return r;
    }

    if (v3CollectLinked) {
        r.valid = true;
        r.isSwap = false;
        r.dexActivityDetected = true;
        r.venue = "Collect Fees";
        FlowRank bestRank;
        bool haveBest = false;
        for (const auto& token : tokenOrder) {
            if (netFlow[token] <= 0) continue;
            const FlowRank candidate = rankFlow(token, netFlow[token]);
            if (!haveBest || betterRank(candidate, bestRank)) {
                haveBest = true;
                bestRank = candidate;
                r.tokenAddr = token;
                r.rawAmount = netFlow[token];
                r.isBuy = true;
            }
        }
        if (!r.tokenAddr.empty()) {
            const int dec = getDecimals(r.tokenAddr);
            r.usdNanos = calcUsdNanos(r.rawAmount, dec, getPriceNanos(r.tokenAddr));
        }
        return r;
    }

    std::string mainToken;
    cpp_int mainNet = 0;
    cpp_int mainAbs = 0;
    FlowRank mainRank;
    bool mainPoolLinked = false;

    for (const auto& token : tokenOrder) {
        if (isBaseAsset(token) || netFlow[token] == 0) continue;

        const cpp_int a = absInt(netFlow[token]);
        const FlowRank candidateRank = rankFlow(token, netFlow[token]);
        const bool poolLinked = flowLinkedToPool(
            graph, token, wallet, swapPools, netFlow[token] > 0
        );

        const bool better =
            mainToken.empty() ||
            (poolLinked && !mainPoolLinked) ||
            (poolLinked == mainPoolLinked && betterRank(candidateRank, mainRank));

        if (better) {
            mainToken = token;
            mainNet = netFlow[token];
            mainAbs = a;
            mainRank = candidateRank;
            mainPoolLinked = poolLinked;
        }
    }

    bool hasOppositeFlow = false;
    if (!mainToken.empty()) {
        for (const auto& token : tokenOrder) {
            if (token == mainToken || netFlow[token] == 0) continue;
            if ((mainNet > 0 && netFlow[token] < 0) ||
                (mainNet < 0 && netFlow[token] > 0)) {
                hasOppositeFlow = true;
                break;
            }
        }

        if (mainNet > 0 && nativeOut > 0) hasOppositeFlow = true;
        if (mainNet < 0 && unwrappedForWallet > 0) hasOppositeFlow = true;
    }

    const bool confirmedSwap =
        !mainToken.empty() &&
        (mainPoolLinked || (hasSwap && hasOppositeFlow));

    const bool inferredRouterSwap =
        !confirmedSwap && !mainToken.empty() &&
        hasOppositeFlow && knownRouter;

    const bool flowLooksLikeSwap = confirmedSwap || inferredRouterSwap;

    if (flowLooksLikeSwap) {
        r.dexActivityDetected = true;
        if (inferredRouterSwap)
            r.diagnosticReason = "SWAP_INFERRED_FROM_FLOW";
        r.isSwap = true;
        r.tokenAddr = mainToken;
        r.rawAmount = mainAbs;
        r.isBuy = mainNet > 0;

        std::string counter;
        cpp_int counterAbs = 0;
        FlowRank counterRank;

        for (const auto& token : tokenOrder) {
            if (token == mainToken || netFlow[token] == 0) continue;
            const bool opposite =
                (mainNet > 0 && netFlow[token] < 0) ||
                (mainNet < 0 && netFlow[token] > 0);
            if (!opposite) continue;

            const cpp_int a = absInt(netFlow[token]);
            const FlowRank candidateRank = rankFlow(token, netFlow[token]);
            const bool preferBase = isBaseAsset(token);
            const bool currentBase = !counter.empty() && isBaseAsset(counter);
            if (counter.empty() || (preferBase && !currentBase) ||
                (preferBase == currentBase && betterRank(candidateRank, counterRank))) {
                counter = token;
                counterAbs = a;
                counterRank = candidateRank;
            }
        }

        if (counter.empty() && r.isBuy && nativeOut > 0) {
            counter = g_chain.nativeMarker;
            counterAbs = nativeOut;
            if (walletIsSender && !txTo.empty()) {
                auto wIt = wrappedByDst.find(txTo);
                if (wIt != wrappedByDst.end() && wIt->second > 0 && wIt->second < counterAbs) {
                    counterAbs = wIt->second;
                    r.diagnosticReason = "NATIVE_REFUND_ADJUSTED";
                }
            }
        }
        if (counter.empty() && !r.isBuy && unwrappedForWallet > 0) {
            counter = g_chain.nativeMarker;
            counterAbs = unwrappedForWallet;
        }
        if (counter.empty() && !r.isBuy && walletIsSender && !txTo.empty()) {
            auto uIt = unwrappedByDst.find(txTo);
            if (uIt != unwrappedByDst.end() && uIt->second > 0) {
                counter = g_chain.nativeMarker;
                counterAbs = uIt->second;
                r.diagnosticReason = "NATIVE_COUNTER_FROM_ROUTER_UNWRAP";
            }
        }

        if (!counter.empty()) {
            r.counterAddr = counter;
            r.counterAmount = counterAbs;
        } else if (!r.isBuy) {

            r.diagnosticReason = "NATIVE_COUNTER_REQUIRES_TRACE";
        }

        r.venue = lookupRouterLabel(txTo);
        if (r.venue.empty() && !swapPools.empty())
            r.venue = "DEX Pool";
        if (r.venue.empty() && urCmds.present)
            r.venue = "Universal Router";
        if (r.venue.empty())
            r.venue = "DEX";

        r.usdNanos = usdFromCounterAsset(r.counterAddr, r.counterAmount);
        if (r.usdNanos <= 0) {
            const int dec = getDecimals(r.tokenAddr);
            r.usdNanos = calcUsdNanos(r.rawAmount, dec, getPriceNanos(r.tokenAddr));
        }
        r.gasUsdNanos = gasCostUsdNanos(receipt);
        return r;
    }

    FlowRank bestTransferRank;
    bool haveTransfer = false;
    for (const auto& token : tokenOrder) {
        if (netFlow[token] == 0) continue;
        const FlowRank candidate = rankFlow(token, netFlow[token]);
        if (!haveTransfer || betterRank(candidate, bestTransferRank)) {
            haveTransfer = true;
            bestTransferRank = candidate;
            r.tokenAddr = token;
            r.rawAmount = absInt(netFlow[token]);
            r.isBuy = netFlow[token] > 0;
        }
    }

    r.isSwap = false;
    if (hasSwap && mainToken.empty()) {
        int nonZeroBase = 0;
        for (const auto& token : tokenOrder)
            if (isBaseAsset(token) && netFlow[token] != 0) ++nonZeroBase;
        if (swapPools.size() >= 2 && nonZeroBase == 1) {
            r.venue = "Arbitrage";
            r.diagnosticReason = "ARBITRAGE_BASE_CYCLE";
        } else {
            r.diagnosticReason = "ONLY_BASE_ASSET_FLOW";
        }
    } else if (!mainToken.empty() && hasOppositeFlow && !flowLooksLikeSwap) {
        r.unknownReason = "UNCONFIRMED_OPPOSITE_FLOW";
        r.tokenAddr.clear();
        r.rawAmount = 0;
    } else if ((!v2MintPools.empty() || !v2BurnPools.empty() ||
                v3Increase || v3Decrease || v3Collect) && !lpAdd && !lpRemove) {
        r.unknownReason = "LP_EVENT_NOT_LINKED_TO_WALLET";
        r.tokenAddr.clear();
        r.rawAmount = 0;
    }
    if (!r.isSwap && r.venue.empty() && r.unknownReason.empty() && !r.tokenAddr.empty() && !g_chain.bridges.empty()) {
        {
            auto git = graph.find(r.tokenAddr);
            if (git != graph.end()) {
                for (const auto& e : git->second) {
                    if (e.from == wallet && g_chain.bridges.count(e.to)) { r.venue = "Bridge Out"; break; }
                    if (e.to == wallet && g_chain.bridges.count(e.from)) { r.venue = "Bridge In"; break; }
                }
            }
        }
    }

    if (!r.tokenAddr.empty()) {
        const int dec = getDecimals(r.tokenAddr);
        r.usdNanos = calcUsdNanos(
            r.rawAmount, dec, getPriceNanos(r.tokenAddr)
        );
    }

    return r;
}
