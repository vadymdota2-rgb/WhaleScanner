#include "tx_analyzer.h"
#include <algorithm>
#include <sstream>
#include <functional>
#include <cmath>

// ============================================================================
// HEX PARSING
// ============================================================================

static std::optional<cpp_int> parseHexIntOpt(std::string_view sv, size_t maxChars = 64) {
    if (sv.size() >= 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X')) {
        sv.remove_prefix(2);
    }
    if (sv.empty() || sv.size() > maxChars) return std::nullopt;
    cpp_int r = 0;
    for (char c : sv) {
        int nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else return std::nullopt;
        r = (r << 4) | nibble;
    }
    return r;
}

cpp_int parseUint256(const std::string& h) {
    auto opt = parseHexIntOpt(h, 64);
    return opt.value_or(cpp_int(0));
}

cpp_int hexToCppInt(const std::string& h) {
    auto opt = parseHexIntOpt(h, 64);
    return opt.value_or(cpp_int(0));
}

static uint64_t hexWordToU64(const std::string& hexNo0x, size_t hexPos) {
    if (hexNo0x.size() < hexPos + 64) return 0;
    uint64_t v = 0;
    for (size_t i = hexPos + 32; i < hexPos + 64; i++) {
        char c = hexNo0x[i];
        int nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (uint64_t)nibble;
    }
    return v;
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// ============================================================================
// FORMATTING
// ============================================================================

std::string formatAmount(const cpp_int& raw, int dec) {
    if (raw == 0) return "0.00";
    cpp_int d = 1;
    for (int i = 0; i < dec; i++) d *= 10;
    std::string ip = (raw / d).convert_to<std::string>();
    std::string fp = (raw % d).convert_to<std::string>();
    while ((int)fp.length() < dec) fp = "0" + fp;
    if ((int)fp.length() > 2) fp = fp.substr(0, 2);
    return ip + "." + fp;
}

cpp_int calcUsdNanos(const cpp_int& raw, int dec, uint64_t pn) {
    if (!pn) return 0;
    cpp_int d = 1;
    for (int i = 0; i < dec; i++) d *= 10;
    return (raw * pn) / d;
}

std::string formatUsd(const cpp_int& n) {
    std::string s = n.convert_to<std::string>();
    while (s.length() < 10) s = "0" + s;
    std::string dl = s.substr(0, s.length() - 9);
    std::string ct = s.substr(s.length() - 9, 2);
    if (dl.empty()) dl = "0";
    return "$" + dl + "." + ct;
}

cpp_int calcUnitPriceNanos(const cpp_int& usdNanos, const cpp_int& rawAmount, int dec) {
    if (rawAmount <= 0) return 0;
    cpp_int d = 1;
    for (int i = 0; i < dec; i++) d *= 10;
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

std::string TxResultV2::typeString() const {
    switch (type) {
        case TxType::SWAP: return "SWAP";
        case TxType::TRANSFER: return "TRANSFER";
        case TxType::LP_ADD: return "LP_ADD";
        case TxType::LP_REMOVE: return "LP_REMOVE";
        case TxType::WRAP: return "WRAP";
        case TxType::UNWRAP: return "UNWRAP";
        case TxType::BRIDGE: return "BRIDGE";
        case TxType::NFT_TRADE: return "NFT_TRADE";
        case TxType::STAKE: return "STAKE";
        case TxType::UNSTAKE: return "UNSTAKE";
        case TxType::APPROVE: return "APPROVE";
        case TxType::REVOKE: return "REVOKE";
        case TxType::FAILED: return "FAILED";
        case TxType::MINT: return "MINT";
        case TxType::BURN: return "BURN";
        case TxType::MULTICALL_MIXED: return "MULTICALL_MIXED";
        case TxType::GAS_REFUND: return "GAS_REFUND";
        case TxType::CONTRACT_CALL: return "CONTRACT_CALL";
        default: return "UNKNOWN";
    }
}

std::string TxResultV2::confidenceString() const {
    switch (confidence) {
        case ConfidenceLevel::CERTAIN: return "CERTAIN";
        case ConfidenceLevel::HIGH: return "HIGH";
        case ConfidenceLevel::MEDIUM: return "MEDIUM";
        case ConfidenceLevel::LOW: return "LOW";
        default: return "GUESS";
    }
}

bool TxResultV2::hasTwoSidedFlow() const {
    bool hasIn = false, hasOut = false;
    for (const auto& f : allFlows) {
        if (f.to == f.from) continue;
        if (f.to == tokenAddr || f.amount > 0) hasIn = true;
        if (f.from == tokenAddr || f.amount > 0) hasOut = true;
    }
    return hasIn && hasOut;
}

// ============================================================================
// CHAIN CONTEXT
// ============================================================================

bool ChainContext::isBaseAsset(const std::string& a) const {
    return baseAssets.count(toLower(a)) > 0;
}
bool ChainContext::isStablecoin(const std::string& a) const {
    return stablecoins.count(toLower(a)) > 0;
}
std::string ChainContext::lookupRouterLabel(const std::string& addr) const {
    auto it = routers.find(toLower(addr));
    return it != routers.end() ? it->second : std::string();
}
bool ChainContext::isPermit2(const std::string& addr) const {
    return permit2Contracts.count(toLower(addr)) > 0;
}
bool ChainContext::isBridge(const std::string& addr) const {
    return bridgeContracts.count(toLower(addr)) > 0;
}
bool ChainContext::isNftMarketplace(const std::string& addr) const {
    return nftMarketplaces.count(toLower(addr)) > 0;
}
bool ChainContext::isStaking(const std::string& addr) const {
    return stakingContracts.count(toLower(addr)) > 0;
}
bool ChainContext::isEntryPoint(const std::string& addr) const {
    return entryPoints.count(toLower(addr)) > 0;
}
bool ChainContext::isIntentReactor(const std::string& addr) const {
    return intentReactors.count(toLower(addr)) > 0;
}


ChainContext makeBscContext() {
    ChainContext c;
    c.displayName = "BNB Smart Chain";
    c.explorerUrl = "https://bscscan.com";
    c.explorerName = "BscScan";
    c.nativeSymbol = "BNB";
    c.nativeMarker = "native:bnb";
    c.wrappedNative = "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c";
    c.stablecoins = {
        "0x55d398326f99059ff775485246999027b3197955",
        "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d",
        "0xe9e7cea3dedca5984780bafc599bd69add087d56"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(c.wrappedNative);
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
    c.permit2Contracts = {
        "0x31c2f6fcff4f8759b3bd5bf0e1084a055615c102"
    };
    c.bridgeContracts = {
        "0x47a1437d6714f2544ebc7d4e0e95c8b9248f1e2b",
        "0x5427fefa711eff984124bfbb1ab6fbf5e3da1820"
    };
    c.nftMarketplaces = {};
    c.stakingContracts = {
        "0x73feaa1ee314f8c655e354234017be2193c9e24e"
    };
    return c;
}

ChainContext makeEthereumContext() {
    ChainContext c;
    const std::string WETH = "0xc02aaa39b223fe8d0a0e5c4f27ead9083c756cc2";
    c.displayName = "Ethereum";
    c.explorerUrl = "https://etherscan.io";
    c.explorerName = "Etherscan";
    c.nativeSymbol = "ETH";
    c.nativeMarker = "native:eth";
    c.wrappedNative = WETH;
    c.stablecoins = {
        "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
        "0xdac17f958d2ee523a2206206994597c13d831ec7",
        "0x6b175474e89094c44da98b954eedeac495271d0f"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(WETH);
    c.routers = {
        {"0x7a250d5630b4cf539739df2c5dacb4c659f2488d", "Uniswap V2"},
        {"0xe592427a0aece92de3edee1f18e0157c05861564", "Uniswap V3"},
        {"0xef1c6e67703c7bd7107eed8303fbe6ec2554bf6b", "Uniswap (Universal Router)"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)"},
        {"0x66a9893cc07d91d95644aedd05d03f95e1dba8af", "Uniswap V4 (Universal Router)"},
    };
    c.permit2Contracts = {
        "0x000000000022d473030f116ddee9f6b43ac78ba3"
    };
    c.bridgeContracts = {
        "0x66a71dcef29a0ffbdbe3c6a460a3b5bc225cd675",
        "0x3c2269811836af69497e5f486a85d7316753cf62",
        "0x8731d54e9d02c286767d56ac03e8037c07e01e98",
        "0x5c7bcd6e7de5423a257d81b442095a1a6ced35c5",
        "0x5427fefa711eff984124bfbb1ab6fbf5e3da1820"
    };
    c.nftMarketplaces = {
        {"0x00000000000000adc04c56bf30ac9d3c0aaf14dc", "Seaport 1.1"},
        {"0x00000000000001ad428e4906ae43d8f9852d0dd6", "Seaport 1.5"},
        {"0x39da41747a83aee658334415666f3ef92dd0d541", "Blur"},
        {"0x0000000000e9c0809c14f4dc1e48e9e99f3fb1e2", "LooksRare"},
        {"0x59728544b08ab483533076417fbbb2fd0b17ce2a", "LooksRare 2"}
    };
    c.stakingContracts = {};
    c.entryPoints = {
        "0x5ff137d4b0fdcd49dca30c7cf57e578a026d2789",
        "0x0576a174d229e3cfa37253523e645a78a0c91b57"
    };
    c.intentReactors = {
        {"0x6000da47483062a0d734ba3dc7576ce882a8d072", "UniswapX"},
        {"0x9008d19f58aabd9ed0d60971565aa8510560ab41", "CoW Swap"}
    };
    return c;
}

ChainContext makeBaseContext() {
    ChainContext c;
    const std::string WETH = "0x4200000000000000000000000000000000000006";
    c.displayName = "Base";
    c.explorerUrl = "https://basescan.org";
    c.explorerName = "BaseScan";
    c.nativeSymbol = "ETH";
    c.nativeMarker = "native:eth";
    c.wrappedNative = WETH;
    c.stablecoins = {
        "0x833589fcd6edb6e08f4c7c32d4f71b54bda02913",
        "0xfde4c96c8593536e31f229ea8f37b2ada2699bb2",
        "0x50c5725949a6f0c72e6c4a641f24049a917db0cb"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(WETH);
    c.routers = {
        {"0x198ef79f1f515f02dfe9e3115ed9fc07183f02fc", "Uniswap (Universal Router)"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)"},
        {"0x6ff5693b99212da76ad316178a184ab56d299b43", "Uniswap V4 (Universal Router)"},
    };
    c.permit2Contracts = {
        "0x000000000022d473030f116ddee9f6b43ac78ba3"
    };
    return c;
}

ChainContext makeArbitrumContext() {
    ChainContext c;
    const std::string WETH = "0x82af49447d8a07e3bd95bd0d56f35241523fbab1";
    c.displayName = "Arbitrum One";
    c.explorerUrl = "https://arbiscan.io";
    c.explorerName = "Arbiscan";
    c.nativeSymbol = "ETH";
    c.nativeMarker = "native:eth";
    c.wrappedNative = WETH;
    c.stablecoins = {
        "0xaf88d065e77c8cc2239327c5edb3a432268e5831",
        "0xfd086bc7cd5c481dcc9c85ebe478a1c0b69fcbb9",
        "0xda10009cbd5d07dd0cecc66161fc93d7c9000da1"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(WETH);
    c.routers = {
        {"0x4c60051384bd2d3c01bfc845cf5f4b44bcbe9de5", "Uniswap (Universal Router)"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)"},
        {"0xe592427a0aece92de3edee1f18e0157c05861564", "Uniswap V3"},
        {"0x68b3465833fb72a70ecdf485e0e4c7bd8665fc45", "Uniswap V3 (Router 2)"},
    };
    c.permit2Contracts = {
        "0x000000000022d473030f116ddee9f6b43ac78ba3"
    };
    return c;
}


// ============================================================================
// PROTOCOL DETECTORS & LOG INDEXER
// ============================================================================

namespace {

// Event topic0 signatures
const std::string ERC20_TRANSFER = "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
const std::string ERC20_APPROVAL = "0x8c5be1e5ebec7d5bd14f71427d1e84f3dd0314c0f7b2291e5b200ac8c7c3b925";
const std::string V2_SWAP = "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822";
const std::string V3_SWAP_1 = "0xc42079f94a6350d7e6235f29174924f928cc2ac818eb64fed8004e115fbcca67";
const std::string V3_SWAP_2 = "0x19b47279256b2a23a1665c810c8d55a1758940ee09377d4f8d26497a3577dc83";
const std::string V4_SWAP = "0x40d6a5d3d3c3e9e8f7e6d5c4b3a2918e7f6d5c4b3a2918e7f6d5c4b3a2918e7f"; // placeholder
const std::string WNATIVE_DEPOSIT = "0xe1fffcc4923d04b559f4d29a8bfc6cda04eb5b0d3c460751c2402c5c5cc9109c";
const std::string WNATIVE_WITHDRAW = "0x7fcf532c15f0a6db0bd6d0e038bea71d30d808c7d98cb3bf7268a95bf5081b65";
const std::string V3_INC_LIQ = "0x3067048beee31b25b2f1681f88dac838c8bba36af25bfb2b7cf7473a5847e35f";
const std::string V3_DEC_LIQ = "0x26f6a048ee9138f2c0ce266f322cb99228e8d619ae2bff30c67f8dcf9d2377b4";
const std::string V3_COLLECT = "0x40d0efd1a53d60ecbf40971b9daf7dc90178c3aadc7aab1765632738fa8b8f01";
const std::string ERC4337_USER_OP = "0x49628fd1471006c1482da88028e9ce4dbb080b815c7b9e659bc9c71c6af32da";

struct URCommands {
    bool present = false;
    bool hasWrap = false, hasUnwrap = false;
    bool hasV2Swap = false, hasV3Swap = false, hasV4Swap = false;
    bool hasSweep = false, hasTransfer = false;
    bool hasPermit2 = false;
    bool hasPayPortion = false;
    int commandCount = 0;
};

bool isGenericMulticallSelector(const std::string& input) {
    return input.size() >= 10 && input[0] == '0' && input[1] == 'x' && input.substr(2, 8) == "ac9650d8";
}

URCommands parseExecuteCommands(const std::string& input) {
    URCommands out;
    if (input.size() < 10 || input[0] != '0' || input[1] != 'x') return out;
    std::string selector = input.substr(2, 8);
    if (selector != "3593564c" && selector != "24856bc3") return out;
    std::string hexNo0x = input.substr(2);
    if (hexNo0x.size() < 8 + 64) return out;
    uint64_t offsetCommands = hexWordToU64(hexNo0x, 8);
    size_t lenHexPos = 8 + offsetCommands * 2;
    if (offsetCommands > hexNo0x.size() || hexNo0x.size() < lenHexPos + 64) return out;
    uint64_t length = hexWordToU64(hexNo0x, lenHexPos);
    if (length == 0 || length > 256) return out;
    size_t dataHexPos = lenHexPos + 64;
    if (hexNo0x.size() < dataHexPos + length * 2) return out;
    out.present = true;
    out.commandCount = static_cast<int>(length);
    for (uint64_t i = 0; i < length; i++) {
        int b = (hexNibble(hexNo0x[dataHexPos + i * 2]) << 4) | hexNibble(hexNo0x[dataHexPos + i * 2 + 1]);
        int cmd = b & 0x1f;
        if (cmd == 0x0b) out.hasWrap = true;
        else if (cmd == 0x0c) out.hasUnwrap = true;
        else if (cmd == 0x08 || cmd == 0x09) out.hasV2Swap = true;
        else if (cmd == 0x00 || cmd == 0x01) out.hasV3Swap = true;
        else if (cmd == 0x10 || cmd == 0x11) out.hasV4Swap = true;
        else if (cmd == 0x04) out.hasSweep = true;
        else if (cmd == 0x05) out.hasTransfer = true;
        else if (cmd == 0x06) out.hasPayPortion = true;
        else if (cmd == 0x02 || cmd == 0x03 || cmd == 0x0a || cmd == 0x0d) out.hasPermit2 = true;
    }
    return out;
}

struct ProtocolSig {
    std::string selector;
    std::string name;
    TxType likelyType;
};

const std::vector<ProtocolSig> KNOWN_SELECTORS = {
    {"0x38ed1739", "swapExactTokensForTokens", TxType::SWAP},
    {"0x8803dbee", "swapTokensForExactTokens", TxType::SWAP},
    {"0x7ff36ab5", "swapExactETHForTokens", TxType::SWAP},
    {"0x18cbafe5", "swapExactTokensForETH", TxType::SWAP},
    {"0xb6f9de95", "swapExactTokensForTokensSupportingFeeOnTransferTokens", TxType::SWAP},
    {"0x791ac947", "swapExactTokensForETHSupportingFeeOnTransferTokens", TxType::SWAP},
    {"0xfb3bdb41", "swapETHForExactTokens", TxType::SWAP},
    {"0x414bf389", "exactInputSingle", TxType::SWAP},
    {"0xc04b8d59", "exactOutputSingle", TxType::SWAP},
    {"0xb858183f", "exactInput", TxType::SWAP},
    {"0x5023b4df", "exactOutput", TxType::SWAP},
    {"0x3593564c", "execute", TxType::SWAP},
    {"0x24856bc3", "execute", TxType::SWAP},
    {"0xac9650d8", "multicall", TxType::MULTICALL_MIXED},
    {"0xe8e33700", "addLiquidity", TxType::LP_ADD},
    {"0xf305d719", "addLiquidityETH", TxType::LP_ADD},
    {"0xbaa2abde", "removeLiquidity", TxType::LP_REMOVE},
    {"0x02751cec", "removeLiquidityETH", TxType::LP_REMOVE},
    {"0x88316456", "mint", TxType::LP_ADD},
    {"0x0c49ccbe", "decreaseLiquidity", TxType::LP_REMOVE},
    {"0xfc6f7865", "increaseLiquidity", TxType::LP_ADD},
    {"0x4f1eb3d8", "burn", TxType::LP_REMOVE},
    {"0xd0e30db0", "deposit", TxType::WRAP},
    {"0x2e1a7d4d", "withdraw", TxType::UNWRAP},
    {"0x36c78516", "permitTransferFrom", TxType::SWAP},
    {"0x30f28d7a", "permitWitnessTransferFrom", TxType::SWAP},
    {"0x8c9d5c1e", "swapAndBridge", TxType::BRIDGE},
    {"0x1a3d5e5f", "bridge", TxType::BRIDGE},
    {"0x2cab2c2c", "sendFrom", TxType::BRIDGE},
    {"0x6dbd9e1a", "swap", TxType::BRIDGE},
    {"0xe2bbb158", "deposit", TxType::STAKE},
    {"0x441a3e70", "withdraw", TxType::UNSTAKE},
    {"0x6e553f65", "enterStaking", TxType::STAKE},
    {"0x2e1a7d4d", "leaveStaking", TxType::UNSTAKE},
    {"0xfb0f3ee1", "fulfillBasicOrder", TxType::NFT_TRADE},
    {"0x87201b41", "fulfillOrder", TxType::NFT_TRADE},
    {"0xe7acab24", "fulfillAdvancedOrder", TxType::NFT_TRADE},
    {"0x095ea7b3", "approve", TxType::APPROVE},
    {"0xa22cb465", "setApprovalForAll", TxType::APPROVE},
};

TxType detectTypeBySelector(const std::string& input) {
    if (input.size() < 10) return TxType::UNKNOWN;
    std::string sel = input.substr(0, 10);
    for (const auto& ps : KNOWN_SELECTORS) {
        if (ps.selector == sel) return ps.likelyType;
    }
    return TxType::UNKNOWN;
}

std::string detectVenueBySelector(const std::string& input) {
    if (input.size() < 10) return "";
    std::string sel = input.substr(0, 10);
    static const std::map<std::string, std::string> MAP = {
        {"0x38ed1739", "Uniswap V2"}, {"0x8803dbee", "Uniswap V2"},
        {"0x7ff36ab5", "Uniswap V2"}, {"0x18cbafe5", "Uniswap V2"},
        {"0x414bf389", "Uniswap V3"}, {"0xc04b8d59", "Uniswap V3"},
        {"0xb858183f", "Uniswap V3"}, {"0x5023b4df", "Uniswap V3"},
        {"0x3593564c", "Universal Router"}, {"0x24856bc3", "Universal Router"},
        {"0xe8e33700", "Uniswap V2"}, {"0xf305d719", "Uniswap V2"},
        {"0xbaa2abde", "Uniswap V2"}, {"0x02751cec", "Uniswap V2"},
        {"0x88316456", "Uniswap V3"}, {"0x0c49ccbe", "Uniswap V3"},
        {"0xfc6f7865", "Uniswap V3"}, {"0x4f1eb3d8", "Uniswap V3"},
    };
    auto it = MAP.find(sel);
    return it != MAP.end() ? it->second : "";
}

struct RawLog {
    std::string address;
    std::string topic0;
    std::vector<std::string> topics;
    std::string data;
    size_t logIndex = 0;
};

class LogIndexer {
public:
    std::vector<RawLog> logs;
    std::vector<RawLog> transfers;
    std::vector<RawLog> approvals;
    std::vector<RawLog> swaps;
    std::vector<RawLog> wrapUnwrap;
    std::vector<RawLog> v3LpEvents;
    std::vector<RawLog> permit2Events;
    std::vector<RawLog> nftEvents;
    std::vector<RawLog> bridgeEvents;
    std::vector<RawLog> stakingEvents;
    std::vector<RawLog> accountAbstractionEvents;
    std::vector<RawLog> intentEvents;
    std::vector<RawLog> other;

    std::set<std::string> touchedTokens;
    std::set<std::string> poolAddresses;
    std::set<std::string> routerAddresses;
    std::map<std::string, std::vector<std::string>> approvalMap; // token -> [spender]

    void index(const json& receiptLogs, const ChainContext& chain) {
        if (!receiptLogs.is_array()) return;
        size_t idx = 0;
        for (auto& l : receiptLogs) {
            if (!l.is_object() || !l.contains("topics") || !l["topics"].is_array() || l["topics"].empty()) continue;
            if (!l["topics"][0].is_string()) continue;
            RawLog rl;
            rl.topic0 = l["topics"][0].get<std::string>();
            rl.logIndex = idx++;
            if (l.contains("address") && l["address"].is_string())
                rl.address = toLower(l["address"].get<std::string>());
            for (auto& t : l["topics"]) {
                if (t.is_string()) rl.topics.push_back(t.get<std::string>());
            }
            if (l.contains("data") && l["data"].is_string())
                rl.data = l["data"].get<std::string>();
            logs.push_back(rl);

            if (rl.topic0 == ERC20_TRANSFER) {
                transfers.push_back(rl);
                touchedTokens.insert(rl.address);
            } else if (rl.topic0 == ERC20_APPROVAL) {
                approvals.push_back(rl);
                if (rl.topics.size() >= 3) {
                    std::string spender = extractAddrFromTopic(rl.topics[2]);
                    approvalMap[rl.address].push_back(spender);
                }
            } else if (rl.topic0 == V2_SWAP || rl.topic0 == V3_SWAP_1 || rl.topic0 == V3_SWAP_2) {
                swaps.push_back(rl);
                poolAddresses.insert(rl.address);
                if (!chain.lookupRouterLabel(rl.address).empty())
                    routerAddresses.insert(rl.address);
            } else if (rl.address == chain.wrappedNative &&
                      (rl.topic0 == WNATIVE_DEPOSIT || rl.topic0 == WNATIVE_WITHDRAW)) {
                wrapUnwrap.push_back(rl);
            } else if (rl.topic0 == V3_INC_LIQ || rl.topic0 == V3_DEC_LIQ || rl.topic0 == V3_COLLECT) {
                v3LpEvents.push_back(rl);
            } else if (chain.isPermit2(rl.address)) {
                permit2Events.push_back(rl);
            } else if (chain.isNftMarketplace(rl.address)) {
                nftEvents.push_back(rl);
            } else if (chain.isBridge(rl.address)) {
                bridgeEvents.push_back(rl);
            } else if (chain.isStaking(rl.address)) {
                stakingEvents.push_back(rl);
            } else if (rl.topic0 == ERC4337_USER_OP) {
                accountAbstractionEvents.push_back(rl);
            } else if (chain.isIntentReactor(rl.address)) {
                intentEvents.push_back(rl);
            } else {
                other.push_back(rl);
            }
        }
    }

private:
    static std::string extractAddrFromTopic(const std::string& topic) {
        if (topic.length() < 66) return "";
        return "0x" + toLower(topic.substr(26));
    }
};

} // anonymous namespace


// ============================================================================
// FLOW RECONSTRUCTION
// ============================================================================

namespace {

std::string extractAddrFromTopic(const std::string& topic) {
    if (topic.length() < 66) return "";
    return "0x" + toLower(topic.substr(26));
}

struct FlowState {
    std::map<std::string, cpp_int> netFlow;
    std::map<std::string, std::vector<std::string>> inSources;
    std::map<std::string, std::vector<std::string>> outDests;
    std::set<std::string> mintedIn;
    std::set<std::string> burnedOut;
    std::set<std::string> allTokens;
    std::set<std::string> permit2SpentTokens;  // tokens spent via Permit2
    cpp_int nativeWrap = 0;
    cpp_int nativeUnwrap = 0;
    cpp_int nativeValueOut = 0;
    cpp_int nativeValueIn = 0;
    bool anyTransferForWallet = false;

    void addIn(const std::string& token, const cpp_int& amt, const std::string& from) {
        netFlow[token] += amt;
        inSources[token].push_back(from);
        allTokens.insert(token);
        if (from == "0x0000000000000000000000000000000000000000") mintedIn.insert(token);
    }
    void addOut(const std::string& token, const cpp_int& amt, const std::string& to) {
        netFlow[token] -= amt;
        outDests[token].push_back(to);
        allTokens.insert(token);
        if (to == "0x0000000000000000000000000000000000000000" ||
            to == "0x000000000000000000000000000000000000dead")
            burnedOut.insert(token);
    }
};

FlowState reconstructFlows(const LogIndexer& idx, const json& tx,
                           const std::string& wallet, const ChainContext& chain) {
    FlowState fs;

    // 1. ERC20 transfers
    for (const auto& rl : idx.transfers) {
        if (rl.topics.size() != 3) continue;
        std::string fr = extractAddrFromTopic(rl.topics[1]);
        std::string to = extractAddrFromTopic(rl.topics[2]);
        auto amtOpt = parseHexIntOpt(rl.data);
        if (!amtOpt || *amtOpt == 0) continue;
        if (fr != wallet && to != wallet) continue;
        fs.anyTransferForWallet = true;
        if (to == wallet) fs.addIn(rl.address, *amtOpt, fr);
        if (fr == wallet) fs.addOut(rl.address, *amtOpt, to);
    }

    // 2. Detect Permit2 flows: if wallet approved Permit2 and token moved to router
    for (const auto& [token, spenders] : idx.approvalMap) {
        for (const auto& spender : spenders) {
            if (chain.isPermit2(spender)) {
                // Check if this token was transferred from wallet without direct approval to destination
                if (fs.outDests.count(token)) {
                    for (const auto& dst : fs.outDests.at(token)) {
                        if (!chain.lookupRouterLabel(dst).empty() || chain.isPermit2(dst)) {
                            fs.permit2SpentTokens.insert(token);
                        }
                    }
                }
            }
        }
    }

    // 3. Wrap/Unwrap
    for (const auto& rl : idx.wrapUnwrap) {
        auto amtOpt = parseHexIntOpt(rl.data);
        if (!amtOpt) continue;
        if (rl.topic0 == WNATIVE_DEPOSIT) fs.nativeWrap += *amtOpt;
        else fs.nativeUnwrap += *amtOpt;
    }

    // 4. Native value from tx
    if (tx.is_object() && tx.contains("from") && tx["from"].is_string() &&
        toLower(tx["from"].get<std::string>()) == wallet) {
        if (tx.contains("value") && tx["value"].is_string()) {
            auto valOpt = parseHexIntOpt(tx["value"].get<std::string>());
            if (valOpt) fs.nativeValueOut = *valOpt;
        }
    }
    if (tx.is_object() && tx.contains("to") && !tx["to"].is_null() && tx["to"].is_string() &&
        toLower(tx["to"].get<std::string>()) == wallet) {
        if (tx.contains("value") && tx["value"].is_string()) {
            auto valOpt = parseHexIntOpt(tx["value"].get<std::string>());
            if (valOpt) fs.nativeValueIn = *valOpt;
        }
    }

    return fs;
}

void enrichFromTrace(FlowState& fs, const json& trace, const std::string& wallet,
                     const ChainContext& chain) {
    if (!trace.is_object()) return;
    std::function<void(const json&)> walk = [&](const json& node) {
        if (!node.is_object()) return;
        if (node.contains("from") && node["from"].is_string() &&
            node.contains("to") && node["to"].is_string() &&
            node.contains("value") && node["value"].is_string()) {
            std::string fr = toLower(node["from"].get<std::string>());
            std::string to = toLower(node["to"].get<std::string>());
            auto valOpt = parseHexIntOpt(node["value"].get<std::string>());
            if (valOpt && *valOpt > 0) {
                if (fr == wallet && to != wallet) fs.nativeValueOut += *valOpt;
                else if (to == wallet && fr != wallet) fs.addIn(chain.nativeMarker, *valOpt, fr);
            }
        }
        if (node.contains("calls") && node["calls"].is_array()) {
            for (auto& c : node["calls"]) walk(c);
        }
    };
    walk(trace);
}

} // anonymous namespace


// ============================================================================
// CLASSIFICATION ENGINE
// ============================================================================

namespace {

struct Classification {
    TxType type = TxType::UNKNOWN;
    std::string venue;
    std::string primaryToken;
    std::string counterToken;
    cpp_int primaryAmount;
    cpp_int counterAmount;
    bool isBuy = false;
    ConfidenceLevel confidence = ConfidenceLevel::GUESS;
    std::string reason;
    bool hasWrapBeforeSwap = false;
    bool hasSweepAfterSwap = false;
    bool isMultiHop = false;
    bool isFeeOnTransfer = false;
    bool isIntentBased = false;
    bool isAccountAbstraction = false;
};

Classification classifyTransaction(const AnalysisInput& in, const LogIndexer& idx,
                                   const FlowState& fs, const URCommands& ur,
                                   const std::string& wallet) {
    Classification cl;
    const auto& chain = in.chain;

    // --- 1. Revert check ---
    if (in.receipt.is_object() && in.receipt.contains("status")) {
        auto st = in.receipt["status"];
        if (st.is_string() && st.get<std::string>() == "0x0") {
            cl.type = TxType::FAILED;
            cl.confidence = ConfidenceLevel::CERTAIN;
            return cl;
        }
    }

    // --- 2. Tx metadata ---
    std::string txTo, txFrom, txInput;
    bool walletIsSender = false;
    if (in.tx.is_object()) {
        if (in.tx.contains("to") && !in.tx["to"].is_null() && in.tx["to"].is_string())
            txTo = toLower(in.tx["to"].get<std::string>());
        if (in.tx.contains("from") && in.tx["from"].is_string()) {
            txFrom = toLower(in.tx["from"].get<std::string>());
            walletIsSender = (txFrom == wallet);
        }
        if (in.tx.contains("input") && in.tx["input"].is_string())
            txInput = in.tx["input"].get<std::string>();
    }

    // --- 3. ERC-4337 detection ---
    if (!idx.accountAbstractionEvents.empty() || chain.isEntryPoint(txTo)) {
        cl.isAccountAbstraction = true;
        // Extract real sender from UserOperationEvent topics
        for (const auto& rl : idx.accountAbstractionEvents) {
            if (rl.topics.size() >= 3) {
                std::string sender = extractAddrFromTopic(rl.topics[2]);
                if (!sender.empty()) {
                    // cl.realSender = sender; // would need field in Classification
                }
            }
        }
    }

    // --- 4. Intent-based detection ---
    if (!idx.intentEvents.empty() || chain.isIntentReactor(txTo)) {
        cl.isIntentBased = true;
    }

    // --- 5. Venue detection ---
    cl.venue = chain.lookupRouterLabel(txTo);
    if (cl.venue.empty() && !idx.poolAddresses.empty()) {
        for (const auto& pool : idx.poolAddresses) {
            cl.venue = chain.lookupRouterLabel(pool);
            if (!cl.venue.empty()) break;
        }
    }
    if (cl.venue.empty() && ur.present) {
        if (ur.hasV4Swap) cl.venue = "Universal Router (V4)";
        else if (ur.hasV3Swap) cl.venue = "Universal Router (V3)";
        else if (ur.hasV2Swap) cl.venue = "Universal Router (V2)";
        else cl.venue = "Universal Router";
    }
    if (cl.venue.empty()) cl.venue = detectVenueBySelector(txInput);
    if (chain.isNftMarketplace(txTo)) cl.venue = "NFT Marketplace";
    if (chain.isBridge(txTo)) cl.venue = "Bridge";
    if (chain.isIntentReactor(txTo)) cl.venue = "Intent-Based Swap";

    // Multi-hop detection
    if (idx.swaps.size() > 1) {
        cl.isMultiHop = true;
        if (cl.venue.empty()) cl.venue = "Multi-Hop Aggregator";
    }

    // --- 6. Wrap/Unwrap (highest priority) ---
    bool wrapOnly = !fs.anyTransferForWallet && walletIsSender && (fs.nativeWrap > 0 || fs.nativeUnwrap > 0);
    if (wrapOnly) {
        cl.type = fs.nativeWrap > 0 ? TxType::WRAP : TxType::UNWRAP;
        cl.primaryToken = chain.wrappedNative;
        cl.primaryAmount = fs.nativeWrap > 0 ? fs.nativeWrap : fs.nativeUnwrap;
        cl.isBuy = fs.nativeWrap > 0;
        cl.confidence = ConfidenceLevel::HIGH;
        return cl;
    }

    // --- 7. Wrap + Swap combo ---
    if ((fs.nativeWrap > 0 || fs.nativeUnwrap > 0) && !idx.swaps.empty()) {
        cl.hasWrapBeforeSwap = fs.nativeWrap > 0;
    }

    // --- 8. NFT Trade ---
    if (!idx.nftEvents.empty() || chain.isNftMarketplace(txTo)) {
        cl.type = TxType::NFT_TRADE;
        cl.confidence = ConfidenceLevel::HIGH;
        for (const auto& [tok, net] : fs.netFlow) {
            if (net != 0) {
                cl.primaryToken = tok;
                cl.primaryAmount = net >= 0 ? net : -net;
                cl.isBuy = net > 0;
                break;
            }
        }
        return cl;
    }

    // --- 9. Bridge ---
    if (chain.isBridge(txTo) || !idx.bridgeEvents.empty()) {
        cl.type = TxType::BRIDGE;
        cl.confidence = ConfidenceLevel::HIGH;
        for (const auto& [tok, net] : fs.netFlow) {
            if (net != 0) {
                cl.primaryToken = tok;
                cl.primaryAmount = net >= 0 ? net : -net;
                cl.isBuy = net > 0;
                break;
            }
        }
        return cl;
    }

    // --- 10. Staking ---
    if (chain.isStaking(txTo) || !idx.stakingEvents.empty()) {
        bool out = false, inF = false;
        for (const auto& [tok, net] : fs.netFlow) {
            if (net < 0) out = true;
            if (net > 0) inF = true;
        }
        cl.type = (out && !inF) ? TxType::STAKE : TxType::UNSTAKE;
        cl.confidence = ConfidenceLevel::MEDIUM;
        for (const auto& [tok, net] : fs.netFlow) {
            if (net != 0) {
                cl.primaryToken = tok;
                cl.primaryAmount = net >= 0 ? net : -net;
                cl.isBuy = net > 0;
                break;
            }
        }
        return cl;
    }

    // --- 11. LP detection ---
    bool hasV3Lp = !idx.v3LpEvents.empty();
    bool hasMintBurn = !fs.mintedIn.empty() || !fs.burnedOut.empty();
    bool sentBase = false, sentNonBase = false, gotBase = false, gotNonBase = false;
    for (const auto& [tok, net] : fs.netFlow) {
        if (chain.isBaseAsset(tok)) {
            if (net > 0) gotBase = true;
            if (net < 0) sentBase = true;
        } else {
            if (net > 0) gotNonBase = true;
            if (net < 0) sentNonBase = true;
        }
    }
    bool lpAdd = false, lpRemove = false;
    if (hasV3Lp) {
        for (const auto& rl : idx.v3LpEvents) {
            if (rl.topic0 == V3_INC_LIQ) lpAdd = true;
            if (rl.topic0 == V3_DEC_LIQ || rl.topic0 == V3_COLLECT) lpRemove = true;
        }
    }
    if (!lpAdd && !lpRemove) {
        for (const auto& tok : fs.mintedIn) {
            if (sentBase && sentNonBase) { lpAdd = true; break; }
        }
        for (const auto& tok : fs.burnedOut) {
            if (gotBase && gotNonBase) { lpRemove = true; break; }
        }
    }
    if (!lpAdd && !lpRemove) {
        for (const auto& [tok, net] : fs.netFlow) {
            if (idx.poolAddresses.count(tok) > 0) {
                if (net > 0 && sentBase && sentNonBase) lpAdd = true;
                if (net < 0 && gotBase && gotNonBase) lpRemove = true;
            }
        }
    }
    if (lpAdd || lpRemove) {
        cl.type = lpAdd ? TxType::LP_ADD : TxType::LP_REMOVE;
        cl.confidence = (hasV3Lp || hasMintBurn) ? ConfidenceLevel::HIGH : ConfidenceLevel::MEDIUM;
        std::string bestTok;
        cpp_int bestAbs = -1;
        for (const auto& [tok, net] : fs.netFlow) {
            if (chain.isBaseAsset(tok)) continue;
            cpp_int a = net >= 0 ? net : -net;
            if (a > bestAbs) { bestAbs = a; bestTok = tok; }
        }
        if (bestTok.empty()) {
            for (const auto& [tok, net] : fs.netFlow) {
                cpp_int a = net >= 0 ? net : -net;
                if (a > bestAbs) { bestAbs = a; bestTok = tok; }
            }
        }
        if (!bestTok.empty()) {
            cl.primaryToken = bestTok;
            cl.primaryAmount = bestAbs;
            cl.isBuy = fs.netFlow.at(bestTok) > 0;
        }
        return cl;
    }

    // --- 12. Approval-only ---
    if (fs.netFlow.empty() && !idx.approvals.empty() && fs.nativeValueOut == 0 && fs.nativeValueIn == 0) {
        cl.type = TxType::APPROVE;
        cl.confidence = ConfidenceLevel::HIGH;
        return cl;
    }

    // --- 13. SWAP vs TRANSFER classification ---
    bool hasSwapEvent = !idx.swaps.empty();
    bool routerCall = !cl.venue.empty() || ur.present || isGenericMulticallSelector(txInput) || ur.hasPermit2;
    bool nativeSwapSignal = (fs.nativeValueOut > 0) && (routerCall || hasSwapEvent || fs.nativeWrap > 0);
    bool nativeInflowSignal = fs.nativeUnwrap > 0 || fs.nativeValueIn > 0;
    bool anyIn = false, anyOut = false;
    for (const auto& [tok, net] : fs.netFlow) {
        if (net > 0) anyIn = true;
        if (net < 0) anyOut = true;
    }
    bool twoSided = anyIn && anyOut;
    bool hasBaseIn = false, hasBaseOut = false;
    for (const auto& [tok, net] : fs.netFlow) {
        if (!chain.isBaseAsset(tok)) continue;
        if (net > 0) hasBaseIn = true;
        if (net < 0) hasBaseOut = true;
    }

    bool isSwap = twoSided || hasSwapEvent || (routerCall && anyIn && anyOut) ||
                  (hasBaseIn && hasBaseOut) || (nativeSwapSignal && anyIn) || (nativeInflowSignal && anyOut);

    // Fee-on-transfer detection: if swap event amount differs from transfer amount
    if (hasSwapEvent && !fs.netFlow.empty()) {
        for (const auto& rl : idx.swaps) {
            if (rl.data.size() < 2) continue;
            std::string d = rl.data.substr(2);
            if (d.length() == 256) {
                auto a0in = parseHexIntOpt("0x" + d.substr(0, 64));
                auto a1in = parseHexIntOpt("0x" + d.substr(64, 64));
                auto a0out = parseHexIntOpt("0x" + d.substr(128, 64));
                auto a1out = parseHexIntOpt("0x" + d.substr(192, 64));
                // Compare with actual transfers to detect fee
                // Simplified: if swap amounts don't match net flow significantly
                // This is a heuristic
            }
        }
    }

    if (!isSwap && !fs.anyTransferForWallet && (fs.nativeValueOut > 0 || fs.nativeValueIn > 0)) {
        cl.type = TxType::TRANSFER;
        cl.primaryToken = chain.nativeMarker;
        cl.primaryAmount = fs.nativeValueOut > 0 ? fs.nativeValueOut : fs.nativeValueIn;
        cl.isBuy = fs.nativeValueIn > 0;
        cl.confidence = ConfidenceLevel::HIGH;
        return cl;
    }

    if (!isSwap) {
        cl.type = TxType::TRANSFER;
        cl.confidence = fs.anyTransferForWallet ? ConfidenceLevel::HIGH : ConfidenceLevel::MEDIUM;
        std::string bestTok;
        cpp_int bestAbs = -1;
        for (const auto& [tok, net] : fs.netFlow) {
            cpp_int a = net >= 0 ? net : -net;
            if (a > bestAbs) { bestAbs = a; bestTok = tok; }
        }
        if (!bestTok.empty()) {
            cl.primaryToken = bestTok;
            cl.primaryAmount = bestAbs;
            cl.isBuy = fs.netFlow.at(bestTok) > 0;
        }
        return cl;
    }

    // --- 14. SWAP analysis ---
    cl.type = TxType::SWAP;

    // Confidence scoring
    int score = 0;
    if (hasSwapEvent) score += 2;
    if (routerCall) score += 1;
    if (twoSided) score += 1;
    if (cl.isMultiHop) score += 1;
    if (ur.present) score += 1;
    if (fs.permit2SpentTokens.size() > 0) score += 1;
    if (!cl.venue.empty() && cl.venue.find("Unknown") == std::string::npos) score += 1;
    if (score >= 5) cl.confidence = ConfidenceLevel::HIGH;
    else if (score >= 3) cl.confidence = ConfidenceLevel::MEDIUM;
    else if (score >= 1) cl.confidence = ConfidenceLevel::LOW;
    else cl.confidence = ConfidenceLevel::GUESS;

    // Best non-base token (the "traded" token)
    std::string bestNonBase;
    cpp_int bestNonBaseAbs = -1;
    cpp_int bestNonBaseNet = 0;
    bool bestCoherent = false, bestFromPool = false;
    for (const auto& [tok, net] : fs.netFlow) {
        if (chain.isBaseAsset(tok)) continue;
        if (net == 0) continue;
        cpp_int a = net >= 0 ? net : -net;
        bool coherent = (net > 0 && (hasBaseOut || nativeSwapSignal)) ||
                        (net < 0 && (hasBaseIn || nativeInflowSignal));
        bool fromPool = false;
        if (net > 0 && fs.inSources.count(tok)) {
            for (const auto& src : fs.inSources.at(tok))
                if (idx.poolAddresses.count(src)) { fromPool = true; break; }
        }
        if (net < 0 && fs.outDests.count(tok)) {
            for (const auto& dst : fs.outDests.at(tok))
                if (idx.poolAddresses.count(dst)) { fromPool = true; break; }
        }
        bool better = bestNonBase.empty() ||
                      (coherent && !bestCoherent) ||
                      (coherent == bestCoherent && fromPool && !bestFromPool) ||
                      (coherent == bestCoherent && fromPool == bestFromPool && a > bestNonBaseAbs);
        if (better) {
            bestNonBase = tok; bestNonBaseAbs = a; bestNonBaseNet = net;
            bestCoherent = coherent; bestFromPool = fromPool;
        }
    }

    if (!bestNonBase.empty()) {
        cl.primaryToken = bestNonBase;
        cl.primaryAmount = bestNonBaseAbs;
        cl.isBuy = bestNonBaseNet > 0;
    } else {
        std::string bestBase;
        cpp_int bestBaseAbs = -1;
        cpp_int bestBaseNet = 0;
        for (const auto& [tok, net] : fs.netFlow) {
            if (!chain.isBaseAsset(tok)) continue;
            cpp_int a = net >= 0 ? net : -net;
            if (a > bestBaseAbs) { bestBaseAbs = a; bestBase = tok; bestBaseNet = net; }
        }
        if (!bestBase.empty()) {
            cl.primaryToken = bestBase;
            cl.primaryAmount = bestBaseAbs;
            cl.isBuy = bestBaseNet > 0;
        } else if (fs.nativeValueOut > 0 && !fs.netFlow.empty()) {
            cl.primaryToken = fs.netFlow.begin()->first;
            cl.primaryAmount = fs.netFlow.begin()->second >= 0 ? fs.netFlow.begin()->second : -fs.netFlow.begin()->second;
            cl.isBuy = fs.netFlow.begin()->second > 0;
        }
    }

    // Counter token selection
    if (!cl.primaryToken.empty()) {
        std::string bestCounter;
        cpp_int bestCounterAbs = -1;
        for (const auto& [tok, net] : fs.netFlow) {
            if (tok == cl.primaryToken) continue;
            if (!chain.isBaseAsset(tok)) continue;
            bool wantsOut = cl.isBuy;
            if (wantsOut && net >= 0) continue;
            if (!wantsOut && net <= 0) continue;
            cpp_int a = net >= 0 ? net : -net;
            if (a > bestCounterAbs) { bestCounterAbs = a; bestCounter = tok; }
        }
        if (bestCounter.empty() && cl.isBuy && fs.nativeValueOut > 0 && nativeSwapSignal) {
            bestCounter = chain.nativeMarker; bestCounterAbs = fs.nativeValueOut;
        }
        if (bestCounter.empty() && !cl.isBuy && nativeInflowSignal) {
            bestCounter = chain.nativeMarker; bestCounterAbs = fs.nativeUnwrap > 0 ? fs.nativeUnwrap : fs.nativeValueIn;
        }
        if (bestCounter.empty()) {
            for (const auto& [tok, net] : fs.netFlow) {
                if (tok == cl.primaryToken) continue;
                bool wantsOut = cl.isBuy;
                if (wantsOut && net >= 0) continue;
                if (!wantsOut && net <= 0) continue;
                cpp_int a = net >= 0 ? net : -net;
                if (a > bestCounterAbs) { bestCounterAbs = a; bestCounter = tok; }
            }
        }
        if (!bestCounter.empty()) {
            cl.counterToken = bestCounter;
            cl.counterAmount = bestCounterAbs;
        }
    }

    return cl;
}

} // anonymous namespace


// ============================================================================
// MAIN ANALYZER
// ============================================================================

TxResultV2 analyzeTxV2(const AnalysisInput& in, const std::string& wallet) {
    TxResultV2 result;
    const auto& chain = in.chain;
    std::string wa = toLower(wallet);

    if (in.receipt.is_null() || !in.receipt.is_object() ||
        !in.receipt.contains("logs") || !in.receipt["logs"].is_array()) {
        return result;
    }
    result.valid = true;

    // Tx metadata
    std::string txTo, txFrom, txInput;
    bool walletIsSender = false;
    if (in.tx.is_object()) {
        if (in.tx.contains("to") && !in.tx["to"].is_null() && in.tx["to"].is_string())
            txTo = toLower(in.tx["to"].get<std::string>());
        if (in.tx.contains("from") && in.tx["from"].is_string()) {
            txFrom = toLower(in.tx["from"].get<std::string>());
            walletIsSender = (txFrom == wa);
        }
        if (in.tx.contains("input") && in.tx["input"].is_string())
            txInput = in.tx["input"].get<std::string>();
    }

    // Gas info
    if (in.receipt.contains("gasUsed") && in.receipt["gasUsed"].is_string())
        result.gasUsed = parseUint256(in.receipt["gasUsed"].get<std::string>());
    if (in.tx.contains("gasPrice") && in.tx["gasPrice"].is_string())
        result.gasPrice = parseUint256(in.tx["gasPrice"].get<std::string>());
    result.gasCostNative = result.gasUsed * result.gasPrice;

    // Parse UR commands
    URCommands ur;
    if (walletIsSender && !txInput.empty()) ur = parseExecuteCommands(txInput);
    bool isGenericMulticall = isGenericMulticallSelector(txInput);

    // Index logs
    LogIndexer idx;
    idx.index(in.receipt["logs"], chain);

    // Reconstruct flows
    FlowState fs = reconstructFlows(idx, in.tx, wa, chain);
    if (in.trace && in.trace->is_object()) enrichFromTrace(fs, *in.trace, wa, chain);

    // Classify
    Classification cl = classifyTransaction(in, idx, fs, ur, wa);

    // Build result
    result.type = cl.type;
    result.venue = cl.venue;
    result.tokenAddr = cl.primaryToken;
    result.counterAddr = cl.counterToken;
    result.rawAmount = cl.primaryAmount;
    result.counterAmount = cl.counterAmount;
    result.isBuy = cl.isBuy;
    result.confidence = cl.confidence;
    result.unknownReason = cl.reason;
    result.hasWrapBeforeSwap = cl.hasWrapBeforeSwap;
    result.hasSweepAfterSwap = cl.hasSweepAfterSwap;
    result.isMultiHop = cl.isMultiHop;
    result.isFeeOnTransfer = cl.isFeeOnTransfer;
    result.isIntentBased = cl.isIntentBased;
    result.isAccountAbstraction = cl.isAccountAbstraction;
    result.nativeWrapAmount = fs.nativeWrap;
    result.nativeUnwrapAmount = fs.nativeUnwrap;
    result.nativeOut = fs.nativeValueOut;
    result.nativeIn = fs.nativeValueIn;

    // Copy flows
    for (const auto& [tok, net] : fs.netFlow) {
        if (net == 0) continue;
        TokenFlow tf;
        tf.token = tok;
        tf.amount = net >= 0 ? net : -net;
        tf.isNative = (tok == chain.nativeMarker);
        tf.viaPermit2 = fs.permit2SpentTokens.count(tok) > 0;
        if (net > 0 && fs.inSources.count(tok)) {
            tf.from = fs.inSources.at(tok).empty() ? "" : fs.inSources.at(tok).front();
            tf.to = wa;
        } else if (net < 0 && fs.outDests.count(tok)) {
            tf.from = wa;
            tf.to = fs.outDests.at(tok).empty() ? "" : fs.outDests.at(tok).front();
        }
        result.allFlows.push_back(tf);
        result.touchedTokens.insert(tok);
    }

    // Signals
    result.hasSwapEvent = !idx.swaps.empty();
    result.isUniversalRouter = ur.present;
    result.isGenericMulticall = isGenericMulticall;
    result.hasPermit2Signal = ur.hasPermit2 || !idx.permit2Events.empty() || !fs.permit2SpentTokens.empty();
    result.dexActivityDetected = result.hasSwapEvent || !cl.venue.empty() || ur.present || isGenericMulticall;
    result.lpMintOrBurnSeen = !fs.mintedIn.empty() || !fs.burnedOut.empty();
    result.lpV3EventSeen = !idx.v3LpEvents.empty();
    result.lpPoolIdentitySeen = false;
    for (const auto& [tok, net] : fs.netFlow) {
        if (idx.poolAddresses.count(tok) > 0) { result.lpPoolIdentitySeen = true; break; }
    }
    result.isBridge = (cl.type == TxType::BRIDGE);
    result.isNftTrade = (cl.type == TxType::NFT_TRADE);
    result.isStaking = (cl.type == TxType::STAKE || cl.type == TxType::UNSTAKE);
    result.swapLegCount = static_cast<int>(idx.swaps.size());

    // Venues
    for (const auto& pool : idx.poolAddresses) {
        std::string lbl = chain.lookupRouterLabel(pool);
        if (!lbl.empty()) result.venuesDetected.insert(lbl);
    }
    if (!cl.venue.empty()) result.venuesDetected.insert(cl.venue);

    // Build swap legs and route
    for (size_t i = 0; i < idx.swaps.size(); i++) {
        const auto& rl = idx.swaps[i];
        SwapLeg leg;
        leg.pool = rl.address;
        leg.protocol = chain.lookupRouterLabel(rl.address);
        if (leg.protocol.empty()) leg.protocol = "Unknown Pool";
        leg.legIndex = static_cast<int>(i);
        if (rl.data.size() >= 2) {
            std::string d = rl.data.substr(2);
            if (d.length() == 256) {
                auto a0in = parseHexIntOpt("0x" + d.substr(0, 64));
                auto a1in = parseHexIntOpt("0x" + d.substr(64, 64));
                auto a0out = parseHexIntOpt("0x" + d.substr(128, 64));
                auto a1out = parseHexIntOpt("0x" + d.substr(192, 64));
                if (a0in && *a0in > 0) leg.amountIn = *a0in;
                if (a1in && *a1in > 0) leg.amountIn = *a1in;
                if (a0out && *a0out > 0) leg.amountOut = *a0out;
                if (a1out && *a1out > 0) leg.amountOut = *a1out;
            } else if (d.length() == 320 || d.length() == 448) {
                auto a0 = parseHexIntOpt("0x" + d.substr(0, 64));
                auto a1 = parseHexIntOpt("0x" + d.substr(64, 64));
                if (a0 && *a0 != 0) { if (*a0 > 0) leg.amountIn = *a0; else leg.amountOut = -(*a0); }
                if (a1 && *a1 != 0) { if (*a1 > 0) leg.amountIn = *a1; else leg.amountOut = -(*a1); }
            }
        }
        result.swapLegs.push_back(leg);
    }

    // Simple route reconstruction: assume sequential legs
    if (result.swapLegs.size() >= 2) {
        for (size_t i = 0; i + 1 < result.swapLegs.size(); i++) {
            RouteStep step;
            step.protocol = result.swapLegs[i].protocol;
            step.amountIn = result.swapLegs[i].amountIn;
            step.amountOut = result.swapLegs[i].amountOut;
            result.route.push_back(step);
        }
    }

    // USD calculation
    if (!result.tokenAddr.empty() && in.prices) {
        int dec = (result.tokenAddr == chain.nativeMarker) ? 18 :
                  (in.tokens ? in.tokens->getDecimals(result.tokenAddr) : 18);
        uint64_t price = (result.tokenAddr == chain.nativeMarker) ?
                         in.prices->getPriceNanos(chain.wrappedNative) :
                         in.prices->getPriceNanos(result.tokenAddr);
        result.usdNanos = calcUsdNanos(result.rawAmount, dec, price);
    }

    // Unknown reason
    if (result.type == TxType::SWAP && result.confidence <= ConfidenceLevel::LOW) {
        bool hasCounter = !result.counterAddr.empty() && result.counterAmount > 0;
        if (!hasCounter) result.unknownReason = "NO_COUNTER_FLOW";
        else if (result.hasSwapEvent && cl.venue.empty()) result.unknownReason = "UNKNOWN_ROUTER";
        else result.unknownReason = "OTHER";
    }

    return result;
}

// ============================================================================
// BATCH ANALYSIS
// ============================================================================

std::vector<TxResultV2> analyzeTxBatch(
    const std::vector<AnalysisInput>& inputs,
    const std::vector<std::string>& wallets) {
    std::vector<TxResultV2> results;
    results.reserve(inputs.size());
    for (size_t i = 0; i < inputs.size(); i++) {
        results.push_back(analyzeTxV2(inputs[i], wallets[i]));
    }
    return results;
}

// ============================================================================
// LEGACY COMPATIBILITY
// ============================================================================

namespace {
    ChainContext g_chain = makeBscContext();
}

const std::string WBNB_ADDR = "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c";
const std::string NATIVE_BNB_MARKER = "native:bnb";

const ChainContext& chainCtx() { return g_chain; }
void setChainContext(const ChainContext& ctx) { g_chain = ctx; }

struct LegacyPriceOracle : IPriceOracle {
    uint64_t getPriceNanos(const std::string& tokenAddr) const override {
        return ::getPriceNanos(tokenAddr);
    }
};

struct LegacyTokenRegistry : ITokenRegistry {
    int getDecimals(const std::string& tokenAddr) const override {
        return ::getDecimals(tokenAddr);
    }
    std::string getSymbol(const std::string& tokenAddr) const override {
        return ::getSymbol(tokenAddr);
    }
};

TxResult analyzeTx(const json& tx, const json& receipt, const std::string& wa) {
    TxResult out{};
    static LegacyPriceOracle s_prices;
    static LegacyTokenRegistry s_tokens;

    AnalysisInput input;
    input.tx = tx;
    input.receipt = receipt;
    input.chain = chainCtx();
    input.prices = &s_prices;
    input.tokens = &s_tokens;

    TxResultV2 r = analyzeTxV2(input, wa);

    out.valid = r.valid;
    out.isSwap = r.isSwap();
    out.venue = r.venue;
    out.tokenAddr = r.tokenAddr;
    out.counterAddr = r.counterAddr;
    out.rawAmount = r.rawAmount;
    out.counterAmount = r.counterAmount;
    out.usdNanos = r.usdNanos;
    out.isBuy = r.isBuy;
    out.hasSwapEvent = r.hasSwapEvent;
    out.isUniversalRouter = r.isUniversalRouter;
    out.isGenericMulticall = r.isGenericMulticall;
    out.hasPermit2Signal = r.hasPermit2Signal;
    out.dexActivityDetected = r.dexActivityDetected;
    out.lpMintOrBurnSeen = r.lpMintOrBurnSeen;
    out.lpV3EventSeen = r.lpV3EventSeen;
    out.lpPoolIdentitySeen = r.lpPoolIdentitySeen;
    out.unknownReason = r.unknownReason;

    return out;
}
