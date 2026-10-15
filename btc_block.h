#pragma once

// Блок биткоина, разобранный до адресов и сумм: его читают сканер бирж
// (btc_chain.cpp) и разметка ETF (btc_etf.cpp).

#include <string>
#include <vector>

struct BIn { std::string addr; long long sats = 0; };
struct BOut { std::string addr; long long sats = 0; };
struct BTx {
    std::string txid;
    bool coinbase = false;
    std::vector<BIn> in;
    std::vector<BOut> out;
    long long inSum() const { long long s = 0; for (auto& i : in) s += i.sats; return s; }
    long long outSum() const { long long s = 0; for (auto& o : out) s += o.sats; return s; }
};

struct Block {
    long long height = 0;
    long long ts = 0;
    std::string hash;
    // Хэш предыдущего блока — по нему видна перестройка цепочки.
    std::string prev;
    std::vector<BTx> txs;
};
