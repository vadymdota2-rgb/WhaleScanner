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

namespace {

struct UniversalRouterCommands {
    bool present = false;
    bool hasWrap = false, hasUnwrap = false;
    bool hasV2Swap = false, hasV3Swap = false;
    bool hasSweep = false, hasTransfer = false;
    bool hasPermit2 = false;
};

enum class CallIntent { NONE, BUY, SELL, TOKEN_SWAP };

struct DecodedCall {
    bool decoded = false;
    std::string function;
    std::string protocol;
    CallIntent intent = CallIntent::NONE;
    std::string tokenIn, tokenOut;
    bool nativeIn = false, nativeOut = false;
    bool viaMulticall = false;
};

struct V2SelInfo { int pathWord; bool ethIn; bool ethOut; const char* name; };
const std::map<std::string, V2SelInfo> V2_SELECTORS = {
    {"7ff36ab5", {1, true,  false, "swapExactETHForTokens"}},
    {"b6f9de95", {1, true,  false, "swapExactETHForTokensSupportingFeeOnTransferTokens"}},
    {"fb3bdb41", {1, true,  false, "swapETHForExactTokens"}},
    {"18cbafe5", {2, false, true,  "swapExactTokensForETH"}},
    {"791ac947", {2, false, true,  "swapExactTokensForETHSupportingFeeOnTransferTokens"}},
    {"4a25d94a", {2, false, true,  "swapTokensForExactETH"}},
    {"38ed1739", {2, false, false, "swapExactTokensForTokens"}},
    {"5c11d795", {2, false, false, "swapExactTokensForTokensSupportingFeeOnTransferTokens"}},
    {"8803dbee", {2, false, false, "swapTokensForExactTokens"}},
};
const std::map<std::string, const char*> V3_SINGLE_SELECTORS = {
    {"414bf389", "exactInputSingle"},
    {"04e45aaf", "exactInputSingle"},
    {"db3e2198", "exactOutputSingle"},
    {"09b81346", "exactOutputSingle"},
};
const std::map<std::string, bool> V3_PATH_SELECTORS = {
    {"c04b8d59", false},
    {"b858183f", false},
    {"f28c0498", true},
    {"5023b4df", true},
};

std::string wordAddr(const std::string& args, size_t wordIdx) {
    size_t pos = wordIdx * 64;
    if (args.size() < pos + 64) return "";
    return "0x" + toLower(args.substr(pos + 24, 40));
}

std::vector<std::string> decodeAddressArray(const std::string& args, int offsetWordIdx) {
    std::vector<std::string> out;
    uint64_t off = hexWordToU64(args, (size_t)offsetWordIdx * 64);
    size_t lenPos = off * 2;
    if (off > args.size() || args.size() < lenPos + 64) return out;
    uint64_t len = hexWordToU64(args, lenPos);
    if (len == 0 || len > 8) return out;
    if (args.size() < lenPos + 64 + len * 64) return out;
    for (uint64_t i = 0; i < len; i++) out.push_back("0x" + toLower(args.substr(lenPos + 64 + i * 64 + 24, 40)));
    return out;
}

CallIntent intentFromPair(const std::string& tIn, const std::string& tOut) {
    if (tIn.empty() || tOut.empty()) return CallIntent::NONE;
    bool bIn = isBaseAsset(tIn), bOut = isBaseAsset(tOut);
    if (bIn && !bOut) return CallIntent::BUY;
    if (!bIn && bOut) return CallIntent::SELL;
    if (!bIn && !bOut) return CallIntent::TOKEN_SWAP;
    return CallIntent::NONE;
}

DecodedCall decodeCall(const std::string& input, int depth) {
    DecodedCall dc;
    if (depth > 3) return dc;
    if (input.size() < 10 || input[0] != '0' || input[1] != 'x') return dc;
    std::string sel = toLower(input.substr(2, 8));
    std::string args = input.substr(10);

    auto v2 = V2_SELECTORS.find(sel);
    if (v2 != V2_SELECTORS.end()) {
        auto path = decodeAddressArray(args, v2->second.pathWord);
        if (path.size() < 2) return dc;
        dc.decoded = true; dc.function = v2->second.name; dc.protocol = "V2 Router";
        dc.tokenIn = path.front(); dc.tokenOut = path.back();
        dc.nativeIn = v2->second.ethIn; dc.nativeOut = v2->second.ethOut;
        if (dc.nativeIn) dc.intent = CallIntent::BUY;
        else if (dc.nativeOut) dc.intent = CallIntent::SELL;
        else dc.intent = intentFromPair(dc.tokenIn, dc.tokenOut);
        return dc;
    }

    auto v3s = V3_SINGLE_SELECTORS.find(sel);
    if (v3s != V3_SINGLE_SELECTORS.end()) {
        std::string tIn = wordAddr(args, 0), tOut = wordAddr(args, 1);
        if (tIn.empty() || tOut.empty()) return dc;
        dc.decoded = true; dc.function = v3s->second; dc.protocol = "V3 Router";
        dc.tokenIn = tIn; dc.tokenOut = tOut;
        dc.intent = intentFromPair(tIn, tOut);
        return dc;
    }

    auto v3p = V3_PATH_SELECTORS.find(sel);
    if (v3p != V3_PATH_SELECTORS.end()) {
        uint64_t structOff = hexWordToU64(args, 0);
        size_t sPos = structOff * 2;
        if (structOff > args.size() || args.size() < sPos + 64) return dc;
        uint64_t pathOff = hexWordToU64(args, sPos);
        size_t lenPos = sPos + pathOff * 2;
        if (args.size() < lenPos + 64) return dc;
        uint64_t byteLen = hexWordToU64(args, lenPos);
        if (byteLen < 43 || byteLen > 200 || args.size() < lenPos + 64 + byteLen * 2) return dc;
        std::string first = "0x" + toLower(args.substr(lenPos + 64, 40));
        std::string last = "0x" + toLower(args.substr(lenPos + 64 + (byteLen - 20) * 2, 40));
        dc.decoded = true; dc.protocol = "V3 Router";
        if (v3p->second) { dc.function = "exactOutput"; dc.tokenIn = last; dc.tokenOut = first; }
        else { dc.function = "exactInput"; dc.tokenIn = first; dc.tokenOut = last; }
        dc.intent = intentFromPair(dc.tokenIn, dc.tokenOut);
        return dc;
    }

    int arrWord = -1;
    if (sel == "ac9650d8") arrWord = 0;
    else if (sel == "5ae401dc") arrWord = 1;
    if (arrWord >= 0) {
        uint64_t arrOff = hexWordToU64(args, (size_t)arrWord * 64);
        size_t lenPos = arrOff * 2;
        if (arrOff > args.size() || args.size() < lenPos + 64) return dc;
        uint64_t n = hexWordToU64(args, lenPos);
        if (n == 0 || n > 16) return dc;
        size_t dataStart = lenPos + 64;
        for (uint64_t i = 0; i < n; i++) {
            if (args.size() < dataStart + i * 64 + 64) break;
            uint64_t elemOff = hexWordToU64(args, dataStart + i * 64);
            size_t ePos = dataStart + elemOff * 2;
            if (args.size() < ePos + 64) continue;
            uint64_t eLen = hexWordToU64(args, ePos);
            if (eLen < 4 || args.size() < ePos + 64 + eLen * 2) continue;
            DecodedCall inner = decodeCall("0x" + args.substr(ePos + 64, eLen * 2), depth + 1);
            if (inner.decoded) { inner.viaMulticall = true; return inner; }
        }
        return dc;
    }

    if (sel == "3593564c" || sel == "24856bc3") {
        dc.decoded = true; dc.function = "execute"; dc.protocol = "Universal Router";
        return dc;
    }

    return dc;
}

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
    uint64_t v = 0;
    for (size_t i = hexPos + 32; i < hexPos + 64; i++) v = (v << 4) | (uint64_t)hexNibble(hexNo0x[i]);
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
    size_t lenHexPos = 8 + offsetCommands * 2;
    if (offsetCommands > hexNo0x.size() || hexNo0x.size() < lenHexPos + 64) return out;

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

const std::string WBNB_ADDR = "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c";
const std::string NATIVE_BNB_MARKER = "native:bnb";

ChainContext makeBscContext() {
    ChainContext c;
    c.displayName = "BNB Smart Chain";
    c.explorerUrl = "https://bscscan.com";
    c.explorerName = "BscScan";
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
        {"0x5dc88340e1c5c6366864ee415d6034cadd1a9897", "Uniswap (Universal Router)"},
        {"0xec8b0f7ffe3ae75d7ffab09429e3675bb63503e4", "Uniswap (Universal Router)"},
        {"0x1906c1d672b88cd1b9ac7593301ca990f94eae07", "Uniswap V4 (Universal Router)"},
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
        {"0x7dae51bd3e3376b8c7c4900e9107f12be3af1ba8", "MDEX"},
        {"0x114f84658c99aa6ea62e3160a87a16deaf7efe83", "WOOFi"},
        {"0xcef5be73ae943b77f9bc08859367d923c030a269", "WOOFi"},
    };
    return c;
}

ChainContext makeEthereumContext() {
    ChainContext c;
    const std::string WETH_ADDR = "0xc02aaa39b223fe8d0a0e5c4f27ead9083c756cc2";
    c.displayName = "Ethereum";
    c.explorerUrl = "https://etherscan.io";
    c.explorerName = "Etherscan";
    c.nativeSymbol = "ETH";
    c.nativeMarker = "native:eth";
    c.wrappedNative = WETH_ADDR;
    c.stablecoins = {
        "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
        "0xdac17f958d2ee523a2206206994597c13d831ec7",
        "0x6b175474e89094c44da98b954eedeac495271d0f"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(WETH_ADDR);
    c.routers = {
        {"0x7a250d5630b4cf539739df2c5dacb4c659f2488d", "Uniswap V2"},
        {"0xe592427a0aece92de3edee1f18e0157c05861564", "Uniswap V3"},
        {"0xef1c6e67703c7bd7107eed8303fbe6ec2554bf6b", "Uniswap (Universal Router)"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)"},
        {"0x66a9893cc07d91d95644aedd05d03f95e1dba8af", "Uniswap V4 (Universal Router)"},
    };
    return c;
}

ChainContext makeBaseContext() {
    ChainContext c;
    const std::string WETH_ADDR = "0x4200000000000000000000000000000000000006";
    c.displayName = "Base";
    c.explorerUrl = "https://basescan.org";
    c.explorerName = "BaseScan";
    c.nativeSymbol = "ETH";
    c.nativeMarker = "native:eth";
    c.wrappedNative = WETH_ADDR;
    c.stablecoins = {
        "0x833589fcd6edb6e08f4c7c32d4f71b54bda02913",
        "0xfde4c96c8593536e31f229ea8f37b2ada2699bb2",
        "0x50c5725949a6f0c72e6c4a641f24049a917db0cb"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(WETH_ADDR);
    c.routers = {
        {"0x198ef79f1f515f02dfe9e3115ed9fc07183f02fc", "Uniswap (Universal Router)"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)"},
        {"0x6ff5693b99212da76ad316178a184ab56d299b43", "Uniswap V4 (Universal Router)"},
    };
    return c;
}

ChainContext makeArbitrumContext() {
    ChainContext c;
    const std::string WETH_ADDR = "0x82af49447d8a07e3bd95bd0d56f35241523fbab1";
    c.displayName = "Arbitrum One";
    c.explorerUrl = "https://arbiscan.io";
    c.explorerName = "Arbiscan";
    c.nativeSymbol = "ETH";
    c.nativeMarker = "native:eth";
    c.wrappedNative = WETH_ADDR;
    c.stablecoins = {
        "0xaf88d065e77c8cc2239327c5edb3a432268e5831",
        "0xfd086bc7cd5c481dcc9c85ebe478a1c0b69fcbb9",
        "0xda10009cbd5d07dd0cecc66161fc93d7c9000da1"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(WETH_ADDR);
    c.routers = {
        {"0x4c60051384bd2d3c01bfc845cf5f4b44bcbe9de5", "Uniswap (Universal Router)"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)"},
        {"0xe592427a0aece92de3edee1f18e0157c05861564", "Uniswap V3"},
        {"0x68b3465833fb72a70ecdf485e0e4c7bd8665fc45", "Uniswap V3 (Router 2)"},
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
const std::string V3_INCREASE_LIQUIDITY_TOPIC =
    "0x3067048beee31b25b2f1681f88dac838c8bba36af25bfb2b7cf7473a5847e35f";
const std::string V3_DECREASE_LIQUIDITY_TOPIC =
    "0x26f6a048ee9138f2c0ce266f322cb99228e8d619ae2bff30c67f8dcf9d2377b4";
const std::string V3_COLLECT_TOPIC =
    "0x40d0efd1a53d60ecbf40971b9daf7dc90178c3aadc7aab1765632738fa8b8f01";

}

TxResult analyzeTx(const json& tx, const json& receipt, const std::string& wa) {
    TxResult r={}; if (receipt.is_null()||!receipt.is_object()||!receipt.contains("logs")||!receipt["logs"].is_array()) return r;

    bool hasSwap=false;
    std::string swapLogAddr;
    std::set<std::string> allSwapPoolAddrs;
    size_t swapLogDataHexLen=0;
    cpp_int wbnbWrapped = 0;
    cpp_int wbnbUnwrapped = 0;

    std::map<std::string, cpp_int> netFlow;
    std::vector<std::string> tokenOrder;
    auto touch = [&](const std::string& tok) {
        if (netFlow.find(tok) == netFlow.end()) { netFlow[tok] = 0; tokenOrder.push_back(tok); }
    };

    bool anyTransferForWallet = false;
    bool v3PositionIncrease = false, v3PositionDecrease = false, v3Collect = false;
    std::string firstCounterpartAddr;
    std::set<std::string> mintedIn;
    std::set<std::string> burnedOut;
    std::set<std::string> outCounterparties;
    std::set<std::string> inCounterparties;
    std::map<std::string, std::set<std::string>> inSources;
    std::map<std::string, std::set<std::string>> outDestinations;

    for (auto& l : receipt["logs"]) {
        if (!l.is_object()||!l.contains("topics")||!l["topics"].is_array()||l["topics"].empty()) continue;
        if (!l["topics"][0].is_string()) continue;
        const std::string t0 = l["topics"][0].get<std::string>();
        std::string logAddr = (l.contains("address") && l["address"].is_string())
            ? toLower(l["address"].get<std::string>()) : "";

        if (SWAP_EVENT_TOPICS.count(t0)) {
            hasSwap = true;
            if (!logAddr.empty()) allSwapPoolAddrs.insert(logAddr);
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

        if (t0 == V3_INCREASE_LIQUIDITY_TOPIC) { v3PositionIncrease = true; continue; }
        if (t0 == V3_DECREASE_LIQUIDITY_TOPIC) { v3PositionDecrease = true; continue; }
        if (t0 == V3_COLLECT_TOPIC) { v3Collect = true; continue; }

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
            inSources[logAddr].insert(fr);
            if (fr == "0x0000000000000000000000000000000000000000") mintedIn.insert(logAddr);
            else inCounterparties.insert(fr);
        }
        if (fr == wa) {
            netFlow[logAddr] -= amt;
            outDestinations[logAddr].insert(to);
            if (to == "0x0000000000000000000000000000000000000000" ||
                to == "0x000000000000000000000000000000000000dead") burnedOut.insert(logAddr);
            else outCounterparties.insert(to);
        }
    }

    cpp_int nativeOut = 0;
    std::string txTo;
    bool walletIsSender = false;
    UniversalRouterCommands urCmds;
    DecodedCall dc;
    bool isGenericMulticall = false;
    if (tx.is_object()) {
        if (tx.contains("to") && !tx["to"].is_null() && tx["to"].is_string())
            txTo = toLower(tx["to"].get<std::string>());
        if (tx.contains("from") && tx["from"].is_string() &&
            toLower(tx["from"].get<std::string>()) == wa) {
            walletIsSender = true;
            if (tx.contains("value") && tx["value"].is_string())
                nativeOut = hexToCppInt(tx["value"].get<std::string>());
        }
        if (walletIsSender && tx.contains("input") && tx["input"].is_string()) {
            const std::string inputStr = tx["input"].get<std::string>();
            urCmds = parseExecuteCommands(inputStr);
            isGenericMulticall = isGenericMulticallSelector(inputStr);
            dc = decodeCall(inputStr, 0);
        }
    }
    bool nativeOutflow = nativeOut > 0;

    if (!anyTransferForWallet) {
        if (walletIsSender && (wbnbWrapped > 0 || wbnbUnwrapped > 0)) {
            r.valid = true;
            r.isSwap = false;
            r.tokenAddr = g_chain.wrappedNative;
            if (wbnbWrapped > 0) { r.venue = "Wrap"; r.rawAmount = wbnbWrapped; r.isBuy = true; }
            else { r.venue = "Unwrap"; r.rawAmount = wbnbUnwrapped; r.isBuy = false; }
            int wrapDec = getDecimals(r.tokenAddr);
            uint64_t wrapPrice = getPriceNanos(r.tokenAddr);
            r.usdNanos = calcUsdNanos(r.rawAmount, wrapDec, wrapPrice);
            r.hasSwapEvent = hasSwap; r.isUniversalRouter = urCmds.present;
            r.isGenericMulticall = isGenericMulticall; r.hasPermit2Signal = urCmds.hasPermit2;
            r.dexActivityDetected = hasSwap || urCmds.present || isGenericMulticall || urCmds.hasPermit2;
            return r;
        }
        return r;
    }
    r.valid = true;

    if (r.venue.empty()) for (auto& addr : allSwapPoolAddrs) { r.venue = lookupRouterLabel(addr); if (!r.venue.empty()) break; }
    if (r.venue.empty() && !firstCounterpartAddr.empty()) r.venue = lookupRouterLabel(firstCounterpartAddr);
    if (r.venue.empty() && !txTo.empty()) r.venue = lookupRouterLabel(txTo);
    if (r.venue.empty() && urCmds.present) {
        if (urCmds.hasV3Swap) r.venue = "Universal Router (V3-style)";
        else if (urCmds.hasV2Swap) r.venue = "Universal Router (V2-style)";
        else r.venue = "Universal Router";
    }
    if (r.venue.empty() && hasSwap && swapLogDataHexLen > 0) {
        if (swapLogDataHexLen == 256) r.venue = "unknown pool (V2-style)";
        else if (swapLogDataHexLen == 320) r.venue = "unknown pool (V3-style)";
        else if (swapLogDataHexLen == 448) r.venue = "unknown pool (V3-style)";
    }

    bool routerCall = (!txTo.empty() && !lookupRouterLabel(txTo).empty()) || urCmds.present || isGenericMulticall || urCmds.hasPermit2;
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

    bool sentBase = false, sentNonBase = false, gotBase = false, gotNonBase = false;
    for (auto& tok : tokenOrder) {
        cpp_int net = netFlow[tok];
        if (isBaseAsset(tok)) {
            if (net > 0) gotBase = true;
            if (net < 0) sentBase = true;
        } else {
            if (net > 0) gotNonBase = true;
            if (net < 0) sentNonBase = true;
        }
    }
    bool lpAdd = false, lpRemove = false;
    for (auto& tok : tokenOrder) {
        cpp_int net = netFlow[tok];
        if (net > 0 && mintedIn.count(tok) && sentBase && sentNonBase) lpAdd = true;
        if (net < 0 && burnedOut.count(tok) && gotBase && gotNonBase) lpRemove = true;
    }
    if (!lpAdd) {
        for (auto& tok : tokenOrder) {
            cpp_int net = netFlow[tok];
            bool poolTokenIn = outCounterparties.count(tok) > 0;
            if (net > 0 && poolTokenIn && sentBase && sentNonBase) { lpAdd = true; break; }
        }
    }
    if (!lpAdd) {
        for (auto& tok : tokenOrder) {
            cpp_int net = netFlow[tok];
            bool poolTokenIn = outCounterparties.count(tok) > 0;
            if (net > 0 && poolTokenIn && mintedIn.count(tok)) { lpAdd = true; break; }
        }
    }
    if (!lpRemove) {
        for (auto& tok : tokenOrder) {
            cpp_int net = netFlow[tok];
            bool poolTokenOut = inCounterparties.count(tok) > 0;
            if (net < 0 && poolTokenOut) { lpRemove = true; break; }
        }
    }
    if (v3PositionIncrease) lpAdd = true;
    if (v3PositionDecrease) lpRemove = true;
    if (v3Collect) lpRemove = true;

    r.lpMintOrBurnSeen = !mintedIn.empty() || !burnedOut.empty();
    r.lpV3EventSeen = v3PositionIncrease || v3PositionDecrease || v3Collect;
    for (auto& tok : tokenOrder) {
        if (outCounterparties.count(tok) > 0 || inCounterparties.count(tok) > 0) { r.lpPoolIdentitySeen = true; break; }
    }

    std::string bestNonBaseTok; cpp_int bestNonBaseAbs = -1; cpp_int bestNonBaseNet = 0;
    bool bestNonBaseCoherent = false, bestNonBaseFromPool = false;
    bool hasBaseIn=false, hasBaseOut=false;

    for (auto& tok : tokenOrder) {
        if (!isBaseAsset(tok)) continue;
        cpp_int net = netFlow[tok];
        if (net > 0) hasBaseIn = true;
        if (net < 0) hasBaseOut = true;
    }

    for (auto& tok : tokenOrder) {
        if (isBaseAsset(tok)) continue;
        cpp_int net = netFlow[tok];
        if (net == 0) continue;
        cpp_int absNet = net >= 0 ? net : -net;
        bool coherent = (net > 0 && (hasBaseOut || nativeSwapSignal)) || (net < 0 && (hasBaseIn || nativeInflowSignal));
        bool fromPool = (net > 0)
            ? (inSources.count(tok) && [&]() { for (auto& s : inSources[tok]) if (allSwapPoolAddrs.count(s)) return true; return false; }())
            : (outDestinations.count(tok) && [&]() { for (auto& d : outDestinations[tok]) if (allSwapPoolAddrs.count(d)) return true; return false; }());
        bool better = bestNonBaseTok.empty() ||
                      (coherent && !bestNonBaseCoherent) ||
                      (coherent == bestNonBaseCoherent && fromPool && !bestNonBaseFromPool) ||
                      (coherent == bestNonBaseCoherent && fromPool == bestNonBaseFromPool && absNet > bestNonBaseAbs);
        if (better) { bestNonBaseAbs = absNet; bestNonBaseTok = tok; bestNonBaseNet = net; bestNonBaseCoherent = coherent; bestNonBaseFromPool = fromPool; }
    }

    r.isSwap = (
        twoSidedFlow ||
        (!bestNonBaseTok.empty() && (hasBaseIn || hasBaseOut || nativeSwapSignal || nativeInflowSignal)) ||
        (hasBaseIn && hasBaseOut)
    );

    if (lpAdd || lpRemove) {
        r.isSwap = false;
        r.venue = lpAdd ? "Add Liquidity" : "Remove Liquidity";
    }

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

    if (!r.isSwap && !lpAdd && !lpRemove && !r.tokenAddr.empty() && r.tokenAddr != NATIVE_BNB_MARKER &&
        !isBaseAsset(r.tokenAddr) && (nativeSwapSignal || nativeInflowSignal)) {
        r.isSwap = true;
        if (r.counterAddr.empty()) {
            if (r.isBuy && nativeOutflow && nativeSwapSignal) { r.counterAddr = NATIVE_BNB_MARKER; r.counterAmount = nativeOut; }
            else if (!r.isBuy && nativeInflowSignal) { r.counterAddr = NATIVE_BNB_MARKER; r.counterAmount = nativeIn; }
        }
    }

    if (dc.decoded && !r.isSwap && r.venue.empty() && dc.intent != CallIntent::NONE) {
        if ((dc.intent == CallIntent::BUY || dc.intent == CallIntent::TOKEN_SWAP) && !dc.tokenOut.empty()) {
            auto it = netFlow.find(dc.tokenOut);
            if (it != netFlow.end() && it->second > 0) {
                r.isSwap = true; r.isBuy = true; r.tokenAddr = dc.tokenOut; r.rawAmount = it->second;
                r.intentGuided = true;
                if (r.counterAddr.empty()) {
                    auto cit = netFlow.find(dc.tokenIn);
                    if (!dc.tokenIn.empty() && cit != netFlow.end() && cit->second < 0) { r.counterAddr = dc.tokenIn; r.counterAmount = -cit->second; }
                    else if (dc.nativeIn && nativeOut > 0) { r.counterAddr = NATIVE_BNB_MARKER; r.counterAmount = nativeOut; }
                }
            }
        } else if (dc.intent == CallIntent::SELL && !dc.tokenIn.empty()) {
            auto it = netFlow.find(dc.tokenIn);
            if (it != netFlow.end() && it->second < 0) {
                r.isSwap = true; r.isBuy = false; r.tokenAddr = dc.tokenIn; r.rawAmount = -it->second;
                r.intentGuided = true;
                if (r.counterAddr.empty()) {
                    auto cit = netFlow.find(dc.tokenOut);
                    if (!dc.tokenOut.empty() && cit != netFlow.end() && cit->second > 0) { r.counterAddr = dc.tokenOut; r.counterAmount = cit->second; }
                    else if (dc.nativeOut && wbnbUnwrapped > 0) { r.counterAddr = NATIVE_BNB_MARKER; r.counterAmount = wbnbUnwrapped; }
                }
            }
        }
    }
    if (r.isSwap && r.venue.empty() && dc.decoded && !dc.protocol.empty())
        r.venue = dc.viaMulticall ? (dc.protocol + std::string(" (multicall)")) : (dc.protocol + std::string(" call"));
    r.calldataDecoded = dc.decoded;

    int tokenDec = (r.tokenAddr == NATIVE_BNB_MARKER) ? 18 : getDecimals(r.tokenAddr);
    uint64_t tokenPrice = (r.tokenAddr == NATIVE_BNB_MARKER) ? getPriceNanos(g_chain.wrappedNative) : getPriceNanos(r.tokenAddr);
    r.usdNanos = calcUsdNanos(r.rawAmount, tokenDec, tokenPrice);
    r.hasSwapEvent = hasSwap; r.isUniversalRouter = urCmds.present;
    r.isGenericMulticall = isGenericMulticall; r.hasPermit2Signal = urCmds.hasPermit2;
    r.dexActivityDetected = hasSwap || routerCall;
    if (!r.isSwap && r.venue.empty() && r.dexActivityDetected) {
        if (!twoSidedFlow) r.unknownReason = "NO_COUNTER_FLOW";
        else if (hasSwap && !routerCall) r.unknownReason = "UNKNOWN_ROUTER";
        else r.unknownReason = "OTHER";
    }
    return r;
}
