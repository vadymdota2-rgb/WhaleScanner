#include "tx_analyzer.h"

#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include "utils.h"

using json = nlohmann::json;

uint64_t getPriceNanos(const std::string& token);
int getDecimals(const std::string& addr);
std::string getSymbol(const std::string& addr);

cpp_int parseUint256(const std::string& h) {
    if (h.length()<66) return 0; cpp_int r=0;
    for (char c:h.substr(2,64)) { r<<=4; if (c>='0'&&c<='9') r|=(c-'0'); else if (c>='a'&&c<='f') r|=(c-'a'+10); else if (c>='A'&&c<='F') r|=(c-'A'+10); } return r;
}
cpp_int hexToCppInt(const std::string& h) {
    if (h.size() < 2 || h[0] != '0' || h[1] != 'x') return 0;
    cpp_int r = 0;
    for (size_t i = 2; i < h.size(); i++) {
        char c = h[i]; r <<= 4;
        if (c >= '0' && c <= '9') r |= (c - '0');
        else if (c >= 'a' && c <= 'f') r |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') r |= (c - 'A' + 10);
    }
    return r;
}
std::string formatAmount(const cpp_int& raw, int dec) {
    if (raw==0) return "0.00"; cpp_int d=1; for (int i=0;i<dec;i++) d*=10;
    std::string ip=(raw/d).convert_to<std::string>(), fp=(raw%d).convert_to<std::string>();
    while ((int)fp.length()<dec) fp="0"+fp; if (fp.length()>2) fp=fp.substr(0,2); return ip+"."+fp;
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

const std::string WBNB_ADDR = "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c";
const std::string NATIVE_BNB_MARKER = "native:bnb";

ChainContext makeBscContext() {
    ChainContext c;
    c.nativeSymbol = "BNB";
    c.nativeMarker = NATIVE_BNB_MARKER;
    c.wrappedNative = WBNB_ADDR;
    c.stablecoins = {
        "0x55d398326f99059ff775485246999027b3197955",
        "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d",
        "0xe9e7cea3dedca5984780bafc599bd69add087d56"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(WBNB_ADDR);
    c.baseAssets.insert("0xc5f0f7b66764f6ec8c8dff7ba683102295e16409");
    c.routers = {
        {"0x10ed43c718714eb63d5aa57b78b54704e256024e", "PancakeSwap V2"},
        {"0x13f4ea83d0bd40e75c8222255bc855a974568dd4", "PancakeSwap V3 (Smart Router)"},
        {"0x1b81d678ffb9c0263b24a97847620c99d213eb14", "PancakeSwap V3 (Swap Router)"},
        {"0x1a0a18ac4becddbd6389559687d1a73d8927e416", "PancakeSwap (Universal Router)"},
        {"0xd9c500dff816a1da21a48a732d3498bf09dc9aeb", "PancakeSwap (Universal Router 2)"},
        {"0x1111111254eeb25477b68fb85ed929f73a960582", "1inch"},
        {"0x9333c74bdd1e118634fe5664aca7a9710b108bab", "OKX DEX"},
        {"0x6015126d7d23648c2e4466693b8deab005ffaba8", "OKX DEX"},
        {"0x6131b5fae19ea4f9d964eac0408e4408b66337b5", "KyberSwap"},
        {"0xdf1a1b60f2d438842916c0adc43748768353ec25", "KyberSwap"},
        {"0x6352a56caadc4f1e25cd6c75970fa768a3304e64", "OpenOcean"},
        {"0x3a6d8ca21d1cf76f653a67577fa0d27453350dd8", "BiSwap"},
        {"0xcf0febd3f17cef5b47b0cd257acf6025c5bff3b7", "ApeSwap"},
    };
    return c;
}

namespace {
ChainContext g_chain = makeBscContext();
}

const ChainContext& chainCtx() { return g_chain; }
void setChainContext(const ChainContext& ctx) { g_chain = ctx; }

bool isBaseAsset(const std::string& a) { return g_chain.baseAssets.count(toLower(a)) > 0; }
bool isStablecoin(const std::string& a) { return g_chain.stablecoins.count(toLower(a)) > 0; }
std::string lookupRouterLabel(const std::string& addr) {
    auto it = g_chain.routers.find(toLower(addr));
    return it != g_chain.routers.end() ? it->second : std::string();
}

namespace {

const std::string ZERO_ADDR = "0x0000000000000000000000000000000000000000";
const std::string DEAD_ADDR = "0x000000000000000000000000000000000000dead";

const std::set<std::string> SWAP_EVENT_TOPICS = {
    "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822",
    "0xc42079f94a6350d7e6235f29174924f928cc2ac818eb64fed8004e115fbcca67",
    "0x19b47279256b2a23a1665c810c8d55a1758940ee09377d4f8d26497a3577dc83",
};
const std::string ERC20_TRANSFER_TOPIC =
    "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
const std::string WBNB_DEPOSIT_TOPIC =
    "0xe1fffcc4923d04b559f4d29a8bfc6cda04eb5b0d3c460751c2402c5c5cc9109c";
const std::string WBNB_WITHDRAWAL_TOPIC =
    "0x7fcf532c15f0a6db0bd6d0e038bea71d30d808c7d98cb3bf7268a95bf5081b65";

}

TxResult analyzeTx(const json& tx, const json& receipt, const std::string& wa) {
    TxResult r={}; if (receipt.is_null()||!receipt.is_object()||!receipt.contains("logs")||!receipt["logs"].is_array()) return r;

    bool hasSwap=false;
    std::string swapLogAddr;
    size_t swapLogDataHexLen=0;
    cpp_int wbnbWrapped = 0;
    cpp_int wbnbUnwrapped = 0;

    std::map<std::string, cpp_int> netFlow;
    std::vector<std::string> tokenOrder;
    auto touch = [&](const std::string& tok) {
        if (netFlow.find(tok) == netFlow.end()) { netFlow[tok] = 0; tokenOrder.push_back(tok); }
    };

    bool anyTransferForWallet = false;
    std::string firstCounterpartAddr;
    std::set<std::string> mintedIn;
    std::set<std::string> burnedOut;
    std::set<std::string> outCounterparties;
    std::set<std::string> inCounterparties;
    bool bridgeTouched = false;

    for (auto& l : receipt["logs"]) {
        if (!l.is_object()||!l.contains("topics")||!l["topics"].is_array()||l["topics"].empty()) continue;
        if (!l["topics"][0].is_string()) continue;
        const std::string t0 = l["topics"][0].get<std::string>();
        std::string logAddr = (l.contains("address") && l["address"].is_string())
            ? toLower(l["address"].get<std::string>()) : "";

        if (SWAP_EVENT_TOPICS.count(t0)) {
            hasSwap = true;
            if (swapLogAddr.empty()) {
                swapLogAddr = logAddr;
                if (l.contains("data") && l["data"].is_string()) {
                    const std::string& d = l["data"].get_ref<const std::string&>();
                    swapLogDataHexLen = d.size() >= 2 ? d.size() - 2 : 0;
                }
            }
            continue;
        }

        if (logAddr == g_chain.wrappedNative && (t0 == WBNB_DEPOSIT_TOPIC || t0 == WBNB_WITHDRAWAL_TOPIC)) {
            if (l.contains("data") && l["data"].is_string()) {
                cpp_int wad = parseUint256(l["data"].get<std::string>());
                if (t0 == WBNB_DEPOSIT_TOPIC) wbnbWrapped += wad; else wbnbUnwrapped += wad;
            }
            continue;
        }

        if (t0 != ERC20_TRANSFER_TOPIC) continue;
        if (l["topics"].size() != 3) continue;
        if (!l.contains("data")||!l["data"].is_string()) continue;
        if (!l["topics"][1].is_string()||!l["topics"][2].is_string()||logAddr.empty()) continue;

        const std::string& t1 = l["topics"][1].get_ref<const std::string&>();
        const std::string& t2 = l["topics"][2].get_ref<const std::string&>();
        const std::string& dataField = l["data"].get_ref<const std::string&>();
        if (t1.length() < 66 || t2.length() < 66) continue;

        std::string fr = "0x"+toLower(t1.substr(26));
        std::string to = "0x"+toLower(t2.substr(26));
        if (fr != wa && to != wa) continue;
        cpp_int amt = parseUint256(dataField);
        if (amt == 0) continue;
        touch(logAddr);
        anyTransferForWallet = true;
        if (firstCounterpartAddr.empty()) firstCounterpartAddr = (to == wa) ? fr : to;
        if (to == wa) {
            netFlow[logAddr] += amt;
            if (fr == ZERO_ADDR) mintedIn.insert(logAddr);
            else inCounterparties.insert(fr);
        }
        if (fr == wa) {
            netFlow[logAddr] -= amt;
            if (to == ZERO_ADDR || to == DEAD_ADDR) burnedOut.insert(logAddr);
            else outCounterparties.insert(to);
            if (g_chain.bridges.count(to)) bridgeTouched = true;
        }
        if (g_chain.bridges.count(fr)) bridgeTouched = true;
    }

    cpp_int nativeOut = 0;
    std::string txTo;
    bool walletIsSender = false;
    if (tx.is_object()) {
        if (tx.contains("to") && !tx["to"].is_null() && tx["to"].is_string())
            txTo = toLower(tx["to"].get<std::string>());
        if (tx.contains("from") && tx["from"].is_string() &&
            toLower(tx["from"].get<std::string>()) == wa) {
            walletIsSender = true;
            if (tx.contains("value") && tx["value"].is_string())
                nativeOut = hexToCppInt(tx["value"].get<std::string>());
        }
    }
    bool nativeOutflow = nativeOut > 0;

    if (!anyTransferForWallet) return r;
    r.valid = true;

    if (!swapLogAddr.empty()) r.venue = lookupRouterLabel(swapLogAddr);
    if (r.venue.empty() && !firstCounterpartAddr.empty()) r.venue = lookupRouterLabel(firstCounterpartAddr);
    if (r.venue.empty() && !txTo.empty()) r.venue = lookupRouterLabel(txTo);
    if (r.venue.empty() && hasSwap && swapLogDataHexLen > 0) {
        if (swapLogDataHexLen == 256) r.venue = "unknown pool (V2-style)";
        else if (swapLogDataHexLen == 320) r.venue = "unknown pool (V3-style)";
        else if (swapLogDataHexLen == 448) r.venue = "unknown pool (V3-style)";
    }

    if (!txTo.empty() && g_chain.bridges.count(txTo)) bridgeTouched = true;
    bool routerCall = !txTo.empty() && !lookupRouterLabel(txTo).empty();
    bool nativeSwapSignal = nativeOutflow && (routerCall || hasSwap || wbnbWrapped > 0);
    cpp_int nativeIn = 0;
    if (walletIsSender && wbnbUnwrapped > 0) nativeIn = wbnbUnwrapped;
    bool nativeInflowSignal = nativeIn > 0;

    bool anyIn = false, anyOut = false;
    for (auto& tok : tokenOrder) {
        if (netFlow[tok] > 0) anyIn = true;
        if (netFlow[tok] < 0) anyOut = true;
    }
    bool twoSidedFlow = anyIn && anyOut;

    std::string bestNonBaseTok; cpp_int bestNonBaseAbs = -1; cpp_int bestNonBaseNet = 0;
    bool hasBaseIn=false, hasBaseOut=false;

    for (auto& tok : tokenOrder) {
        cpp_int net = netFlow[tok];
        if (isBaseAsset(tok)) {
            if (net > 0) hasBaseIn = true;
            if (net < 0) hasBaseOut = true;
        } else {
            if (net <= 0) continue;
            if (net > bestNonBaseAbs) { bestNonBaseAbs = net; bestNonBaseTok = tok; bestNonBaseNet = net; }
        }
    }
    if (bestNonBaseTok.empty()) {
        for (auto& tok : tokenOrder) {
            if (isBaseAsset(tok)) continue;
            cpp_int net = netFlow[tok];
            cpp_int absNet = net >= 0 ? net : -net;
            if (absNet > bestNonBaseAbs) { bestNonBaseAbs = absNet; bestNonBaseTok = tok; bestNonBaseNet = net; }
        }
    }

    bool lpAdd = false, lpRemove = false;
    {
        bool sentBase = hasBaseOut || nativeOutflow;
        bool sentNonBase = false, gotNonBase = false;
        for (auto& tok : tokenOrder) {
            if (isBaseAsset(tok)) continue;
            cpp_int net = netFlow[tok];
            if (net > 0) gotNonBase = true;
            if (net < 0) sentNonBase = true;
        }
        bool gotBase = hasBaseIn || nativeInflowSignal;
        for (auto& tok : tokenOrder) {
            cpp_int net = netFlow[tok];
            bool poolTokenIn = outCounterparties.count(tok) > 0;
            bool poolTokenOut = inCounterparties.count(tok) > 0;
            if (net > 0 && poolTokenIn && sentBase && sentNonBase) lpAdd = true;
            if (net > 0 && mintedIn.count(tok) && poolTokenIn) lpAdd = true;
            if (net < 0 && poolTokenOut && gotBase && gotNonBase) lpRemove = true;
            if (net < 0 && burnedOut.count(tok) && gotBase && gotNonBase) lpRemove = true;
        }
    }

    enum class Confidence { Unknown, Likely, Confirmed };
    Confidence conf = Confidence::Unknown;

    bool dexSignal = hasSwap || routerCall || nativeSwapSignal || nativeInflowSignal;
    bool flowSwap = (
        (!bestNonBaseTok.empty() && (hasBaseIn || hasBaseOut || nativeSwapSignal || nativeInflowSignal)) ||
        (twoSidedFlow && dexSignal)
    );

    if (flowSwap) conf = hasSwap ? Confidence::Confirmed : Confidence::Likely;
    if (lpAdd || lpRemove || bridgeTouched) conf = Confidence::Unknown;
    if (!bestNonBaseTok.empty()) {
        cpp_int net = netFlow[bestNonBaseTok];
        if (net > 0 && mintedIn.count(bestNonBaseTok) && !(hasBaseOut || nativeOutflow)) conf = Confidence::Unknown;
        if (net < 0 && burnedOut.count(bestNonBaseTok) && !(hasBaseIn || nativeInflowSignal)) conf = Confidence::Unknown;
    }

    r.isSwap = (conf != Confidence::Unknown);

    if (!bestNonBaseTok.empty()) {
        r.tokenAddr = bestNonBaseTok;
        r.rawAmount = bestNonBaseAbs;
        r.isBuy = bestNonBaseNet > 0;

        if (r.isSwap) {
            std::string bestCounterTok; cpp_int bestCounterAbs = -1;
            for (auto& tok : tokenOrder) {
                if (!isBaseAsset(tok)) continue;
                cpp_int net = netFlow[tok];
                bool wantsOutflow = r.isBuy;
                if (wantsOutflow && net >= 0) continue;
                if (!wantsOutflow && net <= 0) continue;
                cpp_int absNet = net >= 0 ? net : -net;
                if (absNet > bestCounterAbs) { bestCounterAbs = absNet; bestCounterTok = tok; }
            }
            if (bestCounterTok.empty() && r.isBuy && nativeOutflow && nativeSwapSignal) {
                bestCounterTok = NATIVE_BNB_MARKER; bestCounterAbs = nativeOut;
            }
            if (bestCounterTok.empty() && !r.isBuy && nativeInflowSignal) {
                bestCounterTok = NATIVE_BNB_MARKER; bestCounterAbs = nativeIn;
            }
            if (bestCounterTok.empty()) {
                for (auto& tok : tokenOrder) {
                    if (tok == r.tokenAddr) continue;
                    cpp_int net = netFlow[tok];
                    bool wantsOutflow = r.isBuy;
                    if (wantsOutflow && net >= 0) continue;
                    if (!wantsOutflow && net <= 0) continue;
                    cpp_int absNet = net >= 0 ? net : -net;
                    if (absNet > bestCounterAbs) { bestCounterAbs = absNet; bestCounterTok = tok; }
                }
            }
            if (!bestCounterTok.empty()) { r.counterAddr = bestCounterTok; r.counterAmount = bestCounterAbs; }
            if (bestCounterTok.empty() && conf == Confidence::Likely) { r.isSwap = false; conf = Confidence::Unknown; }
        }
    } else {
        std::string bestBaseTok; cpp_int bestBaseAbs = -1; cpp_int bestBaseNet = 0;
        for (auto& tok : tokenOrder) {
            if (!isBaseAsset(tok)) continue;
            cpp_int net = netFlow[tok];
            if (net <= 0) continue;
            if (net > bestBaseAbs) { bestBaseAbs = net; bestBaseTok = tok; bestBaseNet = net; }
        }
        if (bestBaseTok.empty()) {
            for (auto& tok : tokenOrder) {
                if (!isBaseAsset(tok)) continue;
                cpp_int net = netFlow[tok];
                cpp_int absNet = net >= 0 ? net : -net;
                if (absNet > bestBaseAbs) { bestBaseAbs = absNet; bestBaseTok = tok; bestBaseNet = net; }
            }
        }
        if (!bestBaseTok.empty()) {
            r.tokenAddr = bestBaseTok;
            r.rawAmount = bestBaseAbs;
            r.isBuy = bestBaseNet > 0;

            std::string bestCounterTok; cpp_int bestCounterAbs = -1;
            for (auto& tok : tokenOrder) {
                if (tok == r.tokenAddr || !isBaseAsset(tok)) continue;
                cpp_int net = netFlow[tok];
                bool wantsOutflow = r.isBuy;
                if (wantsOutflow && net >= 0) continue;
                if (!wantsOutflow && net <= 0) continue;
                cpp_int absNet = net >= 0 ? net : -net;
                if (absNet > bestCounterAbs) { bestCounterAbs = absNet; bestCounterTok = tok; }
            }
            if (bestCounterTok.empty() && r.isBuy && nativeOutflow && nativeSwapSignal) {
                bestCounterTok = NATIVE_BNB_MARKER; bestCounterAbs = nativeOut;
            }
            if (bestCounterTok.empty() && !r.isBuy && nativeInflowSignal) {
                bestCounterTok = NATIVE_BNB_MARKER; bestCounterAbs = nativeIn;
            }
            if (!bestCounterTok.empty()) { r.counterAddr = bestCounterTok; r.counterAmount = bestCounterAbs; }
        } else if (nativeOutflow && !tokenOrder.empty()) {
            r.tokenAddr = tokenOrder.front();
            cpp_int net = netFlow[r.tokenAddr];
            r.rawAmount = net >= 0 ? net : -net;
            r.isBuy = net > 0;
            if (r.isBuy && nativeSwapSignal) {
                r.counterAddr = NATIVE_BNB_MARKER;
                r.counterAmount = nativeOut;
            }
        } else {
            r.tokenAddr = tokenOrder.front();
            cpp_int net = netFlow[r.tokenAddr];
            r.rawAmount = net >= 0 ? net : -net;
            r.isBuy = net > 0;
        }
    }

    if (!r.isSwap && conf != Confidence::Unknown && !r.tokenAddr.empty() && r.tokenAddr != NATIVE_BNB_MARKER &&
        !isBaseAsset(r.tokenAddr) && (nativeSwapSignal || nativeInflowSignal)) {
        r.isSwap = true;
        if (r.counterAddr.empty()) {
            if (r.isBuy && nativeOutflow && nativeSwapSignal) { r.counterAddr = NATIVE_BNB_MARKER; r.counterAmount = nativeOut; }
            else if (!r.isBuy && nativeInflowSignal) { r.counterAddr = NATIVE_BNB_MARKER; r.counterAmount = nativeIn; }
        }
    }

    int tokenDec = (r.tokenAddr == NATIVE_BNB_MARKER) ? 18 : getDecimals(r.tokenAddr);
    uint64_t tokenPrice = (r.tokenAddr == NATIVE_BNB_MARKER) ? getPriceNanos(g_chain.wrappedNative) : getPriceNanos(r.tokenAddr);
    r.usdNanos = calcUsdNanos(r.rawAmount, tokenDec, tokenPrice);

    if (r.isSwap && !r.counterAddr.empty() && r.counterAmount > 0) {
        bool counterIsNative = (r.counterAddr == NATIVE_BNB_MARKER);
        int counterDec = counterIsNative ? 18 : getDecimals(r.counterAddr);
        uint64_t counterPrice = counterIsNative ? getPriceNanos(g_chain.wrappedNative) : getPriceNanos(r.counterAddr);
        cpp_int counterUsd = calcUsdNanos(r.counterAmount, counterDec, counterPrice);
        if (counterUsd > 0) r.usdNanos = counterUsd;
    }
    return r;
}
