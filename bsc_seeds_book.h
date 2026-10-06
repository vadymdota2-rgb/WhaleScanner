#pragma once
// Собрано tools/bsc_seeds.py — руками не правится. Адреса бирж из отчётов о
// резервах (DefiLlama, projects/<биржа>; резервы Binance), проверенные в BSC:
// обычный кошелёк, сам отправил от пяти транзакций. Число — сколько отправил.
// Binance 20, Bitget 19, OKX 17, Gate 4, KuCoin 4, HTX 2, CoinEx 2, Bitfinex 1, Bitrue 1, Gemini 1

struct BscSeedBook { const char* addr; const char* ex; };
inline const BscSeedBook BSC_SEEDS_BOOK[] = {
    {"0x08439901c2bb071cd0812ed329675c9657434083", "Binance"},  // 25707
    {"0x98adef6f2ac8572ec48965509d69a8dd5e8bba9d", "Binance"},  // 5765
    {"0x4ed6cf63bd9c009d247ee51224fc1c7041f517f1", "Binance"},  // 3850
    {"0x18e226459ccf0eec276514a4fd3b226d8961e4d1", "Binance"},  // 3757
    {"0x0e4158c85ff724526233c1aeb4ff6f0c46827fbe", "Binance"},  // 3242
    {"0x4fdfe365436b5273a42f135c6a6244a20404271e", "Binance"},  // 1453
    {"0x4aec0e98fc1fb55b9cc2faaa7a81acca42cb4e96", "Binance"},  // 919
    {"0xd3a22590f8243f8e83ac230d1842c9af0404c4a1", "Binance"},  // 284
    {"0xbf83d18a46325acb7d8f40a462d23a92f467ed7a", "Binance"},  // 188
    {"0x835678a611b28684005a5e2233695fb6cbbb0007", "Binance"},  // 185
    {"0x87433fec6f8d9df13d1e17c4b11364ecd2e93a51", "Binance"},  // 181
    {"0x43684d03d81d3a4c70da68febdd61029d426f042", "Binance"},  // 147
    {"0x86523c87c8ec98c7539e2c58cd813ee9d1a08d96", "Binance"},  // 143
    {"0x28c6c06298d514db089934071355e5743bf21d60", "Binance"},  // 42
    {"0xa7c0d36c4698981fab42a7d8c783674c6fe2592d", "Binance"},  // 9
    {"0x28e8d4eab37cfbf49edf7ff39c27377b7ae3a2a0", "Binance"},  // 7
    {"0xb2a6a72843db0f508204a56448413f3867ea691a", "Binance"},  // 7
    {"0xab72bd3eb3b5cc90165fa39da85ad0d496330c00", "Binance"},  // 6
    {"0x22132139bf7f3921b1cadeab931f4fbf7bf2bc9e", "Binance"},  // 5
    {"0x43839fe6bb18eae45c4228e5d6c8521a9ab57b6e", "Binance"},  // 5
    {"0x77134cbc06cb00b66f4c7e623d5fdbf6777635ec", "Bitfinex"},  // 7
    {"0xbcf6011192399df75a96b0a4ce47c4820853e9e5", "Bitget"},  // 3277505
    {"0x864a7fa57e0f8902a2de4892e925f1272edbe3fa", "Bitget"},  // 2774372
    {"0x1084203d70950bd7a93aef75eb32a51df2422a07", "Bitget"},  // 2676794
    {"0xed9ab5bc05a0152b9dc2f7902e8369af7bc18771", "Bitget"},  // 184999
    {"0xffa8db7b38579e6a2d14f9b347a9ace4d044cd54", "Bitget"},  // 4611
    {"0x26209d9f0dc3ac0129c3fb1badabfeb9ee728c66", "Bitget"},  // 775
    {"0xa316c725bc8401c97d6d96f283c14b827541744e", "Bitget"},  // 761
    {"0x1ff33329a8f8c1927131cbb72362b00abeea02d3", "Bitget"},  // 383
    {"0xe7b3b0a59b026ec1fef16561daf93672a61bafec", "Bitget"},  // 222
    {"0x149ded7438caf5e5bfdc507a6c25436214d445e1", "Bitget"},  // 82
    {"0xc43999113f8fe724d91356c26105def1449ebdfd", "Bitget"},  // 78
    {"0x4cad1efbfd5f0848e0c3c14fab23f92b2fa81f17", "Bitget"},  // 63
    {"0xadfffc33cdc9970349cbcea3d73ec343d6ed116d", "Bitget"},  // 59
    {"0xac65bdf867103ae2c3a75cdd4b68f9d7178c604f", "Bitget"},  // 22
    {"0x4c1d7de286d7c20df5f2ba44b3bc706c1e03bf13", "Bitget"},  // 20
    {"0xe4786cfe980ef5a6428a2fffafabf24f1fc79b64", "Bitget"},  // 20
    {"0x80097a87a7dcde470e34c10b5cceb85abf83b531", "Bitget"},  // 19
    {"0x14b5f559c27bc00c39f668a88471498d68d18768", "Bitget"},  // 17
    {"0x8911b8f5127eec40c14e1ad0500dc4dbd279d7a7", "Bitget"},  // 5
    {"0x868f027a5e3bd1cd29606a6681c3ddb7d3dd9b67", "Bitrue"},  // 1008209
    {"0x85cf05f35b6d542ac1d777d3f8cfde57578696fc", "CoinEx"},  // 1214610
    {"0xda07f1603a1c514b2f4362f3eae7224a9cdefaf9", "CoinEx"},  // 484
    {"0xf379a3d1ab6625eef34347d054cfaeafdf8f24a7", "Gate"},  // 102720
    {"0x85faa6c1f2450b9caea300838981c2e6e120c35c", "Gate"},  // 719
    {"0xa4992ccf2a74132936b87cbf28b5d52304ba3be7", "Gate"},  // 183
    {"0x1c4b70a3968436b9a0a9cf5205c787eb81bb558c", "Gate"},  // 15
    {"0xd24400ae8bfebb18ca49be86258a3c749cf46853", "Gemini"},  // 690
    {"0xdd3cb5c974601bc3974d908ea4a86020f9999e0c", "HTX"},  // 1392597
    {"0x18709e89bd403f470088abdacebe86cc60dda12e", "HTX"},  // 125
    {"0xf8ba3ec49212ca45325a2335a8ab1279770df6c0", "KuCoin"},  // 101459
    {"0xbb36acf8c156f19e9550d1f66b1cdd2cb003b65d", "KuCoin"},  // 828
    {"0x2933782b5a8d72f2754103d1489614f29bfa4625", "KuCoin"},  // 740
    {"0x7b915c27a0ed48e2ce726ee40f20b2bf8a88a1b3", "KuCoin"},  // 83
    {"0x42cf18596ee08e877d532df1b7cf763059a7ea57", "OKX"},  // 230907
    {"0x8c3cb9665833fd9f79eb14cba16d82bbab6f22d8", "OKX"},  // 43792
    {"0xb4ec508adeb174610b4295e233a458b3475964f7", "OKX"},  // 14944
    {"0x3c5883c650d600bd543a9b5c8d9a3a6f5d16b8f4", "OKX"},  // 11541
    {"0x0799ddbf6f14db566ca4df4ff0575c4cc1e7749c", "OKX"},  // 6671
    {"0xbb3c6d28def21b6297016622a57a0b05015e3ad2", "OKX"},  // 2251
    {"0x611f7bf868a6212f871e89f7e44684045ddfb09d", "OKX"},  // 1987
    {"0xb0a27099582833c0cb8c7a0565759ff145113d64", "OKX"},  // 1802
    {"0xc68c17e6eec0fde3605c595c9b98de5c1a4cc3e4", "OKX"},  // 983
    {"0x03ae1a796dfe0400439211133d065bda774b9d3e", "OKX"},  // 963
    {"0xf81233a61c0d6d13c6fe504ddbba3e2630ea0c5c", "OKX"},  // 766
    {"0x45f329ff834a84fccea2319f91e1993540b22a2a", "OKX"},  // 525
    {"0x5f8215ee653cb7225c741c7aa8591468d1f158b8", "OKX"},  // 193
    {"0x62383739d68dd0f844103db8dfb05a7eded5bbe6", "OKX"},  // 11
    {"0xebe80f029b1c02862b9e8a70a7e5317c06f62cae", "OKX"},  // 11
    {"0x559432e18b281731c054cd703d4b49872be4ed53", "OKX"},  // 5
    {"0x7e4aa755550152a522d9578621ea22edab204308", "OKX"},  // 5
};
