#pragma once

#include <string>
#include <cstdint>
#include <set>
#include <map>
#include <boost/multiprecision/cpp_int.hpp>
#include "json.hpp"

using boost::multiprecision::cpp_int;

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

cpp_int parseUint256(const std::string& h);
cpp_int hexToCppInt(const std::string& h);
std::string formatAmount(const cpp_int& raw, int dec);
cpp_int calcUsdNanos(const cpp_int& raw, int dec, uint64_t pn);
std::string formatUsd(const cpp_int& n);
cpp_int calcUnitPriceNanos(const cpp_int& usdNanos, const cpp_int& rawAmount, int dec);
std::string formatPriceUsd(const cpp_int& n);

TxResult analyzeTx(const nlohmann::json& tx, const nlohmann::json& receipt, const std::string& wa);
