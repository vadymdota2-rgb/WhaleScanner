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
std::string lookupRouterLabel(const std::string& addr);
bool isBaseAsset(const std::string& a);

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

namespace {

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

        if (logAddr == WBNB_ADDR && (t0 == WBNB_DEPOSIT_TOPIC || t0 == WBNB_WITHDRAWAL_TOPIC)) {
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
        if (to == wa) netFlow[logAddr] += amt;
        if (fr == wa) netFlow[logAddr] -= amt;
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

    bool routerCall = !txTo.empty() && !lookupRouterLabel(txTo).empty();
    bool nativeSwapSignal = nativeOutflow && (routerCall || hasSwap || wbnbWrapped > 0);
    cpp_int nativeIn = 0;
    if (walletIsSender && hasSwap && wbnbUnwrapped > 0) nativeIn = wbnbUnwrapped;
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

    r.isSwap = (
        twoSidedFlow ||
        (!bestNonBaseTok.empty() && (hasBaseIn || hasBaseOut || nativeSwapSignal || nativeInflowSignal)) ||
        (hasBaseIn && hasBaseOut)
    );

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

    if (!r.isSwap && !r.tokenAddr.empty() && r.tokenAddr != NATIVE_BNB_MARKER &&
        !isBaseAsset(r.tokenAddr) && (nativeSwapSignal || nativeInflowSignal)) {
        r.isSwap = true;
        if (r.counterAddr.empty()) {
            if (r.isBuy && nativeOutflow && nativeSwapSignal) { r.counterAddr = NATIVE_BNB_MARKER; r.counterAmount = nativeOut; }
            else if (!r.isBuy && nativeInflowSignal) { r.counterAddr = NATIVE_BNB_MARKER; r.counterAmount = nativeIn; }
        }
    }

    int tokenDec = (r.tokenAddr == NATIVE_BNB_MARKER) ? 18 : getDecimals(r.tokenAddr);
    uint64_t tokenPrice = (r.tokenAddr == NATIVE_BNB_MARKER) ? getPriceNanos(WBNB_ADDR) : getPriceNanos(r.tokenAddr);
    r.usdNanos = calcUsdNanos(r.rawAmount, tokenDec, tokenPrice);
    return r;
}
