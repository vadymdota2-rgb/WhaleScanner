#include "chains.h"
#include "utils.h"

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
    c.v2Factory = "0xca143ce32fe78f1f7019d7d551a6402fc5350c73";
    c.stablecoins = {
        "0x55d398326f99059ff775485246999027b3197955",
        "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d",
        "0xe9e7cea3dedca5984780bafc599bd69add087d56",
        "0xc5f0f7b66764f6ec8c8dff7ba683102295e16409",
        "0x8d0d000ee44948fc98c9b98a4fa4921476f08b0d"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(WBNB_ADDR);
    // Крупные ликвидные активы BSC — не помечаем ⚠ в холде
    c.baseAssets.insert("0x7130d2a12b9bcbfae4f2634d864a1ee1ce3ead9c"); // BTCB
    c.baseAssets.insert("0x2170ed0880ac9a755fd29b2688956bd959f933f8"); // ETH
    c.baseAssets.insert("0x0e09fabb73bd3ade0a17ecc321fd13a19e81ce82"); // CAKE
    c.baseAssets.insert("0x1af3f329e8be154074d8769d1ffa4ee058b1dbc3"); // DAI
    c.baseAssets.insert("0x1d2f0da169ceb9fc7b3144628db156f3f6c60dbe"); // XRP
    c.baseAssets.insert("0x3ee2200efb3400fabb9aacf31297cbdd1d435d47"); // ADA
    c.baseAssets.insert("0x4338665cbb7b2485a8855a139b75d5e34ab0db94"); // LTC
    c.baseAssets.insert("0xba2ae424d960c26247dd6c32edc70b295c744c43"); // DOGE
    c.baseAssets.insert("0x7083609fce4d1d8dc0c979aab8c869ea2c873402"); // DOT
    c.baseAssets.insert("0xbf5140a22578168fd72bdfcb88963812f75091f0"); // UNI
    c.baseAssets.insert("0x0d8ce2a99bb6e3b7db580ed848240e4a0f9ae153"); // FIL
    c.baseAssets.insert("0xcc42724c6683b7e57334c4e856f4c9965ed682bd"); // MATIC
    c.baseAssets.insert("0x1ce0c2827e2ef14d5c4f29a091d735a204794041"); // AVAX
    c.baseAssets.insert("0x570a5d26f7765ecb712c0924e4de545b89fd43df"); // SOL
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
        {"0x07964f135f276412b3182a3b2407b8dd45000000", "Transit Swap"},
        {"0x6aba0315493b7e6989041c91181337b662fb1b90", "Binance Alpha"},
        {"0x88649f4743a758171077b98ee2003f1989b1615a", "Binance Wallet"},
    };
    c.bridges = {
        "0x4a364f8c717caad9a442737eb7b8a55cc6cf18d8",
        "0x6694340fc020c5e6b96567843da2df01b2ce1eb6",
        "0x78bc5ee9f11d133a08b331c2e18fe81be0ed02dc",
        "0xdd90e5e87a2081dcf0391920868ebc2ffb81a1af",
    };
    c.knownPoolInfra = {
        "0x28e2ea090877bf75740558f6bfb36a5ffee9e9df",
        "0x238a358808379702088667322f80ac48bad5e6c4",
        "0xa0ffb9c1ce1fe56963b0321b32e7a0302114058b",
        "0xc697d2898e0d09264376196696c51d7abbbaa4a9",
    };
    c.rpcEndpoints = {
    "https://bscrpc.pancakeswap.finance",
    "https://rpc-bsc.48.club",
    "https://bsc.rpc.blxrbdn.com",
    "https://bsc.blockrazor.xyz",
    "https://nodes.sequence.app/bsc",
    "https://bsc-dataseed1.bnbchain.org",
    "https://bsc-dataseed.binance.org",
    "https://bsc-dataseed.nariox.org",
    "https://bsc.nodereal.io",
    "https://bsc-dataseed1.defibit.io",
        "https://bsc.publicnode.com",
        "https://bsc-mainnet.public.blastapi.io",
        "https://bsc.meowrpc.com",
    };

    c.dexscreenerChainId = "bsc";
    c.coingeckoPlatform = "binance-smart-chain";
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
    c.knownPoolInfra = {
        "0x000000000004444c5dc75cb358380d2e3de08a90",
    };
    c.routers = {
        {"0x7a250d5630b4cf539739df2c5dacb4c659f2488d", "Uniswap V2"},
        {"0xe592427a0aece92de3edee1f18e0157c05861564", "Uniswap V3"},
        {"0xef1c6e67703c7bd7107eed8303fbe6ec2554bf6b", "Uniswap (Universal Router)"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)"},
        {"0x66a9893cc07d91d95644aedd05d03f95e1dba8af", "Uniswap V4 (Universal Router)"},
    };
    c.rpcEndpoints = {
    "https://eth.llamarpc.com",
    "https://ethereum.publicnode.com",
    "https://rpc.ankr.com/eth",
    "https://cloudflare-eth.com"
    };

    c.dexscreenerChainId = "ethereum";
    c.coingeckoPlatform = "ethereum";
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
    c.knownPoolInfra = {
        "0x498581ff718922c3f8e6a244956af099b2652b2b",
    };
    c.routers = {
        {"0x198ef79f1f515f02dfe9e3115ed9fc07183f02fc", "Uniswap (Universal Router)"},
        {"0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)"},
        {"0x6ff5693b99212da76ad316178a184ab56d299b43", "Uniswap V4 (Universal Router)"},
    };
    c.rpcEndpoints = {
    "https://mainnet.base.org",
    "https://base.publicnode.com"
    };

    c.dexscreenerChainId = "base";
    c.coingeckoPlatform = "base";
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
    c.rpcEndpoints = {
    "https://arbitrum.llamarpc.com",
    "https://arbitrum-one.publicnode.com"
    };

    c.dexscreenerChainId = "arbitrum";
    c.coingeckoPlatform = "arbitrum-one";
    return c;
}

bool chainConfigByName(const std::string& name, ChainContext& out) {
    std::string n = toLower(name);
    if (n == "bsc" || n == "bnb")          { out = makeBscContext();      return true; }
    if (n == "ethereum" || n == "eth")     { out = makeEthereumContext(); return true; }
    if (n == "base")                       { out = makeBaseContext();     return true; }
    if (n == "arbitrum" || n == "arb")     { out = makeArbitrumContext(); return true; }
    return false;
}
