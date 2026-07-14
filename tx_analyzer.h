#pragma once

#include <string>
#include <cstdint>
#include <set>
#include <map>
#include <boost/multiprecision/cpp_int.hpp>
#include "json.hpp"

using boost::multiprecision::cpp_int;

struct ChainContext {
    std::string nativeSymbol;
    std::string nativeMarker;
    std::string wrappedNative;

    std::set<std::string> stablecoins;
    std::set<std::string> baseAssets;

    std::map<std::string, std::string> routers;
    std::set<std::string> bridges;
};

struct TxResult {
    bool valid, isSwap, isBuy;
    cpp_int rawAmount, usdNanos;
    std::string tokenAddr;
    std::string venue;
    std::string counterAddr;
    cpp_int counterAmount;
};

extern const std::string WBNB_ADDR;
extern const std::string NATIVE_BNB_MARKER;

cpp_int parseUint256(const std::string&);
cpp_int hexToCppInt(const std::string&);
std::string formatAmount(const cpp_int&, int);
cpp_int calcUsdNanos(const cpp_int&, int, uint64_t);
std::string formatUsd(const cpp_int&);
cpp_int calcUnitPriceNanos(const cpp_int&, const cpp_int&, int);
std::string formatPriceUsd(const cpp_int&);

ChainContext makeBscContext();
const ChainContext& chainCtx();
void setChainContext(const ChainContext&);

bool isBaseAsset(const std::string&);
bool isStablecoin(const std::string&);
std::string lookupRouterLabel(const std::string&);

TxResult analyzeTx(const nlohmann::json&,
                   const nlohmann::json&,
                   const std::string&);
