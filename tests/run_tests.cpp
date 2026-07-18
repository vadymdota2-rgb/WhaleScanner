#include "tx_analyzer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <map>

namespace fs = std::filesystem;
using json = nlohmann::json;

extern std::map<std::string, int> g_testDecimals;
extern std::map<std::string, uint64_t> g_testPrices;

int getDecimals(const std::string& a) {
    auto it = g_testDecimals.find(a);
    return it != g_testDecimals.end() ? it->second : 18;
}
uint64_t getPriceNanos(const std::string& a) {
    auto it = g_testPrices.find(a);
    return it != g_testPrices.end() ? it->second : 500000000ULL;
}
std::string getSymbol(const std::string&) { return "TOKEN"; }

std::map<std::string, int> g_testDecimals = {
    {"0x55d398326f99059ff775485246999027b3197955", 18},
};
std::map<std::string, uint64_t> g_testPrices = {
    {"0x55d398326f99059ff775485246999027b3197955", 1000000000ULL},
};

std::string readFile(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool checkField(const json& expected, const std::string& key, bool actual, std::vector<std::string>& errs) {
    if (!expected.contains(key)) return true;
    bool exp = expected[key].get<bool>();
    if (exp != actual) {
        errs.push_back(key + ": expected " + (exp ? "true" : "false") + ", got " + (actual ? "true" : "false"));
        return false;
    }
    return true;
}

bool checkStringField(const json& expected, const std::string& key, const std::string& actual, std::vector<std::string>& errs) {
    if (!expected.contains(key)) return true;
    std::string exp = expected[key].get<std::string>();
    if (exp != actual) {
        errs.push_back(key + ": expected [" + exp + "], got [" + actual + "]");
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    fs::path testsDir = (argc > 1) ? fs::path(argv[1]) : fs::current_path();
    int passed = 0, failed = 0;

    for (auto& entry : fs::directory_iterator(testsDir)) {
        if (!entry.is_directory()) continue;
        fs::path dir = entry.path();
        fs::path txPath = dir / "tx.json";
        fs::path receiptPath = dir / "receipt.json";
        fs::path expectedPath = dir / "expected.json";
        fs::path walletPath = dir / "wallet.txt";
        if (!fs::exists(txPath) || !fs::exists(receiptPath) || !fs::exists(expectedPath) || !fs::exists(walletPath)) continue;

        std::string caseName = dir.filename().string();
        json tx = json::parse(readFile(txPath));
        json receipt = json::parse(readFile(receiptPath));
        json expected = json::parse(readFile(expectedPath));
        std::string wallet = readFile(walletPath);
        while (!wallet.empty() && (wallet.back() == '\n' || wallet.back() == '\r' || wallet.back() == ' ')) wallet.pop_back();

        TxResult r = analyzeTx(tx, receipt, wallet);

        std::vector<std::string> errs;
        checkField(expected, "valid", r.valid, errs);
        checkField(expected, "isSwap", r.isSwap, errs);
        checkField(expected, "isBuy", r.isBuy, errs);
        checkStringField(expected, "tokenAddr", r.tokenAddr, errs);
        checkStringField(expected, "venue", r.venue, errs);

        if (errs.empty()) {
            std::cout << "PASS  " << caseName << std::endl;
            passed++;
        } else {
            std::cout << "FAIL  " << caseName << std::endl;
            for (auto& e : errs) std::cout << "        " << e << std::endl;
            failed++;
        }
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed == 0 ? 0 : 1;
}
