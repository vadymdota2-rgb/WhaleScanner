#include "tx_analyzer.h"

#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <limits>
#include <functional>
#include "utils.h"

using json = nlohmann::json;

uint64_t getPriceNanos(const std::string& token);
int getDecimals(const std::string& addr);
std::string getSymbol(const std::string& addr);

namespace {

const std::string ZERO_ADDR = "0x0000000000000000000000000000000000000000";
const std::string DEAD_ADDR = "0x000000000000000000000000000000000000dead";

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool isHexString(const std::string& value) {
    if (value.size() < 2 || value[0] != '0' || value[1] != 'x') return false;
    for (size_t i = 2; i < value.size(); ++i) {
        if (hexNibble(value[i]) < 0) return false;
    }
    return true;
}

cpp_int absInt(const cpp_int& v) {
    return v < 0 ? -v : v;
}

std::string normalizeAddress(const std::string& value) {
    return toLower(value);
}

class AbiReader {
public:
    explicit AbiReader(const std::string& calldata) {
        if (calldata.size() >= 2 && calldata.substr(0, 2) == "0x") hex_ = calldata.substr(2);
        else hex_ = calldata;
    }

    bool valid() const {
        return hex_.size() >= 8 && (hex_.size() % 2 == 0);
    }

    std::string selector() const {
        if (hex_.size() < 8) return {};
        return toLower(hex_.substr(0, 8));
    }

    bool readWord(size_t index, std::string& out) const {
        const size_t pos = 8 + index * 64;
        if (pos > hex_.size() || hex_.size() - pos < 64) return false;
        out = hex_.substr(pos, 64);
        return true;
    }

    bool readUint(size_t index, cpp_int& out) const {
        std::string word;
        if (!readWord(index, word)) return false;
        out = hexToCppInt("0x" + word);
        return true;
    }

    bool readSize(size_t index, size_t& out) const {
        cpp_int v;
        if (!readUint(index, v)) return false;
        if (v < 0 || v > std::numeric_limits<size_t>::max()) return false;
        out = v.convert_to<size_t>();
        return true;
    }

    bool readAddress(size_t index, std::string& out) const {
        std::string word;
        if (!readWord(index, word)) return false;
        out = "0x" + toLower(word.substr(24, 40));
        return true;
    }

    bool readBool(size_t index, bool& out) const {
        cpp_int value;
        if (!readUint(index, value)) return false;
        out = value != 0;
        return true;
    }

    bool readBytes32(size_t index, std::string& out) const {
        std::string word;
        if (!readWord(index, word)) return false;
        out = "0x" + toLower(word);
        return true;
    }

    bool readBytes(size_t offsetWord, std::string& out, size_t maxBytes = 1 << 20) const {
        size_t byteOffset = 0;
        if (!readSize(offsetWord, byteOffset)) return false;
        const size_t pos = 8 + byteOffset * 2;
        if (pos > hex_.size() || hex_.size() - pos < 64) return false;

        cpp_int lenVal = hexToCppInt("0x" + hex_.substr(pos, 64));
        if (lenVal < 0 || lenVal > maxBytes) return false;
        const size_t len = lenVal.convert_to<size_t>();
        const size_t dataPos = pos + 64;
        if (dataPos > hex_.size() || hex_.size() - dataPos < len * 2) return false;
        out = "0x" + hex_.substr(dataPos, len * 2);
        return true;
    }

    bool readAddressArray(size_t offsetWord, std::vector<std::string>& out, size_t maxItems = 64) const {
        size_t byteOffset = 0;
        if (!readSize(offsetWord, byteOffset)) return false;
        const size_t pos = 8 + byteOffset * 2;
        if (pos > hex_.size() || hex_.size() - pos < 64) return false;

        cpp_int lenVal = hexToCppInt("0x" + hex_.substr(pos, 64));
        if (lenVal < 0 || lenVal > maxItems) return false;
        const size_t len = lenVal.convert_to<size_t>();
        const size_t dataPos = pos + 64;
        if (dataPos > hex_.size() || len > (hex_.size() - dataPos) / 64) return false;

        out.clear();
        out.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            const std::string word = hex_.substr(dataPos + i * 64, 64);
            out.push_back("0x" + toLower(word.substr(24, 40)));
        }
        return true;
    }

    bool readBytesArray(size_t offsetWord, std::vector<std::string>& out,
                        size_t maxItems = 128, size_t maxBytesEach = 1 << 20) const {
        size_t arrayOffset = 0;
        if (!readSize(offsetWord, arrayOffset)) return false;
        const size_t base = 8 + arrayOffset * 2;
        if (base > hex_.size() || hex_.size() - base < 64) return false;

        cpp_int lenVal = hexToCppInt("0x" + hex_.substr(base, 64));
        if (lenVal < 0 || lenVal > maxItems) return false;
        const size_t len = lenVal.convert_to<size_t>();

        const size_t offsetsBase = base + 64;
        if (offsetsBase > hex_.size() || len > (hex_.size() - offsetsBase) / 64) return false;

        out.clear();
        out.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            cpp_int relVal = hexToCppInt("0x" + hex_.substr(offsetsBase + i * 64, 64));
            if (relVal < 0 || relVal > std::numeric_limits<size_t>::max()) return false;
            const size_t rel = relVal.convert_to<size_t>();
            const size_t item = offsetsBase + rel * 2;
            if (item > hex_.size() || hex_.size() - item < 64) return false;

            cpp_int itemLenVal = hexToCppInt("0x" + hex_.substr(item, 64));
            if (itemLenVal < 0 || itemLenVal > maxBytesEach) return false;
            const size_t itemLen = itemLenVal.convert_to<size_t>();
            const size_t dataPos = item + 64;
            if (dataPos > hex_.size() || hex_.size() - dataPos < itemLen * 2) return false;
            out.push_back("0x" + hex_.substr(dataPos, itemLen * 2));
        }
        return true;
    }

    bool readTupleWord(size_t tupleOffsetWord, size_t tupleWordIndex, std::string& out) const {
        size_t tupleOffset = 0;
        if (!readSize(tupleOffsetWord, tupleOffset)) return false;
        const size_t pos = 8 + tupleOffset * 2 + tupleWordIndex * 64;
        if (pos > hex_.size() || hex_.size() - pos < 64) return false;
        out = hex_.substr(pos, 64);
        return true;
    }

    bool readTupleUint(size_t tupleOffsetWord, size_t tupleWordIndex, cpp_int& out) const {
        std::string word;
        if (!readTupleWord(tupleOffsetWord, tupleWordIndex, word)) return false;
        out = hexToCppInt("0x" + word);
        return true;
    }

    bool readTupleAddress(size_t tupleOffsetWord, size_t tupleWordIndex, std::string& out) const {
        std::string word;
        if (!readTupleWord(tupleOffsetWord, tupleWordIndex, word)) return false;
        out = "0x" + toLower(word.substr(24, 40));
        return true;
    }

    bool readTupleBytes(size_t tupleOffsetWord, size_t tupleWordIndex, std::string& out,
                        size_t maxBytes = 1 << 20) const {
        size_t tupleOffset = 0;
        if (!readSize(tupleOffsetWord, tupleOffset)) return false;
        const size_t tupleBase = 8 + tupleOffset * 2;

        std::string offsetWord;
        if (!readTupleWord(tupleOffsetWord, tupleWordIndex, offsetWord)) return false;
        cpp_int relVal = hexToCppInt("0x" + offsetWord);
        if (relVal < 0 || relVal > std::numeric_limits<size_t>::max()) return false;
        const size_t rel = relVal.convert_to<size_t>();

        const size_t pos = tupleBase + rel * 2;
        if (pos > hex_.size() || hex_.size() - pos < 64) return false;
        cpp_int lenVal = hexToCppInt("0x" + hex_.substr(pos, 64));
        if (lenVal < 0 || lenVal > maxBytes) return false;
        const size_t len = lenVal.convert_to<size_t>();
        if (pos + 64 > hex_.size() || hex_.size() - (pos + 64) < len * 2) return false;
        out = "0x" + hex_.substr(pos + 64, len * 2);
        return true;
    }


    struct TargetCall {
        std::string target;
        std::string calldata;
        cpp_int value = 0;
        bool allowFailure = false;
    };

    // Decodes Multicall2 tryAggregate: (address,bytes)[]
    bool readTargetBytesTupleArray(size_t offsetWord,
                                   std::vector<TargetCall>& out,
                                   size_t maxItems = 128,
                                   size_t maxBytesEach = 1 << 20) const {
        size_t arrayOffset = 0;
        if (!readSize(offsetWord, arrayOffset)) return false;
        const size_t base = 8 + arrayOffset * 2;
        if (base > hex_.size() || hex_.size() - base < 64) return false;

        cpp_int lenValue = hexToCppInt("0x" + hex_.substr(base, 64));
        if (lenValue < 0 || lenValue > maxItems) return false;
        const size_t count = lenValue.convert_to<size_t>();
        const size_t offsetsBase = base + 64;
        if (offsetsBase > hex_.size() || count > (hex_.size() - offsetsBase) / 64) return false;

        out.clear();
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            cpp_int relValue = hexToCppInt("0x" + hex_.substr(offsetsBase + i * 64, 64));
            if (relValue < 0 || relValue > std::numeric_limits<size_t>::max()) return false;
            const size_t tupleBase = offsetsBase + relValue.convert_to<size_t>() * 2;
            if (tupleBase > hex_.size() || hex_.size() - tupleBase < 128) return false;

            TargetCall call;
            call.target = "0x" + toLower(hex_.substr(tupleBase + 24, 40));

            cpp_int bytesRel = hexToCppInt("0x" + hex_.substr(tupleBase + 64, 64));
            if (bytesRel < 0 || bytesRel > std::numeric_limits<size_t>::max()) return false;
            const size_t bytesPos = tupleBase + bytesRel.convert_to<size_t>() * 2;
            if (bytesPos > hex_.size() || hex_.size() - bytesPos < 64) return false;

            cpp_int bytesLenValue = hexToCppInt("0x" + hex_.substr(bytesPos, 64));
            if (bytesLenValue < 0 || bytesLenValue > maxBytesEach) return false;
            const size_t bytesLen = bytesLenValue.convert_to<size_t>();
            if (bytesPos + 64 > hex_.size() || hex_.size() - (bytesPos + 64) < bytesLen * 2) return false;
            call.calldata = "0x" + hex_.substr(bytesPos + 64, bytesLen * 2);
            out.push_back(std::move(call));
        }
        return true;
    }

    // Decodes Multicall3 aggregate3: (address,bool,bytes)[]
    bool readAggregate3TupleArray(size_t offsetWord,
                                  std::vector<TargetCall>& out,
                                  size_t maxItems = 128,
                                  size_t maxBytesEach = 1 << 20) const {
        size_t arrayOffset = 0;
        if (!readSize(offsetWord, arrayOffset)) return false;
        const size_t base = 8 + arrayOffset * 2;
        if (base > hex_.size() || hex_.size() - base < 64) return false;

        cpp_int lenValue = hexToCppInt("0x" + hex_.substr(base, 64));
        if (lenValue < 0 || lenValue > maxItems) return false;
        const size_t count = lenValue.convert_to<size_t>();
        const size_t offsetsBase = base + 64;
        if (offsetsBase > hex_.size() || count > (hex_.size() - offsetsBase) / 64) return false;

        out.clear();
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            cpp_int relValue = hexToCppInt("0x" + hex_.substr(offsetsBase + i * 64, 64));
            if (relValue < 0 || relValue > std::numeric_limits<size_t>::max()) return false;
            const size_t tupleBase = offsetsBase + relValue.convert_to<size_t>() * 2;
            if (tupleBase > hex_.size() || hex_.size() - tupleBase < 192) return false;

            TargetCall call;
            call.target = "0x" + toLower(hex_.substr(tupleBase + 24, 40));
            call.allowFailure = hexToCppInt("0x" + hex_.substr(tupleBase + 64, 64)) != 0;

            cpp_int bytesRel = hexToCppInt("0x" + hex_.substr(tupleBase + 128, 64));
            if (bytesRel < 0 || bytesRel > std::numeric_limits<size_t>::max()) return false;
            const size_t bytesPos = tupleBase + bytesRel.convert_to<size_t>() * 2;
            if (bytesPos > hex_.size() || hex_.size() - bytesPos < 64) return false;

            cpp_int bytesLenValue = hexToCppInt("0x" + hex_.substr(bytesPos, 64));
            if (bytesLenValue < 0 || bytesLenValue > maxBytesEach) return false;
            const size_t bytesLen = bytesLenValue.convert_to<size_t>();
            if (bytesPos + 64 > hex_.size() || hex_.size() - (bytesPos + 64) < bytesLen * 2) return false;
            call.calldata = "0x" + hex_.substr(bytesPos + 64, bytesLen * 2);
            out.push_back(std::move(call));
        }
        return true;
    }

    // Decodes Multicall3 aggregate3Value: (address,bool,uint256,bytes)[]
    bool readAggregate3ValueTupleArray(size_t offsetWord,
                                       std::vector<TargetCall>& out,
                                       size_t maxItems = 128,
                                       size_t maxBytesEach = 1 << 20) const {
        size_t arrayOffset = 0;
        if (!readSize(offsetWord, arrayOffset)) return false;
        const size_t base = 8 + arrayOffset * 2;
        if (base > hex_.size() || hex_.size() - base < 64) return false;

        cpp_int lenValue = hexToCppInt("0x" + hex_.substr(base, 64));
        if (lenValue < 0 || lenValue > maxItems) return false;
        const size_t count = lenValue.convert_to<size_t>();
        const size_t offsetsBase = base + 64;
        if (offsetsBase > hex_.size() || count > (hex_.size() - offsetsBase) / 64) return false;

        out.clear();
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            cpp_int relValue = hexToCppInt("0x" + hex_.substr(offsetsBase + i * 64, 64));
            if (relValue < 0 || relValue > std::numeric_limits<size_t>::max()) return false;
            const size_t tupleBase = offsetsBase + relValue.convert_to<size_t>() * 2;
            if (tupleBase > hex_.size() || hex_.size() - tupleBase < 256) return false;

            TargetCall call;
            call.target = "0x" + toLower(hex_.substr(tupleBase + 24, 40));
            call.allowFailure = hexToCppInt("0x" + hex_.substr(tupleBase + 64, 64)) != 0;
            call.value = hexToCppInt("0x" + hex_.substr(tupleBase + 128, 64));

            cpp_int bytesRel = hexToCppInt("0x" + hex_.substr(tupleBase + 192, 64));
            if (bytesRel < 0 || bytesRel > std::numeric_limits<size_t>::max()) return false;
            const size_t bytesPos = tupleBase + bytesRel.convert_to<size_t>() * 2;
            if (bytesPos > hex_.size() || hex_.size() - bytesPos < 64) return false;

            cpp_int bytesLenValue = hexToCppInt("0x" + hex_.substr(bytesPos, 64));
            if (bytesLenValue < 0 || bytesLenValue > maxBytesEach) return false;
            const size_t bytesLen = bytesLenValue.convert_to<size_t>();
            if (bytesPos + 64 > hex_.size() || hex_.size() - (bytesPos + 64) < bytesLen * 2) return false;
            call.calldata = "0x" + hex_.substr(bytesPos + 64, bytesLen * 2);
            out.push_back(std::move(call));
        }
        return true;
    }

private:
    std::string hex_;
};

std::vector<std::string> decodeV3Path(const std::string& bytesHex, bool reverse) {
    std::vector<std::string> out;
    if (!isHexString(bytesHex)) return out;
    std::string h = bytesHex.substr(2);
    if (h.size() < 40) return out;

    size_t pos = 0;
    out.push_back("0x" + toLower(h.substr(pos, 40)));
    pos += 40;

    while (pos + 6 + 40 <= h.size()) {
        pos += 6; // uint24 fee
        out.push_back("0x" + toLower(h.substr(pos, 40)));
        pos += 40;
    }

    if (reverse) std::reverse(out.begin(), out.end());
    return out;
}

struct UniversalRouterCommands {
    bool present = false;
    bool hasWrap = false;
    bool hasUnwrap = false;
    bool hasV2Swap = false;
    bool hasV3Swap = false;
    bool hasSweep = false;
    bool hasTransfer = false;
    bool hasPermit2 = false;
};

uint64_t lowU64FromWord(const std::string& word) {
    if (word.size() != 64) return 0;
    uint64_t value = 0;
    for (size_t i = 48; i < 64; ++i) {
        int n = hexNibble(word[i]);
        if (n < 0) return 0;
        value = (value << 4) | static_cast<uint64_t>(n);
    }
    return value;
}

bool isGenericMulticallSelector(const std::string& input) {
    if (!isHexString(input) || input.size() < 10) return false;
    const std::string selector = toLower(input.substr(2, 8));
    return selector == "ac9650d8" || selector == "5ae401dc" ||
           selector == "252dba42" || selector == "82ad56cb" ||
           selector == "174dea71" || selector == "4d2301cc";
}

UniversalRouterCommands parseExecuteCommands(const std::string& input) {
    UniversalRouterCommands out;
    if (!isHexString(input) || input.size() < 10) return out;

    const std::string selector = toLower(input.substr(2, 8));
    if (selector != "3593564c" && selector != "24856bc3") return out;

    AbiReader abi(input);
    std::string commands;
    if (!abi.readBytes(0, commands, 4096)) return out;

    out.present = true;
    const std::string hex = commands.substr(2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = hexNibble(hex[i]);
        const int lo = hexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) continue;
        const int cmd = ((hi << 4) | lo) & 0x7f;

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

struct TransferEdge {
    std::string token;
    std::string from;
    std::string to;
    cpp_int amount = 0;
    size_t logIndex = 0;
};

struct ParsedReceipt {
    bool valid = false;
    bool anyWalletTransfer = false;
    bool hasSwapEvent = false;
    bool v3Increase = false;
    bool v3Decrease = false;
    bool v3Collect = false;

    cpp_int wrappedNative = 0;
    cpp_int unwrappedNative = 0;

    std::map<std::string, cpp_int> netFlow;
    std::vector<std::string> tokenOrder;
    std::set<std::string> swapPools;
    std::set<std::string> mintedTokens;
    std::set<std::string> burnedTokens;
    std::map<std::string, std::set<std::string>> inSources;
    std::map<std::string, std::set<std::string>> outDestinations;
    std::set<std::string> inCounterparties;
    std::set<std::string> outCounterparties;
    std::vector<TransferEdge> transfers;

    std::string firstCounterparty;
    size_t firstSwapDataHexLen = 0;
};

const std::set<std::string> SWAP_EVENT_TOPICS = {
    "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822",
    "0xc42079f94a6350d7e6235f29174924f928cc2ac818eb64fed8004e115fbcca67",
    "0x19b47279256b2a23a1665c810c8d55a1758940ee09377d4f8d26497a3577dc83",
};

const std::string ERC20_TRANSFER_TOPIC =
    "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
const std::string WNATIVE_DEPOSIT_TOPIC =
    "0xe1fffcc4923d04b559f4d29a8bfc6cda04eb5b0d3c460751c2402c5c5cc9109c";
const std::string WNATIVE_WITHDRAWAL_TOPIC =
    "0x7fcf532c15f0a6db0bd6d0e038bea71d30d808c7d98cb3bf7268a95bf5081b65";
const std::string V3_INCREASE_LIQUIDITY_TOPIC =
    "0x3067048beee31b25b2f1681f88dac838c8bba36af25bfb2b7cf7473a5847e35f";
const std::string V3_DECREASE_LIQUIDITY_TOPIC =
    "0x26f6a048ee9138f2c0ce266f322cb99228e8d619ae2bff30c67f8dcf9d2377b4";
const std::string V3_COLLECT_TOPIC =
    "0x40d0efd1a53d60ecbf40971b9daf7dc90178c3aadc7aab1765632738fa8b8f01";

ParsedReceipt parseReceipt(const json& receipt, const std::string& wallet) {
    ParsedReceipt out;
    if (!receipt.is_object() || !receipt.contains("logs") || !receipt["logs"].is_array()) return out;
    out.valid = true;

    auto touch = [&](const std::string& token) {
        if (!out.netFlow.count(token)) {
            out.netFlow[token] = 0;
            out.tokenOrder.push_back(token);
        }
    };

    for (size_t logIndex = 0; logIndex < receipt["logs"].size(); ++logIndex) {
        const auto& log = receipt["logs"][logIndex];
        if (!log.is_object() || !log.contains("topics") || !log["topics"].is_array() ||
            log["topics"].empty() || !log["topics"][0].is_string()) continue;

        const std::string topic0 = toLower(log["topics"][0].get<std::string>());
        const std::string logAddr =
            (log.contains("address") && log["address"].is_string())
                ? toLower(log["address"].get<std::string>()) : "";

        if (SWAP_EVENT_TOPICS.count(topic0)) {
            out.hasSwapEvent = true;
            if (!logAddr.empty()) out.swapPools.insert(logAddr);
            if (out.firstSwapDataHexLen == 0 && log.contains("data") && log["data"].is_string()) {
                const std::string data = log["data"].get<std::string>();
                out.firstSwapDataHexLen = data.size() >= 2 ? data.size() - 2 : 0;
            }
            continue;
        }

        if (logAddr == chainCtx().wrappedNative &&
            (topic0 == WNATIVE_DEPOSIT_TOPIC || topic0 == WNATIVE_WITHDRAWAL_TOPIC)) {
            if (log.contains("data") && log["data"].is_string()) {
                cpp_int amount = parseUint256(log["data"].get<std::string>());
                if (topic0 == WNATIVE_DEPOSIT_TOPIC) out.wrappedNative += amount;
                else out.unwrappedNative += amount;
            }
            continue;
        }

        if (topic0 == V3_INCREASE_LIQUIDITY_TOPIC) { out.v3Increase = true; continue; }
        if (topic0 == V3_DECREASE_LIQUIDITY_TOPIC) { out.v3Decrease = true; continue; }
        if (topic0 == V3_COLLECT_TOPIC) { out.v3Collect = true; continue; }

        if (topic0 != ERC20_TRANSFER_TOPIC ||
            log["topics"].size() != 3 ||
            !log["topics"][1].is_string() ||
            !log["topics"][2].is_string() ||
            !log.contains("data") ||
            !log["data"].is_string() ||
            logAddr.empty()) continue;

        const std::string t1 = log["topics"][1].get<std::string>();
        const std::string t2 = log["topics"][2].get<std::string>();
        if (t1.size() < 66 || t2.size() < 66) continue;

        const std::string from = "0x" + toLower(t1.substr(26));
        const std::string to = "0x" + toLower(t2.substr(26));

        const cpp_int amount = parseUint256(log["data"].get<std::string>());
        if (amount == 0) continue;

        out.transfers.push_back(TransferEdge{logAddr, from, to, amount, logIndex});

        if (from != wallet && to != wallet) continue;

        touch(logAddr);
        out.anyWalletTransfer = true;

        if (out.firstCounterparty.empty()) {
            out.firstCounterparty = (to == wallet) ? from : to;
        }

        if (to == wallet) {
            out.netFlow[logAddr] += amount;
            out.inSources[logAddr].insert(from);
            if (from == ZERO_ADDR) out.mintedTokens.insert(logAddr);
            else out.inCounterparties.insert(from);
        }

        if (from == wallet) {
            out.netFlow[logAddr] -= amount;
            out.outDestinations[logAddr].insert(to);
            if (to == ZERO_ADDR || to == DEAD_ADDR) out.burnedTokens.insert(logAddr);
            else out.outCounterparties.insert(to);
        }
    }

    return out;
}

bool sourceMatchesPool(const ParsedReceipt& p, const std::string& token, bool incoming) {
    const auto& mapRef = incoming ? p.inSources : p.outDestinations;
    auto it = mapRef.find(token);
    if (it == mapRef.end()) return false;
    for (const auto& addr : it->second) {
        if (p.swapPools.count(addr)) return true;
    }
    return false;
}


struct GraphPathResult {
    bool found = false;
    bool ambiguous = false;
    cpp_int amount = 0;
    std::vector<size_t> edges;
};

bool isGraphTerminal(const std::string& address) {
    return address.empty() || address == ZERO_ADDR || address == DEAD_ADDR;
}

void dfsTransferPath(const ParsedReceipt& parsed,
                     const std::string& token,
                     const std::string& current,
                     const std::set<std::string>& targets,
                     size_t minLogIndex,
                     int depth,
                     std::set<std::string>& visitedAddresses,
                     std::vector<size_t>& path,
                     std::vector<std::vector<size_t>>& matches) {
    if (depth > 6 || matches.size() > 4) return;
    if (targets.count(current)) {
        matches.push_back(path);
        return;
    }
    if (isGraphTerminal(current) || visitedAddresses.count(current)) return;

    visitedAddresses.insert(current);
    for (size_t i = 0; i < parsed.transfers.size(); ++i) {
        const TransferEdge& edge = parsed.transfers[i];
        if (edge.token != token || edge.from != current || edge.logIndex < minLogIndex) continue;
        if (isGraphTerminal(edge.to)) continue;

        path.push_back(i);
        dfsTransferPath(
            parsed, token, edge.to, targets, edge.logIndex + 1,
            depth + 1, visitedAddresses, path, matches
        );
        path.pop_back();
    }
    visitedAddresses.erase(current);
}

GraphPathResult findTransferPath(const ParsedReceipt& parsed,
                                 const std::string& token,
                                 const std::set<std::string>& starts,
                                 const std::set<std::string>& targets) {
    GraphPathResult result;
    std::vector<std::vector<size_t>> matches;

    for (const std::string& start : starts) {
        if (isGraphTerminal(start)) continue;
        std::set<std::string> visited;
        std::vector<size_t> path;
        dfsTransferPath(parsed, token, start, targets, 0, 0, visited, path, matches);
        if (matches.size() > 4) break;
    }

    if (matches.empty()) return result;
    result.found = true;
    result.ambiguous = matches.size() > 1;

    // Prefer the shortest route. Its limiting amount is the minimum edge amount.
    const auto best = std::min_element(
        matches.begin(), matches.end(),
        [](const auto& a, const auto& b) { return a.size() < b.size(); }
    );
    result.edges = *best;

    if (!result.edges.empty()) {
        result.amount = parsed.transfers[result.edges.front()].amount;
        for (size_t edgeIndex : result.edges)
            result.amount = std::min(result.amount, parsed.transfers[edgeIndex].amount);
    }
    return result;
}

GraphPathResult walletToPoolPath(const ParsedReceipt& parsed,
                                 const std::string& token,
                                 const std::string& wallet) {
    return findTransferPath(parsed, token, {wallet}, parsed.swapPools);
}

GraphPathResult poolToRecipientsPath(const ParsedReceipt& parsed,
                                     const std::string& token,
                                     const std::set<std::string>& recipients) {
    return findTransferPath(parsed, token, parsed.swapPools, recipients);
}

bool graphMatchesPool(const ParsedReceipt& parsed,
                      const std::string& token,
                      const std::string& wallet,
                      bool incoming,
                      const std::set<std::string>& recipients,
                      GraphPathResult* detail = nullptr) {
    GraphPathResult path = incoming
        ? poolToRecipientsPath(parsed, token, recipients)
        : walletToPoolPath(parsed, token, wallet);
    if (detail) *detail = path;
    return path.found;
}

DecodedIntent decodeV2(const json& tx, const AbiReader& abi,
                       const std::string& router, const std::string& protocol) {
    DecodedIntent d;
    d.selector = abi.selector();
    d.router = router;
    d.protocol = protocol;
    if (tx.contains("value") && tx["value"].is_string())
        d.nativeValue = hexToCppInt(tx["value"].get<std::string>());

    auto decodePath = [&](size_t offsetWord) {
        if (!abi.readAddressArray(offsetWord, d.path)) return false;
        if (d.path.size() < 2) return false;
        d.tokenIn = d.path.front();
        d.tokenOut = d.path.back();
        return true;
    };

    if (d.selector == "7ff36ab5" || d.selector == "b6f9de95") {
        d.functionName = d.selector == "7ff36ab5"
            ? "swapExactETHForTokens"
            : "swapExactETHForTokensSupportingFeeOnTransferTokens";
        d.operation = TxOperation::SWAP_EXACT_IN;
        d.swap = d.exactInput = true;
        d.tokenIn = chainCtx().nativeMarker;
        abi.readUint(0, d.amountOutMin);
        decodePath(1);
        abi.readAddress(2, d.recipient);
        if (!d.path.empty()) d.tokenOut = d.path.back();
        d.amountIn = d.nativeValue;
        d.valid = !d.tokenOut.empty();
        return d;
    }

    if (d.selector == "fb3bdb41") {
        d.functionName = "swapETHForExactTokens";
        d.operation = TxOperation::SWAP_EXACT_OUT;
        d.swap = d.exactOutput = true;
        d.tokenIn = chainCtx().nativeMarker;
        abi.readUint(0, d.amountOut);
        decodePath(1);
        abi.readAddress(2, d.recipient);
        if (!d.path.empty()) d.tokenOut = d.path.back();
        d.amountInMax = d.nativeValue;
        d.valid = !d.tokenOut.empty();
        return d;
    }

    if (d.selector == "38ed1739" || d.selector == "5c11d795") {
        d.functionName = d.selector == "38ed1739"
            ? "swapExactTokensForTokens"
            : "swapExactTokensForTokensSupportingFeeOnTransferTokens";
        d.operation = TxOperation::SWAP_EXACT_IN;
        d.swap = d.exactInput = true;
        abi.readUint(0, d.amountIn);
        abi.readUint(1, d.amountOutMin);
        decodePath(2);
        abi.readAddress(3, d.recipient);
        d.valid = !d.tokenIn.empty() && !d.tokenOut.empty();
        return d;
    }

    if (d.selector == "8803dbee") {
        d.functionName = "swapTokensForExactTokens";
        d.operation = TxOperation::SWAP_EXACT_OUT;
        d.swap = d.exactOutput = true;
        abi.readUint(0, d.amountOut);
        abi.readUint(1, d.amountInMax);
        decodePath(2);
        abi.readAddress(3, d.recipient);
        d.valid = !d.tokenIn.empty() && !d.tokenOut.empty();
        return d;
    }

    if (d.selector == "18cbafe5" || d.selector == "791ac947") {
        d.functionName = d.selector == "18cbafe5"
            ? "swapExactTokensForETH"
            : "swapExactTokensForETHSupportingFeeOnTransferTokens";
        d.operation = TxOperation::SWAP_EXACT_IN;
        d.swap = d.exactInput = true;
        abi.readUint(0, d.amountIn);
        abi.readUint(1, d.amountOutMin);
        decodePath(2);
        abi.readAddress(3, d.recipient);
        if (!d.path.empty()) d.tokenIn = d.path.front();
        d.tokenOut = chainCtx().nativeMarker;
        d.valid = !d.tokenIn.empty();
        return d;
    }

    if (d.selector == "4a25d94a") {
        d.functionName = "swapTokensForExactETH";
        d.operation = TxOperation::SWAP_EXACT_OUT;
        d.swap = d.exactOutput = true;
        abi.readUint(0, d.amountOut);
        abi.readUint(1, d.amountInMax);
        decodePath(2);
        abi.readAddress(3, d.recipient);
        if (!d.path.empty()) d.tokenIn = d.path.front();
        d.tokenOut = chainCtx().nativeMarker;
        d.valid = !d.tokenIn.empty();
        return d;
    }

    return d;
}

DecodedIntent decodeV3(const json& tx, const AbiReader& abi,
                       const std::string& router, const std::string& protocol) {
    DecodedIntent d;
    d.selector = abi.selector();
    d.router = router;
    d.protocol = protocol;
    if (tx.contains("value") && tx["value"].is_string())
        d.nativeValue = hexToCppInt(tx["value"].get<std::string>());

    // SwapRouter exactInputSingle((address,address,uint24,address,uint256,uint256,uint256,uint160))
    if (d.selector == "414bf389") {
        d.functionName = "exactInputSingle";
        d.operation = TxOperation::SWAP_EXACT_IN;
        d.swap = d.exactInput = true;
        abi.readAddress(0, d.tokenIn);
        abi.readAddress(1, d.tokenOut);
        abi.readAddress(3, d.recipient);
        abi.readUint(5, d.amountIn);
        abi.readUint(6, d.amountOutMin);
        d.path = {d.tokenIn, d.tokenOut};
        d.valid = !d.tokenIn.empty() && !d.tokenOut.empty();
        return d;
    }

    // SwapRouter02 exactInputSingle((address,address,uint24,address,uint256,uint256,uint160))
    if (d.selector == "04e45aaf") {
        d.functionName = "exactInputSingle";
        d.operation = TxOperation::SWAP_EXACT_IN;
        d.swap = d.exactInput = true;
        abi.readAddress(0, d.tokenIn);
        abi.readAddress(1, d.tokenOut);
        abi.readAddress(3, d.recipient);
        abi.readUint(4, d.amountIn);
        abi.readUint(5, d.amountOutMin);
        d.path = {d.tokenIn, d.tokenOut};
        d.valid = !d.tokenIn.empty() && !d.tokenOut.empty();
        return d;
    }

    // exactOutputSingle legacy/router02
    if (d.selector == "db3e2198" || d.selector == "5023b4df") {
        d.functionName = "exactOutputSingle";
        d.operation = TxOperation::SWAP_EXACT_OUT;
        d.swap = d.exactOutput = true;
        abi.readAddress(0, d.tokenIn);
        abi.readAddress(1, d.tokenOut);
        abi.readAddress(3, d.recipient);

        const bool legacy = d.selector == "db3e2198";
        abi.readUint(legacy ? 5 : 4, d.amountOut);
        abi.readUint(legacy ? 6 : 5, d.amountInMax);
        d.path = {d.tokenIn, d.tokenOut};
        d.valid = !d.tokenIn.empty() && !d.tokenOut.empty();
        return d;
    }

    // exactInput((bytes,address,uint256,uint256,uint256)) legacy
    if (d.selector == "c04b8d59") {
        d.functionName = "exactInput";
        d.operation = TxOperation::SWAP_EXACT_IN;
        d.swap = d.exactInput = true;
        std::string pathBytes;
        abi.readTupleBytes(0, 0, pathBytes);
        abi.readTupleAddress(0, 1, d.recipient);
        abi.readTupleUint(0, 3, d.amountIn);
        abi.readTupleUint(0, 4, d.amountOutMin);
        d.path = decodeV3Path(pathBytes, false);
        if (!d.path.empty()) {
            d.tokenIn = d.path.front();
            d.tokenOut = d.path.back();
        }
        d.valid = d.path.size() >= 2;
        return d;
    }

    // exactInput((bytes,address,uint256,uint256)) Router02
    if (d.selector == "b858183f") {
        d.functionName = "exactInput";
        d.operation = TxOperation::SWAP_EXACT_IN;
        d.swap = d.exactInput = true;
        std::string pathBytes;
        abi.readTupleBytes(0, 0, pathBytes);
        abi.readTupleAddress(0, 1, d.recipient);
        abi.readTupleUint(0, 2, d.amountIn);
        abi.readTupleUint(0, 3, d.amountOutMin);
        d.path = decodeV3Path(pathBytes, false);
        if (!d.path.empty()) {
            d.tokenIn = d.path.front();
            d.tokenOut = d.path.back();
        }
        d.valid = d.path.size() >= 2;
        return d;
    }

    // exactOutput legacy / Router02
    if (d.selector == "f28c0498" || d.selector == "09b81346") {
        d.functionName = "exactOutput";
        d.operation = TxOperation::SWAP_EXACT_OUT;
        d.swap = d.exactOutput = true;
        std::string pathBytes;
        abi.readTupleBytes(0, 0, pathBytes);
        abi.readTupleAddress(0, 1, d.recipient);

        const bool legacy = d.selector == "f28c0498";
        abi.readTupleUint(0, legacy ? 3 : 2, d.amountOut);
        abi.readTupleUint(0, legacy ? 4 : 3, d.amountInMax);

        d.path = decodeV3Path(pathBytes, true);
        if (!d.path.empty()) {
            d.tokenIn = d.path.front();
            d.tokenOut = d.path.back();
        }
        d.valid = d.path.size() >= 2;
        return d;
    }

    return d;
}


struct V4ActionSummary {
    bool valid = false;
    bool hasSwap = false;
    bool exactInput = false;
    bool exactOutput = false;
    bool hasAddLiquidity = false;
    bool hasRemoveLiquidity = false;
    bool hasWrap = false;
    bool hasUnwrap = false;
    std::string settleToken;
    std::string takeToken;
};

V4ActionSummary decodeV4ActionsPayload(const std::string& payload) {
    V4ActionSummary summary;
    if (!isHexString(payload)) return summary;

    // V4_SWAP input is abi.encode(bytes actions, bytes[] params), without a selector.
    AbiReader abi("0x00000000" + payload.substr(2));
    std::string actions;
    std::vector<std::string> params;
    if (!abi.readBytes(0, actions, 4096) ||
        !abi.readBytesArray(1, params, 256, 1 << 20)) {
        return summary;
    }

    summary.valid = true;
    const std::string actionHex = actions.substr(2);
    const size_t count = std::min(params.size(), actionHex.size() / 2);

    for (size_t i = 0; i < count; ++i) {
        const int hi = hexNibble(actionHex[i * 2]);
        const int lo = hexNibble(actionHex[i * 2 + 1]);
        if (hi < 0 || lo < 0) continue;
        const int action = (hi << 4) | lo;

        // Official Uniswap v4 Actions constants:
        // 0x00..0x05 liquidity, 0x06..0x09 swaps,
        // 0x0b..0x0d settle, 0x0e..0x11 take, 0x15/0x16 wrap/unwrap.
        if (action == 0x00 || action == 0x02 ||
            action == 0x04 || action == 0x05) {
            summary.hasAddLiquidity = true;
        } else if (action == 0x01 || action == 0x03) {
            summary.hasRemoveLiquidity = true;
        } else if (action == 0x06 || action == 0x07) {
            summary.hasSwap = true;
            summary.exactInput = true;
        } else if (action == 0x08 || action == 0x09) {
            summary.hasSwap = true;
            summary.exactOutput = true;
        } else if (action == 0x15) {
            summary.hasWrap = true;
        } else if (action == 0x16) {
            summary.hasUnwrap = true;
        }

        // Settlement/take actions expose the currency as their first ABI word.
        if (action == 0x0b || action == 0x0c || action == 0x0d) {
            AbiReader p("0x00000000" + params[i].substr(2));
            std::string token;
            if (p.readAddress(0, token)) summary.settleToken = token;
        } else if (action == 0x0e || action == 0x0f ||
                   action == 0x10 || action == 0x11 ||
                   action == 0x14) {
            AbiReader p("0x00000000" + params[i].substr(2));
            std::string token;
            if (p.readAddress(0, token)) summary.takeToken = token;
        }
    }

    return summary;
}

DecodedIntent decodeLiquidity(const json& tx, const AbiReader& abi,
                              const std::string& router, const std::string& protocol);

DecodedIntent decodeUniversal(const json& tx, const AbiReader& abi,
                              const std::string& router, const std::string& protocol,
                              int depth) {
    DecodedIntent d;
    if (abi.selector() != "3593564c" && abi.selector() != "24856bc3") return d;

    d.valid = true;
    d.selector = abi.selector();
    d.functionName = "execute";
    d.router = router;
    d.protocol = protocol;
    d.universalRouter = true;

    if (tx.contains("value") && tx["value"].is_string())
        d.nativeValue = hexToCppInt(tx["value"].get<std::string>());

    UniversalRouterCommands summary;
    std::string commands;
    std::vector<std::string> inputs;
    if (!abi.readBytes(0, commands, 4096) || !abi.readBytesArray(1, inputs, 256, 1 << 20)) {
        d.malformed = true;
        return d;
    }

    const std::string commandHex = commands.substr(2);
    const size_t count = std::min(inputs.size(), commandHex.size() / 2);

    for (size_t i = 0; i < count; ++i) {
        int hi = hexNibble(commandHex[i * 2]);
        int lo = hexNibble(commandHex[i * 2 + 1]);
        if (hi < 0 || lo < 0) continue;
        const int command = ((hi << 4) | lo) & 0x7f;

        if (command == 0x00 || command == 0x01) {
            summary.hasV3Swap = true;
            d.swap = true;
            d.operation = command == 0x00 ? TxOperation::SWAP_EXACT_IN : TxOperation::SWAP_EXACT_OUT;
            d.exactInput = command == 0x00;
            d.exactOutput = command == 0x01;

            AbiReader inputAbi("0x00000000" + inputs[i].substr(2));
            // recipient, amount, amountLimit, path, payerIsUser
            inputAbi.readAddress(0, d.recipient);
            if (d.exactInput) {
                inputAbi.readUint(1, d.amountIn);
                inputAbi.readUint(2, d.amountOutMin);
            } else {
                inputAbi.readUint(1, d.amountOut);
                inputAbi.readUint(2, d.amountInMax);
            }
            std::string pathBytes;
            if (inputAbi.readBytes(3, pathBytes)) {
                d.path = decodeV3Path(pathBytes, d.exactOutput);
                if (d.path.size() >= 2) {
                    d.tokenIn = d.path.front();
                    d.tokenOut = d.path.back();
                }
            }
        } else if (command == 0x08 || command == 0x09) {
            summary.hasV2Swap = true;
            d.swap = true;
            d.operation = command == 0x08 ? TxOperation::SWAP_EXACT_IN : TxOperation::SWAP_EXACT_OUT;
            d.exactInput = command == 0x08;
            d.exactOutput = command == 0x09;

            AbiReader inputAbi("0x00000000" + inputs[i].substr(2));
            inputAbi.readAddress(0, d.recipient);
            if (d.exactInput) {
                inputAbi.readUint(1, d.amountIn);
                inputAbi.readUint(2, d.amountOutMin);
            } else {
                inputAbi.readUint(1, d.amountOut);
                inputAbi.readUint(2, d.amountInMax);
            }
            inputAbi.readAddressArray(3, d.path);
            if (d.path.size() >= 2) {
                d.tokenIn = d.path.front();
                d.tokenOut = d.path.back();
            }
        } else if (command == 0x10) {
            d.v4 = true;
            const V4ActionSummary v4 = decodeV4ActionsPayload(inputs[i]);
            d.v4ActionsDecoded = v4.valid;

            if (v4.hasSwap) {
                d.swap = true;
                d.exactInput = v4.exactInput;
                d.exactOutput = v4.exactOutput;
                d.operation = v4.exactOutput
                    ? TxOperation::SWAP_EXACT_OUT
                    : TxOperation::SWAP_EXACT_IN;

                // V4 native currency is address(0). Convert it to the chain marker.
                d.tokenIn = v4.settleToken;
                d.tokenOut = v4.takeToken;
                if (d.tokenIn == ZERO_ADDR) d.tokenIn = chainCtx().nativeMarker;
                if (d.tokenOut == ZERO_ADDR) d.tokenOut = chainCtx().nativeMarker;
                if (!d.tokenIn.empty() && !d.tokenOut.empty())
                    d.path = {d.tokenIn, d.tokenOut};
            } else if (v4.hasAddLiquidity) {
                d.liquidity = true;
                d.operation = TxOperation::ADD_LIQUIDITY;
            } else if (v4.hasRemoveLiquidity) {
                d.liquidity = true;
                d.operation = TxOperation::REMOVE_LIQUIDITY;
            } else if (v4.hasWrap) {
                d.operation = TxOperation::WRAP_NATIVE;
            } else if (v4.hasUnwrap) {
                d.operation = TxOperation::UNWRAP_NATIVE;
            }
        } else if (command == 0x11) {
            d.permit2 = true; // V3 position manager permit signal.
        } else if (command == 0x12) {
            // V3_POSITION_MANAGER_CALL; nested calldata may be mint/increase/decrease.
            AbiReader nested(inputs[i]);
            DecodedIntent child = decodeLiquidity(tx, nested, router, protocol);
            if (child.valid) {
                d.children.push_back(child);
                d.liquidity = child.liquidity;
                d.operation = child.operation;
            }
        } else if (command == 0x13) {
            d.v4 = true; // V4_INITIALIZE_POOL
        } else if (command == 0x14) {
            d.v4 = true;
            d.liquidity = true;
            d.operation = TxOperation::ADD_LIQUIDITY;
        } else if (command == 0x21 && depth < 6) {
            // EXECUTE_SUB_PLAN is abi.encode(bytes commands, bytes[] inputs).
            AbiReader sub("0x00000000" + inputs[i].substr(2));
            std::string subCommands;
            std::vector<std::string> subInputs;
            if (sub.readBytes(0, subCommands, 4096) &&
                sub.readBytesArray(1, subInputs, 256, 1 << 20)) {
                // Rebuild execute(bytes,bytes[]) calldata by replacing the dummy selector.
                // The payload itself already has correct ABI offsets.
                const std::string fakeExecute = "0x24856bc3" + inputs[i].substr(2);
                json subTx = tx;
                subTx["input"] = fakeExecute;
                AbiReader subAbi(fakeExecute);
                DecodedIntent child = decodeUniversal(
                    subTx, subAbi, router, protocol, depth + 1
                );
                if (child.valid) {
                    child.universalSubPlan = true;
                    d.children.push_back(child);
                    d.universalSubPlan = true;
                    if (child.swap) {
                        d.swap = true;
                        d.exactInput = child.exactInput;
                        d.exactOutput = child.exactOutput;
                        d.operation = child.operation;
                        d.tokenIn = child.tokenIn;
                        d.tokenOut = child.tokenOut;
                        d.path = child.path;
                        d.amountIn = child.amountIn;
                        d.amountOut = child.amountOut;
                        d.amountOutMin = child.amountOutMin;
                        d.amountInMax = child.amountInMax;
                    } else if (d.operation == TxOperation::UNKNOWN) {
                        d.operation = child.operation;
                    }
                }
            }
        } else if (command == 0x40) {
            // Current Universal Router reserves 0x40 for Across V4 deposit.
            d.bridge = true;
            d.acrossBridge = true;
            d.operation = TxOperation::BRIDGE;
            if (d.protocol.empty()) d.protocol = "Across";
        } else if (command == 0x0b) {
            summary.hasWrap = true;
            if (!d.swap) d.operation = TxOperation::WRAP_NATIVE;
        } else if (command == 0x0c) {
            summary.hasUnwrap = true;
            if (!d.swap) d.operation = TxOperation::UNWRAP_NATIVE;
        } else if (command == 0x04) {
            summary.hasSweep = true;
        } else if (command == 0x05) {
            summary.hasTransfer = true;
        } else if (command == 0x02 || command == 0x03 || command == 0x0a || command == 0x0d) {
            summary.hasPermit2 = true;
            d.permit2 = true;
        }
    }

    if (!d.swap) {
        if (summary.hasWrap) d.operation = TxOperation::WRAP_NATIVE;
        else if (summary.hasUnwrap) d.operation = TxOperation::UNWRAP_NATIVE;
        else if (summary.hasTransfer || summary.hasSweep) d.operation = TxOperation::TRANSFER;
    }

    return d;
}



std::string selectorName(const std::string& selector) {
    auto it = chainCtx().selectorNames.find(toLower(selector));
    return it == chainCtx().selectorNames.end() ? std::string() : it->second;
}

DecodedIntent decodeKnownAggregator(const json& tx, const AbiReader& abi,
                                    const std::string& router, const std::string& protocol) {
    DecodedIntent d;
    if (!chainCtx().aggregators.count(router)) return d;

    d.valid = true;
    d.aggregator = true;
    d.router = router;
    d.protocol = protocol.empty() ? "Aggregator" : protocol;
    d.selector = abi.selector();
    d.functionName = selectorName(d.selector);
    if (d.functionName.empty()) d.functionName = "aggregatorCall";

    if (tx.contains("value") && tx["value"].is_string())
        d.nativeValue = hexToCppInt(tx["value"].get<std::string>());

    if (chainCtx().aggregatorSwapSelectors.count(d.selector)) {
        d.swap = true;
        d.operation = TxOperation::SWAP_EXACT_IN;
    }
    return d;
}

DecodedIntent decodeV4Envelope(const json& tx, const AbiReader& abi,
                               const std::string& router, const std::string& protocol) {
    DecodedIntent d;
    auto info = chainCtx().routerInfo.find(router);
    const bool v4Router = info != chainCtx().routerInfo.end() && info->second.supportsV4;
    if (!v4Router) return d;

    d.valid = true;
    d.v4 = true;
    d.router = router;
    d.protocol = protocol.empty() ? "V4 Router" : protocol;
    d.selector = abi.selector();
    d.functionName = selectorName(d.selector);
    if (d.functionName.empty()) d.functionName = "v4RouterCall";

    if (tx.contains("value") && tx["value"].is_string())
        d.nativeValue = hexToCppInt(tx["value"].get<std::string>());

    // V4 routers are action-based. Until action-level token extraction is available,
    // mark swap family only for selectors registered as swaps.
    if (chainCtx().aggregatorSwapSelectors.count(d.selector)) {
        d.swap = true;
        d.operation = TxOperation::SWAP_EXACT_IN;
    }
    return d;
}

DecodedIntent decodePermit2(const json& tx, const AbiReader& abi,
                            const std::string& router, const std::string& protocol) {
    DecodedIntent d;
    d.selector = abi.selector();
    d.router = router;
    d.protocol = protocol;

    // Permit2 transferFrom(address,address,uint160,address)
    if (d.selector == "36c78516") {
        d.valid = true;
        d.permit2 = true;
        d.functionName = "permit2.transferFrom";
        d.operation = TxOperation::TRANSFER;
        abi.readAddress(0, d.recipient);     // from
        std::string to;
        abi.readAddress(1, to);
        abi.readUint(2, d.amountIn);
        abi.readAddress(3, d.tokenIn);
        d.tokenOut = d.tokenIn;
        d.secondaryToken = to;
        return d;
    }

    // Permit2 approve(address,address,uint160,uint48)
    if (d.selector == "87517c45") {
        d.valid = true;
        d.permit2 = true;
        d.functionName = "permit2.approve";
        d.operation = TxOperation::TRANSFER;
        abi.readAddress(0, d.tokenIn);
        abi.readAddress(1, d.recipient);     // spender
        abi.readUint(2, d.amountIn);
        return d;
    }

    // permit(address,PermitSingle,bytes) and permit(address,PermitBatch,bytes)
    if (d.selector == "2b67b570" || d.selector == "2a2d80d1") {
        d.valid = true;
        d.permit2 = true;
        d.functionName = d.selector == "2b67b570"
            ? "permit2.permitSingle"
            : "permit2.permitBatch";
        d.operation = TxOperation::TRANSFER;
        abi.readAddress(0, d.recipient); // owner
        return d;
    }

    // permitTransferFrom / permitWitnessTransferFrom families.
    if (d.selector == "30f28b7a" || d.selector == "137c29fe" ||
        d.selector == "edd9444b" || d.selector == "81c98a17") {
        d.valid = true;
        d.permit2 = true;
        d.functionName = "permit2.permitTransferFrom";
        d.operation = TxOperation::TRANSFER;
        return d;
    }

    return d;
}

DecodedIntent decodeLiquidity(const json& tx, const AbiReader& abi,
                              const std::string& router, const std::string& protocol) {
    DecodedIntent d;
    d.selector = abi.selector();
    d.router = router;
    d.protocol = protocol;
    if (tx.contains("value") && tx["value"].is_string())
        d.nativeValue = hexToCppInt(tx["value"].get<std::string>());

    // addLiquidity(address,address,uint256,uint256,uint256,uint256,address,uint256)
    if (d.selector == "e8e33700") {
        d.valid = true;
        d.liquidity = true;
        d.operation = TxOperation::ADD_LIQUIDITY;
        d.functionName = "addLiquidity";
        abi.readAddress(0, d.tokenIn);
        abi.readAddress(1, d.secondaryToken);
        abi.readUint(2, d.amountIn);
        abi.readUint(3, d.amountOut);
        abi.readAddress(6, d.recipient);
        d.path = {d.tokenIn, d.secondaryToken};
        return d;
    }

    // addLiquidityETH(address,uint256,uint256,uint256,address,uint256)
    if (d.selector == "f305d719") {
        d.valid = true;
        d.liquidity = true;
        d.operation = TxOperation::ADD_LIQUIDITY;
        d.functionName = "addLiquidityETH";
        abi.readAddress(0, d.tokenIn);
        d.secondaryToken = chainCtx().nativeMarker;
        abi.readUint(1, d.amountIn);
        abi.readAddress(4, d.recipient);
        d.nativeValue = d.nativeValue;
        d.path = {d.tokenIn, d.secondaryToken};
        return d;
    }

    // removeLiquidity(address,address,uint256,uint256,uint256,address,uint256)
    if (d.selector == "baa2abde") {
        d.valid = true;
        d.liquidity = true;
        d.operation = TxOperation::REMOVE_LIQUIDITY;
        d.functionName = "removeLiquidity";
        abi.readAddress(0, d.tokenIn);
        abi.readAddress(1, d.secondaryToken);
        abi.readUint(2, d.amountIn);
        abi.readAddress(5, d.recipient);
        d.path = {d.tokenIn, d.secondaryToken};
        return d;
    }

    // removeLiquidityETH / supporting fee on transfer / permit variants.
    if (d.selector == "02751cec" || d.selector == "af2979eb" ||
        d.selector == "ded9382a" || d.selector == "5b0d5984") {
        d.valid = true;
        d.liquidity = true;
        d.operation = TxOperation::REMOVE_LIQUIDITY;
        d.functionName = "removeLiquidityETH";
        abi.readAddress(0, d.tokenIn);
        d.secondaryToken = chainCtx().nativeMarker;
        abi.readUint(1, d.amountIn);
        abi.readAddress(4, d.recipient);
        d.path = {d.tokenIn, d.secondaryToken};
        return d;
    }

    // V3 NonfungiblePositionManager mint((...))
    if (d.selector == "88316456") {
        d.valid = true;
        d.liquidity = true;
        d.operation = TxOperation::ADD_LIQUIDITY;
        d.functionName = "v3.mintPosition";
        abi.readAddress(0, d.tokenIn);
        abi.readAddress(1, d.secondaryToken);
        abi.readAddress(9, d.recipient);
        abi.readUint(5, d.amountIn);
        abi.readUint(6, d.amountOut);
        d.path = {d.tokenIn, d.secondaryToken};
        return d;
    }

    // increaseLiquidity((uint256,uint256,uint256,uint256,uint256,uint256))
    if (d.selector == "219f5d17") {
        d.valid = true;
        d.liquidity = true;
        d.operation = TxOperation::ADD_LIQUIDITY;
        d.functionName = "v3.increaseLiquidity";
        abi.readUint(1, d.amountIn);
        abi.readUint(2, d.amountOut);
        return d;
    }

    // decreaseLiquidity((uint256,uint128,uint256,uint256,uint256))
    if (d.selector == "0c49ccbe") {
        d.valid = true;
        d.liquidity = true;
        d.operation = TxOperation::REMOVE_LIQUIDITY;
        d.functionName = "v3.decreaseLiquidity";
        return d;
    }

    // collect((uint256,address,uint128,uint128))
    if (d.selector == "fc6f7865") {
        d.valid = true;
        d.liquidity = true;
        d.operation = TxOperation::REMOVE_LIQUIDITY;
        d.functionName = "v3.collect";
        abi.readAddress(1, d.recipient);
        return d;
    }

    // burn(uint256)
    if (d.selector == "42966c68") {
        d.valid = true;
        d.liquidity = true;
        d.operation = TxOperation::REMOVE_LIQUIDITY;
        d.functionName = "v3.burnPosition";
        return d;
    }

    return d;
}

DecodedIntent decodeBridge(const json& tx, const AbiReader& abi,
                           const std::string& router, const std::string& protocol) {
    DecodedIntent d;
    if (!chainCtx().bridges.count(router)) return d;

    d.valid = true;
    d.bridge = true;
    d.operation = TxOperation::BRIDGE;
    d.selector = abi.selector();
    d.functionName = "bridgeCall";
    d.router = router;
    d.protocol = protocol.empty() ? "Bridge" : protocol;

    if (tx.contains("value") && tx["value"].is_string())
        d.nativeValue = hexToCppInt(tx["value"].get<std::string>());

    // Best-effort extraction for common bridge methods. Receipt remains source of truth.
    abi.readAddress(0, d.tokenIn);
    abi.readAddress(1, d.recipient);
    abi.readUint(2, d.amountIn);
    return d;
}

DecodedIntent decodeRecursive(const json& tx, const std::string& input,
                              const std::string& router, const std::string& protocol,
                              int depth) {
    DecodedIntent none;
    if (depth > 6) return none;

    AbiReader abi(input);
    if (!abi.valid()) return none;

    json nestedTx = tx;
    nestedTx["input"] = input;

    DecodedIntent d = decodeV2(nestedTx, abi, router, protocol);
    if (d.valid) return d;

    d = decodeV3(nestedTx, abi, router, protocol);
    if (d.valid) return d;

    d = decodeUniversal(nestedTx, abi, router, protocol, depth);
    if (d.valid) return d;

    d = decodePermit2(nestedTx, abi, router, protocol);
    if (d.valid) return d;

    d = decodeLiquidity(nestedTx, abi, router, protocol);
    if (d.valid) return d;

    d = decodeBridge(nestedTx, abi, router, protocol);
    if (d.valid) return d;

    d = decodeKnownAggregator(nestedTx, abi, router, protocol);
    if (d.valid && d.swap) return d;

    d = decodeV4Envelope(nestedTx, abi, router, protocol);
    if (d.valid && d.swap) return d;

    if (abi.selector() == "ac9650d8" || abi.selector() == "5ae401dc" ||
        abi.selector() == "252dba42" || abi.selector() == "82ad56cb" ||
        abi.selector() == "174dea71" || abi.selector() == "4d2301cc") {
        DecodedIntent aggregate;
        aggregate.valid = true;
        aggregate.multicall = true;
        aggregate.selector = abi.selector();
        aggregate.functionName = "multicall";
        aggregate.router = router;
        aggregate.protocol = protocol;

        std::vector<AbiReader::TargetCall> targeted;
        bool targetedDecoded = false;

        if (abi.selector() == "82ad56cb") {
            targetedDecoded = abi.readTargetBytesTupleArray(1, targeted, 128, 1 << 20);
        } else if (abi.selector() == "174dea71") {
            targetedDecoded = abi.readAggregate3TupleArray(0, targeted, 128, 1 << 20);
        } else if (abi.selector() == "4d2301cc") {
            targetedDecoded = abi.readAggregate3ValueTupleArray(0, targeted, 128, 1 << 20);
        } else if (abi.selector() == "252dba42") {
            targetedDecoded = abi.readTargetBytesTupleArray(0, targeted, 128, 1 << 20);
        }

        if (targetedDecoded) {
            aggregate.targetedMulticall = true;
            for (const auto& call : targeted) {
                json childTx = tx;
                childTx["to"] = call.target;
                childTx["input"] = call.calldata;
                if (call.value > 0)
                    childTx["value"] = "0x" + call.value.convert_to<std::string>();

                const std::string childProtocol = lookupRouterLabel(call.target);
                DecodedIntent child = decodeRecursive(
                    childTx, call.calldata, call.target, childProtocol, depth + 1
                );
                if (!child.valid) continue;
                aggregate.children.push_back(child);
                if (child.swap) {
                    child.multicall = true;
                    child.targetedMulticall = true;
                    child.children = aggregate.children;
                    return child;
                }
                if (aggregate.operation == TxOperation::UNKNOWN)
                    aggregate.operation = child.operation;
            }
            return aggregate;
        }

        size_t bytesArrayWord = abi.selector() == "5ae401dc" ? 1 : 0;
        std::vector<std::string> calls;
        if (!abi.readBytesArray(bytesArrayWord, calls, 128, 1 << 20))
            return aggregate;

        for (const auto& call : calls) {
            DecodedIntent child = decodeRecursive(tx, call, router, protocol, depth + 1);
            if (!child.valid) continue;
            aggregate.children.push_back(child);

            if (child.swap) {
                child.multicall = true;
                child.children = aggregate.children;
                return child;
            }

            if (aggregate.operation == TxOperation::UNKNOWN)
                aggregate.operation = child.operation;
        }
        return aggregate;
    }

    // Return recognized non-swap aggregator/V4 envelope after multicall probing.
    d = decodeKnownAggregator(nestedTx, abi, router, protocol);
    if (d.valid) return d;
    d = decodeV4Envelope(nestedTx, abi, router, protocol);
    if (d.valid) return d;

    return none;
}

} // namespace

cpp_int parseUint256(const std::string& h) {
    if (!isHexString(h) || h.size() < 66) return 0;
    cpp_int result = 0;
    for (char c : h.substr(2, 64)) {
        int n = hexNibble(c);
        if (n < 0) return 0;
        result <<= 4;
        result |= n;
    }
    return result;
}

cpp_int hexToCppInt(const std::string& h) {
    if (!isHexString(h)) return 0;
    cpp_int result = 0;
    for (size_t i = 2; i < h.size(); ++i) {
        int n = hexNibble(h[i]);
        if (n < 0) return 0;
        result <<= 4;
        result |= n;
    }
    return result;
}

std::string formatAmount(const cpp_int& raw, int decimals) {
    if (raw == 0) return "0.00";
    if (decimals < 0) decimals = 0;
    if (decimals > 1000) decimals = 1000;

    cpp_int divisor = 1;
    for (int i = 0; i < decimals; ++i) divisor *= 10;

    const bool negative = raw < 0;
    const cpp_int value = negative ? -raw : raw;

    std::string integerPart = (value / divisor).convert_to<std::string>();
    std::string fractionPart = (value % divisor).convert_to<std::string>();

    while (static_cast<int>(fractionPart.size()) < decimals)
        fractionPart = "0" + fractionPart;

    if (fractionPart.size() > 2) fractionPart.resize(2);
    if (fractionPart.empty()) fractionPart = "00";
    if (fractionPart.size() == 1) fractionPart += "0";

    return std::string(negative ? "-" : "") + integerPart + "." + fractionPart;
}

cpp_int calcUsdNanos(const cpp_int& raw, int decimals, uint64_t priceNanos) {
    if (!priceNanos || raw <= 0) return 0;
    cpp_int divisor = 1;
    for (int i = 0; i < decimals; ++i) divisor *= 10;
    return (raw * priceNanos) / divisor;
}

std::string formatUsd(const cpp_int& nanos) {
    bool negative = nanos < 0;
    cpp_int value = negative ? -nanos : nanos;
    std::string s = value.convert_to<std::string>();
    while (s.size() < 10) s = "0" + s;

    const std::string dollars = s.substr(0, s.size() - 9);
    const std::string cents = s.substr(s.size() - 9, 2);
    return std::string(negative ? "-$" : "$") + (dollars.empty() ? "0" : dollars) + "." + cents;
}

cpp_int calcUnitPriceNanos(const cpp_int& usdNanos, const cpp_int& rawAmount, int decimals) {
    if (rawAmount <= 0) return 0;
    cpp_int divisor = 1;
    for (int i = 0; i < decimals; ++i) divisor *= 10;
    return (usdNanos * divisor) / rawAmount;
}

std::string formatPriceUsd(const cpp_int& nanos) {
    bool negative = nanos < 0;
    cpp_int value = negative ? -nanos : nanos;
    std::string s = value.convert_to<std::string>();
    while (s.size() < 10) s = "0" + s;

    std::string dollars = s.substr(0, s.size() - 9);
    std::string fraction = s.substr(s.size() - 9);
    if (dollars.empty()) dollars = "0";

    const size_t last = fraction.find_last_not_of('0');
    size_t keep = last == std::string::npos ? 0 : last + 1;
    if (keep < 2) keep = 2;
    fraction.resize(keep);

    return std::string(negative ? "-$" : "$") + dollars + "." + fraction;
}

const std::string WBNB_ADDR = "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c";
const std::string NATIVE_BNB_MARKER = "native:bnb";

namespace {
ChainContext g_chain;
}

static void addRouter(ChainContext& c, const std::string& address, const std::string& label,
                      const RouterInfo& info = {}) {
    const std::string a = toLower(address);
    c.routers[a] = label;
    RouterInfo ri = info;
    if (ri.protocol.empty()) ri.protocol = label;
    c.routerInfo[a] = ri;
}

ChainContext makeBscContext() {
    ChainContext c;
    c.displayName = "BNB Smart Chain";
    c.explorerUrl = "https://bscscan.com";
    c.explorerName = "BscScan";
    c.nativeSymbol = "BNB";
    c.nativeMarker = "native:bnb";
    c.wrappedNative = WBNB_ADDR;
    c.stablecoins = {
        "0x55d398326f99059ff775485246999027b3197955",
        "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d",
        "0xe9e7cea3dedca5984780bafc599bd69add087d56"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(c.wrappedNative);
    c.baseAssets.insert("0xc5f0f7b66764f6ec8c8dff7ba683102295e16409");

    addRouter(c, "0x10ed43c718714eb63d5aa57b78b54704e256024e", "PancakeSwap V2",
              {"PancakeSwap", "V2", "V2", true});
    addRouter(c, "0x13f4ea83d0bd40e75c8222255bc855a974568dd4", "PancakeSwap V3 (Smart Router)",
              {"PancakeSwap", "V3", "V3", false, true, false, false, true});
    addRouter(c, "0x1b81d678ffb9c0263b24a97847620c99d213eb14", "PancakeSwap V3 (Swap Router)",
              {"PancakeSwap", "V3", "V3", false, true});
    addRouter(c, "0x1a0a18ac4becddbd6389559687d1a73d8927e416", "PancakeSwap (Universal Router)",
              {"PancakeSwap", "Universal", "Universal", true, true, false, true, true, true});
    addRouter(c, "0xd9c500dff816a1da21a48a732d3498bf09dc9aeb", "PancakeSwap (Universal Router 2)",
              {"PancakeSwap", "Universal 2", "Universal", true, true, true, true, true, true});
    addRouter(c, "0x1111111254eeb25477b68fb85ed929f73a960582", "1inch",
              {"1inch", "", "Aggregator"});
    addRouter(c, "0x9333c74bdd1e118634fe5664aca7a9710b108bab", "OKX DEX",
              {"OKX", "", "Aggregator"});
    addRouter(c, "0x6015126d7d23648c2e4466693b8deab005ffaba8", "OKX DEX",
              {"OKX", "", "Aggregator"});
    addRouter(c, "0x6131b5fae19ea4f9d964eac0408e4408b66337b5", "KyberSwap",
              {"KyberSwap", "", "Aggregator"});
    addRouter(c, "0xdf1a1b60f2d438842916c0adc43748768353ec25", "KyberSwap",
              {"KyberSwap", "", "Aggregator"});
    addRouter(c, "0x6352a56caadc4f1e25cd6c75970fa768a3304e64", "OpenOcean",
              {"OpenOcean", "", "Aggregator"});
    addRouter(c, "0x9f138be5aa5cc442ea7cc7d18cd9e30593ed90b9", "Odos",
              {"Odos", "", "Aggregator"});

    for (const auto& [addr, info] : c.routerInfo)
        if (info.routerType == "Aggregator") c.aggregators.insert(addr);

    c.selectorNames = {
        {"12aa3caf", "1inch.swap"},
        {"0502b1c5", "1inch.unoswap"},
        {"f78dc253", "1inch.unoswapTo"},
        {"e449022e", "1inch.unoswapTo2"},
        {"415565b0", "0x.transformERC20"},
        {"3593564c", "UniversalRouter.execute"},
        {"24856bc3", "UniversalRouter.execute"},
        {"ac9650d8", "multicall(bytes[])"},
        {"5ae401dc", "multicall(uint256,bytes[])"},
        {"82ad56cb", "tryAggregate"},
        {"174dea71", "aggregate3"},
        {"4d2301cc", "aggregate3Value"},
        {"24856bc3", "UniversalRouter.execute"},
        {"3593564c", "UniversalRouter.execute"}
    };
    c.aggregatorSwapSelectors = {
        "12aa3caf", "0502b1c5", "f78dc253", "e449022e", "415565b0"
    };

    return c;
}

ChainContext makeEthereumContext() {
    ChainContext c;
    c.displayName = "Ethereum";
    c.explorerUrl = "https://etherscan.io";
    c.explorerName = "Etherscan";
    c.nativeSymbol = "ETH";
    c.nativeMarker = "native:eth";
    c.wrappedNative = "0xc02aaa39b223fe8d0a0e5c4f27ead9083c756cc2";
    c.stablecoins = {
        "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
        "0xdac17f958d2ee523a2206206994597c13d831ec7",
        "0x6b175474e89094c44da98b954eedeac495271d0f"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(c.wrappedNative);

    addRouter(c, "0x7a250d5630b4cf539739df2c5dacb4c659f2488d", "Uniswap V2",
              {"Uniswap", "V2", "V2", true});
    addRouter(c, "0xe592427a0aece92de3edee1f18e0157c05861564", "Uniswap V3",
              {"Uniswap", "V3", "V3", false, true});
    addRouter(c, "0xef1c6e67703c7bd7107eed8303fbe6ec2554bf6b", "Uniswap (Universal Router)",
              {"Uniswap", "Universal", "Universal", true, true, false, true, true, true});
    addRouter(c, "0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)",
              {"Uniswap", "Universal", "Universal", true, true, false, true, true, true});
    addRouter(c, "0x66a9893cc07d91d95644aedd05d03f95e1dba8af", "Uniswap V4 (Universal Router)",
              {"Uniswap", "V4", "Universal", false, false, true, true, true, true});

    c.selectorNames = {
        {"3593564c", "UniversalRouter.execute"},
        {"24856bc3", "UniversalRouter.execute"},
        {"ac9650d8", "multicall(bytes[])"},
        {"5ae401dc", "multicall(uint256,bytes[])"},
        {"82ad56cb", "tryAggregate"},
        {"174dea71", "aggregate3"},
        {"4d2301cc", "aggregate3Value"},
        {"24856bc3", "UniversalRouter.execute"},
        {"3593564c", "UniversalRouter.execute"}
    };
    return c;
}

ChainContext makeBaseContext() {
    ChainContext c;
    c.displayName = "Base";
    c.explorerUrl = "https://basescan.org";
    c.explorerName = "BaseScan";
    c.nativeSymbol = "ETH";
    c.nativeMarker = "native:eth";
    c.wrappedNative = "0x4200000000000000000000000000000000000006";
    c.stablecoins = {
        "0x833589fcd6edb6e08f4c7c32d4f71b54bda02913",
        "0xfde4c96c8593536e31f229ea8f37b2ada2699bb2",
        "0x50c5725949a6f0c72e6c4a641f24049a917db0cb"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(c.wrappedNative);

    addRouter(c, "0x198ef79f1f515f02dfe9e3115ed9fc07183f02fc", "Uniswap (Universal Router)",
              {"Uniswap", "Universal", "Universal", true, true, false, true, true, true});
    addRouter(c, "0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)",
              {"Uniswap", "Universal", "Universal", true, true, false, true, true, true});
    addRouter(c, "0x6ff5693b99212da76ad316178a184ab56d299b43", "Uniswap V4 (Universal Router)",
              {"Uniswap", "V4", "Universal", false, false, true, true, true, true});

    c.selectorNames = {
        {"3593564c", "UniversalRouter.execute"},
        {"24856bc3", "UniversalRouter.execute"},
        {"ac9650d8", "multicall(bytes[])"},
        {"5ae401dc", "multicall(uint256,bytes[])"},
        {"82ad56cb", "tryAggregate"},
        {"174dea71", "aggregate3"},
        {"4d2301cc", "aggregate3Value"},
        {"24856bc3", "UniversalRouter.execute"},
        {"3593564c", "UniversalRouter.execute"}
    };
    return c;
}

ChainContext makeArbitrumContext() {
    ChainContext c;
    c.displayName = "Arbitrum One";
    c.explorerUrl = "https://arbiscan.io";
    c.explorerName = "Arbiscan";
    c.nativeSymbol = "ETH";
    c.nativeMarker = "native:eth";
    c.wrappedNative = "0x82af49447d8a07e3bd95bd0d56f35241523fbab1";
    c.stablecoins = {
        "0xaf88d065e77c8cc2239327c5edb3a432268e5831",
        "0xfd086bc7cd5c481dcc9c85ebe478a1c0b69fcbb9",
        "0xda10009cbd5d07dd0cecc66161fc93d7c9000da1"
    };
    c.baseAssets = c.stablecoins;
    c.baseAssets.insert(c.wrappedNative);

    addRouter(c, "0x4c60051384bd2d3c01bfc845cf5f4b44bcbe9de5", "Uniswap (Universal Router)",
              {"Uniswap", "Universal", "Universal", true, true, false, true, true, true});
    addRouter(c, "0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad", "Uniswap (Universal Router)",
              {"Uniswap", "Universal", "Universal", true, true, false, true, true, true});
    addRouter(c, "0xe592427a0aece92de3edee1f18e0157c05861564", "Uniswap V3",
              {"Uniswap", "V3", "V3", false, true});
    addRouter(c, "0x68b3465833fb72a70ecdf485e0e4c7bd8665fc45", "Uniswap V3 (Router 2)",
              {"Uniswap", "V3 Router02", "V3", false, true, false, false, true});

    c.selectorNames = {
        {"3593564c", "UniversalRouter.execute"},
        {"24856bc3", "UniversalRouter.execute"},
        {"ac9650d8", "multicall(bytes[])"},
        {"5ae401dc", "multicall(uint256,bytes[])"},
        {"82ad56cb", "tryAggregate"},
        {"174dea71", "aggregate3"},
        {"4d2301cc", "aggregate3Value"},
        {"24856bc3", "UniversalRouter.execute"},
        {"3593564c", "UniversalRouter.execute"}
    };
    return c;
}

const ChainContext& chainCtx() {
    if (g_chain.displayName.empty()) g_chain = makeBscContext();
    return g_chain;
}

void setChainContext(const ChainContext& ctx) {
    g_chain = ctx;
}

bool isBaseAsset(const std::string& a) {
    return chainCtx().baseAssets.count(toLower(a)) > 0;
}

bool isStablecoin(const std::string& a) {
    return chainCtx().stablecoins.count(toLower(a)) > 0;
}

bool isNativeAsset(const std::string& a) {
    return toLower(a) == toLower(chainCtx().nativeMarker);
}

bool isQuoteAsset(const std::string& a) {
    return isNativeAsset(a) || isBaseAsset(a);
}

std::string lookupRouterLabel(const std::string& addr) {
    auto it = chainCtx().routers.find(toLower(addr));
    return it == chainCtx().routers.end() ? std::string() : it->second;
}

DecodedIntent decodeTransactionInput(const json& tx) {
    DecodedIntent none;
    if (!tx.is_object() || !tx.contains("input") || !tx["input"].is_string()) return none;

    const std::string input = tx["input"].get<std::string>();
    if (!isHexString(input) || input.size() < 10) return none;

    std::string router;
    if (tx.contains("to") && !tx["to"].is_null() && tx["to"].is_string())
        router = toLower(tx["to"].get<std::string>());

    const std::string protocol = lookupRouterLabel(router);
    DecodedIntent decoded = decodeRecursive(tx, input, router, protocol, 0);

    if (!decoded.valid && (!protocol.empty() || chainCtx().aggregators.count(router))) {
        AbiReader abi(input);
        decoded.valid = true;
        decoded.selector = abi.selector();
        decoded.functionName = "known_protocol_call";
        decoded.router = router;
        decoded.protocol = protocol;
    }

    return decoded;
}

TxResult analyzeTx(const json& tx, const json& receipt, const std::string& walletAddress) {
    TxResult result;
    const std::string wallet = toLower(walletAddress);

    ParsedReceipt parsed = parseReceipt(receipt, wallet);
    if (!parsed.valid) return result;

    DecodedIntent decoded = decodeTransactionInput(tx);

    auto applyDecoded = [&](TxResult& r) {
        r.calldataDecoded = decoded.valid;
        r.calldataSwap = decoded.swap;
        r.decodedSelector = decoded.selector;
        r.decodedFunction = decoded.functionName;
        r.decodedTokenIn = decoded.tokenIn;
        r.decodedTokenOut = decoded.tokenOut;
        if (decoded.universalRouter) r.isUniversalRouter = true;
        if (decoded.multicall) r.isGenericMulticall = true;
        if (decoded.permit2) {
            r.hasPermit2Signal = true;
            r.permit2Decoded = true;
        }
        if (decoded.liquidity) r.liquidityDecoded = true;
        if (decoded.bridge) r.bridgeDecoded = true;
        if (decoded.aggregator) r.aggregatorDecoded = true;
        if (decoded.v4) r.v4Decoded = true;
        if (decoded.targetedMulticall) r.targetedMulticallDecoded = true;
        if (decoded.universalSubPlan) r.universalSubPlanDecoded = true;
        if (decoded.acrossBridge) r.acrossBridgeDecoded = true;
        if (decoded.v4ActionsDecoded) r.v4ActionsDecoded = true;
    };

    std::string txTo;
    bool walletIsSender = false;
    cpp_int nativeOut = 0;

    if (tx.is_object()) {
        if (tx.contains("to") && !tx["to"].is_null() && tx["to"].is_string())
            txTo = toLower(tx["to"].get<std::string>());

        if (tx.contains("from") && tx["from"].is_string() &&
            toLower(tx["from"].get<std::string>()) == wallet) {
            walletIsSender = true;
            if (tx.contains("value") && tx["value"].is_string())
                nativeOut = hexToCppInt(tx["value"].get<std::string>());
        }
    }

    UniversalRouterCommands ur = {};
    if (tx.contains("input") && tx["input"].is_string())
        ur = parseExecuteCommands(tx["input"].get<std::string>());

    const bool knownRouter = !txTo.empty() && !lookupRouterLabel(txTo).empty();
    const bool routerCall = knownRouter || decoded.valid || ur.present || isGenericMulticallSelector(
        tx.contains("input") && tx["input"].is_string() ? tx["input"].get<std::string>() : "");

    const bool nativeOutflow = walletIsSender && nativeOut > 0;
    const bool nativeSwapSignal = nativeOutflow &&
        (routerCall || parsed.hasSwapEvent || parsed.wrappedNative > 0);

    cpp_int nativeIn = 0;
    if (walletIsSender && parsed.hasSwapEvent && parsed.unwrappedNative > 0)
        nativeIn = parsed.unwrappedNative;
    const bool nativeInflowSignal = nativeIn > 0;

    if (!parsed.anyWalletTransfer) {
        if (walletIsSender && (parsed.wrappedNative > 0 || parsed.unwrappedNative > 0)) {
            result.valid = true;
            result.isSwap = false;
            result.tokenAddr = chainCtx().wrappedNative;

            if (parsed.wrappedNative > 0) {
                result.venue = "Wrap";
                result.rawAmount = parsed.wrappedNative;
                result.isBuy = true;
            } else {
                result.venue = "Unwrap";
                result.rawAmount = parsed.unwrappedNative;
                result.isBuy = false;
            }

            result.usdNanos = calcUsdNanos(
                result.rawAmount,
                getDecimals(result.tokenAddr),
                getPriceNanos(result.tokenAddr)
            );
            result.hasSwapEvent = parsed.hasSwapEvent;
            result.dexActivityDetected = parsed.hasSwapEvent || routerCall;
            applyDecoded(result);
            return result;
        }
        return result;
    }

    result.valid = true;
    result.hasSwapEvent = parsed.hasSwapEvent;
    result.isUniversalRouter = ur.present;
    result.isGenericMulticall = decoded.multicall;
    result.hasPermit2Signal = ur.hasPermit2 || decoded.permit2;
    result.dexActivityDetected = parsed.hasSwapEvent || routerCall;

    for (const auto& pool : parsed.swapPools) {
        result.venue = lookupRouterLabel(pool);
        if (!result.venue.empty()) break;
    }
    if (result.venue.empty() && !parsed.firstCounterparty.empty())
        result.venue = lookupRouterLabel(parsed.firstCounterparty);
    if (result.venue.empty() && !txTo.empty())
        result.venue = lookupRouterLabel(txTo);
    if (result.venue.empty() && !decoded.protocol.empty())
        result.venue = decoded.protocol;
    if (result.venue.empty() && ur.present) {
        if (ur.hasV3Swap) result.venue = "Universal Router (V3-style)";
        else if (ur.hasV2Swap) result.venue = "Universal Router (V2-style)";
        else result.venue = "Universal Router";
    }
    if (result.venue.empty() && parsed.hasSwapEvent) {
        if (parsed.firstSwapDataHexLen == 256) result.venue = "unknown pool (V2-style)";
        else if (parsed.firstSwapDataHexLen == 320 || parsed.firstSwapDataHexLen == 448)
            result.venue = "unknown pool (V3-style)";
    }

    bool anyIn = false;
    bool anyOut = false;
    bool sentBase = false;
    bool sentNonBase = false;
    bool gotBase = false;
    bool gotNonBase = false;
    bool hasBaseIn = false;
    bool hasBaseOut = false;

    for (const auto& token : parsed.tokenOrder) {
        const cpp_int net = parsed.netFlow[token];
        if (net > 0) anyIn = true;
        if (net < 0) anyOut = true;

        if (isBaseAsset(token)) {
            if (net > 0) gotBase = hasBaseIn = true;
            if (net < 0) sentBase = hasBaseOut = true;
        } else {
            if (net > 0) gotNonBase = true;
            if (net < 0) sentNonBase = true;
        }
    }

    const bool twoSidedFlow = anyIn && anyOut;

    bool lpAdd = false;
    bool lpRemove = false;

    for (const auto& token : parsed.tokenOrder) {
        const cpp_int net = parsed.netFlow[token];
        if (net > 0 && parsed.mintedTokens.count(token) && sentBase && sentNonBase)
            lpAdd = true;
        if (net < 0 && parsed.burnedTokens.count(token) && gotBase && gotNonBase)
            lpRemove = true;
    }

    if (parsed.v3Increase) lpAdd = true;
    if (parsed.v3Decrease || parsed.v3Collect) lpRemove = true;

    // Calldata may identify LP intent even when NFT position transfers or pool-side
    // token movements do not touch the wallet symmetrically.
    if (decoded.valid && decoded.liquidity) {
        if (decoded.operation == TxOperation::ADD_LIQUIDITY) lpAdd = true;
        if (decoded.operation == TxOperation::REMOVE_LIQUIDITY) lpRemove = true;
    }

    result.lpMintOrBurnSeen = !parsed.mintedTokens.empty() || !parsed.burnedTokens.empty();
    result.lpV3EventSeen = parsed.v3Increase || parsed.v3Decrease || parsed.v3Collect;
    result.lpPoolIdentitySeen = false;
    for (const auto& token : parsed.tokenOrder) {
        const cpp_int net = parsed.netFlow[token];
        if (net == 0) continue;

        GraphPathResult poolPath;
        const bool directPool = sourceMatchesPool(parsed, token, net > 0);
        const bool graphPool = graphMatchesPool(
            parsed, token, wallet, net > 0, {wallet}, &poolPath
        );

        if (directPool || graphPool) {
            result.lpPoolIdentitySeen = true;
            break;
        }
    }

    std::string bestNonBase;
    cpp_int bestAbs = -1;
    cpp_int bestNet = 0;
    bool bestCoherent = false;
    bool bestPoolRelated = false;

    for (const auto& token : parsed.tokenOrder) {
        if (isBaseAsset(token)) continue;
        const cpp_int net = parsed.netFlow[token];
        if (net == 0) continue;

        const cpp_int magnitude = absInt(net);
        const bool coherent =
            (net > 0 && (hasBaseOut || nativeSwapSignal)) ||
            (net < 0 && (hasBaseIn || nativeInflowSignal));
        const bool poolRelated =
            sourceMatchesPool(parsed, token, net > 0) ||
            graphMatchesPool(parsed, token, wallet, net > 0, {wallet});

        const bool better =
            bestNonBase.empty() ||
            (coherent && !bestCoherent) ||
            (coherent == bestCoherent && poolRelated && !bestPoolRelated) ||
            (coherent == bestCoherent && poolRelated == bestPoolRelated && magnitude > bestAbs);

        if (better) {
            bestNonBase = token;
            bestAbs = magnitude;
            bestNet = net;
            bestCoherent = coherent;
            bestPoolRelated = poolRelated;
        }
    }

    result.isSwap =
        twoSidedFlow ||
        (!bestNonBase.empty() && (hasBaseIn || hasBaseOut || nativeSwapSignal || nativeInflowSignal)) ||
        (hasBaseIn && hasBaseOut);

    if (lpAdd || lpRemove) {
        result.isSwap = false;
        result.venue = lpAdd ? "Add Liquidity" : "Remove Liquidity";
    }

    if (!bestNonBase.empty()) {
        result.tokenAddr = bestNonBase;
        result.rawAmount = bestAbs;
        result.isBuy = bestNet > 0;
    } else {
        std::string bestToken;
        cpp_int bestTokenAbs = -1;
        cpp_int bestTokenNet = 0;

        for (const auto& token : parsed.tokenOrder) {
            const cpp_int net = parsed.netFlow[token];
            const cpp_int magnitude = absInt(net);
            if (magnitude > bestTokenAbs) {
                bestToken = token;
                bestTokenAbs = magnitude;
                bestTokenNet = net;
            }
        }

        if (!bestToken.empty()) {
            result.tokenAddr = bestToken;
            result.rawAmount = bestTokenAbs;
            result.isBuy = bestTokenNet > 0;
        }
    }

    if (result.isSwap && !result.tokenAddr.empty()) {
        cpp_int bestCounterAbs = -1;
        std::string bestCounter;

        for (const auto& token : parsed.tokenOrder) {
            if (token == result.tokenAddr) continue;
            const cpp_int net = parsed.netFlow[token];

            if (result.isBuy && net >= 0) continue;
            if (!result.isBuy && net <= 0) continue;

            const cpp_int magnitude = absInt(net);
            const bool quote = isBaseAsset(token);
            const bool currentQuote = !bestCounter.empty() && isBaseAsset(bestCounter);

            if (bestCounter.empty() ||
                (quote && !currentQuote) ||
                (quote == currentQuote && magnitude > bestCounterAbs)) {
                bestCounter = token;
                bestCounterAbs = magnitude;
            }
        }

        if (bestCounter.empty() && result.isBuy && nativeSwapSignal) {
            bestCounter = chainCtx().nativeMarker;
            bestCounterAbs = nativeOut;
        } else if (bestCounter.empty() && !result.isBuy && nativeInflowSignal) {
            bestCounter = chainCtx().nativeMarker;
            bestCounterAbs = nativeIn;
        }

        result.counterAddr = bestCounter;
        result.counterAmount = bestCounterAbs > 0 ? bestCounterAbs : 0;
    }

    if (!result.isSwap && !lpAdd && !lpRemove &&
        decoded.valid && decoded.bridge && parsed.anyWalletTransfer) {
        result.venue = decoded.protocol.empty() ? "Bridge" : decoded.protocol;
        result.calldataMatched = true;
    }

    // Semantic recovery: calldata establishes the expected pair, receipt must confirm at least one side.
    if (!result.isSwap && !lpAdd && !lpRemove && decoded.valid && decoded.swap) {
        const std::string tokenIn = toLower(decoded.tokenIn);
        const std::string tokenOut = toLower(decoded.tokenOut);

        const cpp_int observedIn =
            (!tokenIn.empty() && !isNativeAsset(tokenIn) && parsed.netFlow.count(tokenIn))
                ? parsed.netFlow[tokenIn] : 0;
        const cpp_int observedOut =
            (!tokenOut.empty() && !isNativeAsset(tokenOut) && parsed.netFlow.count(tokenOut))
                ? parsed.netFlow[tokenOut] : 0;

        bool recovered = false;

        if (!tokenOut.empty() && !isQuoteAsset(tokenOut) &&
            (observedOut > 0 || (isNativeAsset(tokenIn) && nativeOutflow))) {
            result.isSwap = true;
            result.isBuy = true;
            result.tokenAddr = tokenOut;
            result.rawAmount = observedOut > 0 ? observedOut : absInt(parsed.netFlow[tokenOut]);
            result.counterAddr = tokenIn;

            if (isNativeAsset(tokenIn)) result.counterAmount = nativeOut;
            else if (observedIn < 0) result.counterAmount = -observedIn;
            else if (decoded.amountIn > 0) result.counterAmount = decoded.amountIn;
            else result.counterAmount = decoded.amountInMax;

            recovered = result.rawAmount > 0;
        } else if (!tokenIn.empty() && !isQuoteAsset(tokenIn) &&
                   (observedIn < 0 || (isNativeAsset(tokenOut) && nativeInflowSignal))) {
            result.isSwap = true;
            result.isBuy = false;
            result.tokenAddr = tokenIn;
            result.rawAmount = observedIn < 0 ? -observedIn : absInt(parsed.netFlow[tokenIn]);
            result.counterAddr = tokenOut;

            if (isNativeAsset(tokenOut)) result.counterAmount = nativeIn;
            else if (observedOut > 0) result.counterAmount = observedOut;
            else if (decoded.amountOut > 0) result.counterAmount = decoded.amountOut;
            else result.counterAmount = decoded.amountOutMin;

            recovered = result.rawAmount > 0;
        } else if (!tokenIn.empty() && !tokenOut.empty() &&
                   !isQuoteAsset(tokenIn) && !isQuoteAsset(tokenOut)) {
            const bool sawInput = observedIn < 0;
            const bool sawOutput = observedOut > 0;

            if (sawInput || sawOutput) {
                result.isSwap = true;
                if (sawOutput) {
                    result.isBuy = true;
                    result.tokenAddr = tokenOut;
                    result.rawAmount = observedOut;
                    result.counterAddr = tokenIn;
                    result.counterAmount = sawInput ? -observedIn :
                        (decoded.amountIn > 0 ? decoded.amountIn : decoded.amountInMax);
                } else {
                    result.isBuy = false;
                    result.tokenAddr = tokenIn;
                    result.rawAmount = -observedIn;
                    result.counterAddr = tokenOut;
                    result.counterAmount = decoded.amountOut > 0
                        ? decoded.amountOut : decoded.amountOutMin;
                }
                recovered = result.rawAmount > 0;
            }
        }

        if (recovered) {
            result.calldataMatched = true;
            result.calldataRecovered = true;
            if (result.venue.empty())
                result.venue = decoded.protocol.empty() ? "Decoded Router Call" : decoded.protocol;
        }
    }

    // Aggregator/V4 calldata may establish a swap family without exposing token
    // addresses. In that case use only receipt-confirmed pool-related wallet flow.
    if (!result.isSwap && !lpAdd && !lpRemove &&
        decoded.valid && decoded.swap &&
        decoded.tokenIn.empty() && decoded.tokenOut.empty() &&
        parsed.hasSwapEvent) {
        std::string candidate;
        cpp_int candidateNet = 0;
        cpp_int candidateAbs = -1;

        for (const auto& token : parsed.tokenOrder) {
            if (isBaseAsset(token)) continue;
            const cpp_int net = parsed.netFlow[token];
            if (net == 0 || !sourceMatchesPool(parsed, token, net > 0)) continue;
            const cpp_int magnitude = absInt(net);
            if (magnitude > candidateAbs) {
                candidate = token;
                candidateNet = net;
                candidateAbs = magnitude;
            }
        }

        if (!candidate.empty()) {
            result.isSwap = true;
            result.isBuy = candidateNet > 0;
            result.tokenAddr = candidate;
            result.rawAmount = candidateAbs;
            result.calldataMatched = true;
            result.calldataRecovered = true;

            for (const auto& token : parsed.tokenOrder) {
                if (token == candidate) continue;
                const cpp_int net = parsed.netFlow[token];
                if ((result.isBuy && net < 0) || (!result.isBuy && net > 0)) {
                    result.counterAddr = token;
                    result.counterAmount = absInt(net);
                    if (isBaseAsset(token)) break;
                }
            }

            if (result.counterAddr.empty() && result.isBuy && nativeSwapSignal) {
                result.counterAddr = chainCtx().nativeMarker;
                result.counterAmount = nativeOut;
            } else if (result.counterAddr.empty() && !result.isBuy && nativeInflowSignal) {
                result.counterAddr = chainCtx().nativeMarker;
                result.counterAmount = nativeIn;
            }
        }
    }

    // Receipt transfer-graph recovery. This handles wallet -> router -> pool and
    // pool -> router -> recipient chains that direct wallet netFlow cannot pair.
    if (!result.isSwap && !lpAdd && !lpRemove && parsed.hasSwapEvent) {
        result.transferGraphSeen = !parsed.transfers.empty();

        std::set<std::string> recipients = {wallet};
        if (decoded.valid && !decoded.recipient.empty())
            recipients.insert(toLower(decoded.recipient));

        struct Candidate {
            std::string token;
            cpp_int net = 0;
            cpp_int amount = 0;
            bool incoming = false;
            bool ambiguous = false;
        };

        std::vector<Candidate> candidates;
        for (const std::string& token : parsed.tokenOrder) {
            if (isBaseAsset(token)) continue;
            const cpp_int net = parsed.netFlow[token];
            if (net == 0) continue;

            GraphPathResult path;
            const bool incoming = net > 0;
            if (!graphMatchesPool(parsed, token, wallet, incoming, recipients, &path))
                continue;

            if (incoming) result.graphPathFromPool = true;
            else result.graphPathToPool = true;

            candidates.push_back(Candidate{
                token, net, path.amount > 0 ? path.amount : absInt(net),
                incoming, path.ambiguous
            });
        }

        if (candidates.size() == 1) {
            const Candidate& main = candidates.front();
            const bool intentSupportsSwap =
                decoded.swap || routerCall || knownRouter || ur.present;

            bool directionSupported = false;
            if (main.incoming) {
                directionSupported =
                    nativeSwapSignal || hasBaseOut ||
                    (decoded.swap && (decoded.tokenOut.empty() ||
                     toLower(decoded.tokenOut) == main.token));
            } else {
                directionSupported =
                    nativeInflowSignal || hasBaseIn ||
                    intentSupportsSwap;
            }

            if (directionSupported) {
                result.isSwap = true;
                result.isBuy = main.incoming;
                result.tokenAddr = main.token;
                result.rawAmount = absInt(main.net);
                if (result.rawAmount == 0) result.rawAmount = main.amount;
                result.graphRecovered = result.rawAmount > 0;
                result.graphAmbiguous = main.ambiguous;

                // First try a directly observed opposite wallet flow.
                cpp_int bestCounterAmount = 0;
                std::string bestCounter;
                for (const std::string& token : parsed.tokenOrder) {
                    if (token == main.token) continue;
                    const cpp_int net = parsed.netFlow[token];
                    if ((result.isBuy && net >= 0) || (!result.isBuy && net <= 0))
                        continue;

                    const cpp_int magnitude = absInt(net);
                    if (bestCounter.empty() ||
                        (isBaseAsset(token) && !isBaseAsset(bestCounter)) ||
                        (isBaseAsset(token) == isBaseAsset(bestCounter) &&
                         magnitude > bestCounterAmount)) {
                        bestCounter = token;
                        bestCounterAmount = magnitude;
                    }
                }

                // Then search the full transfer graph for the missing counter side.
                if (bestCounter.empty()) {
                    for (const TransferEdge& edge : parsed.transfers) {
                        if (edge.token == main.token || isGraphTerminal(edge.token)) continue;

                        GraphPathResult counterPath = result.isBuy
                            ? walletToPoolPath(parsed, edge.token, wallet)
                            : poolToRecipientsPath(parsed, edge.token, recipients);

                        if (!counterPath.found || counterPath.amount <= 0) continue;
                        if (bestCounter.empty() ||
                            (isBaseAsset(edge.token) && !isBaseAsset(bestCounter)) ||
                            (isBaseAsset(edge.token) == isBaseAsset(bestCounter) &&
                             counterPath.amount > bestCounterAmount)) {
                            bestCounter = edge.token;
                            bestCounterAmount = counterPath.amount;
                            result.graphAmbiguous =
                                result.graphAmbiguous || counterPath.ambiguous;
                        }
                    }
                }

                if (bestCounter.empty() && result.isBuy && nativeSwapSignal) {
                    bestCounter = chainCtx().nativeMarker;
                    bestCounterAmount = nativeOut;
                } else if (bestCounter.empty() && !result.isBuy && nativeInflowSignal) {
                    bestCounter = chainCtx().nativeMarker;
                    bestCounterAmount = nativeIn;
                } else if (bestCounter.empty() && decoded.swap) {
                    bestCounter = result.isBuy ? decoded.tokenIn : decoded.tokenOut;
                    if (result.isBuy)
                        bestCounterAmount = decoded.amountIn > 0
                            ? decoded.amountIn : decoded.amountInMax;
                    else
                        bestCounterAmount = decoded.amountOut > 0
                            ? decoded.amountOut : decoded.amountOutMin;
                }

                result.counterAddr = bestCounter;
                result.counterAmount = bestCounterAmount;
                result.calldataMatched = result.calldataMatched || decoded.swap;

                if (result.venue.empty())
                    result.venue = decoded.protocol.empty()
                        ? "Receipt Transfer Graph"
                        : decoded.protocol;
            }
        } else if (candidates.size() > 1) {
            result.graphAmbiguous = true;
        }
    }

    if (!result.tokenAddr.empty()) {
        const int decimals = isNativeAsset(result.tokenAddr) ? 18 : getDecimals(result.tokenAddr);
        const uint64_t price = isNativeAsset(result.tokenAddr)
            ? getPriceNanos(chainCtx().wrappedNative)
            : getPriceNanos(result.tokenAddr);
        result.usdNanos = calcUsdNanos(result.rawAmount, decimals, price);
    }

    const bool decodedWalletIntent =
        decoded.valid &&
        (
            decoded.swap ||
            decoded.liquidity ||
            decoded.bridge ||
            decoded.operation == TxOperation::WRAP_NATIVE ||
            decoded.operation == TxOperation::UNWRAP_NATIVE
        ) &&
        (
            walletIsSender ||
            (!decoded.recipient.empty() && toLower(decoded.recipient) == wallet)
        );

    result.walletSwapRelated =
        result.isSwap ||
        result.venue == "Add Liquidity" ||
        result.venue == "Remove Liquidity" ||
        result.venue == "Wrap" ||
        result.venue == "Unwrap" ||
        result.graphPathToPool ||
        result.graphPathFromPool ||
        result.graphRecovered ||
        result.calldataMatched ||
        result.calldataRecovered ||
        decodedWalletIntent ||
        (
            walletIsSender &&
            (
                knownRouter ||
                ur.present ||
                decoded.multicall ||
                decoded.permit2
            )
        );

    result.unrelatedSwapEvent =
        parsed.hasSwapEvent && !result.walletSwapRelated;

    // A Swap event anywhere in the receipt is only diagnostic. It is DEX
    // activity only after correlation with the tracked wallet.
    result.dexActivityDetected = result.walletSwapRelated;

    if (!result.isSwap &&
        result.venue != "Add Liquidity" &&
        result.venue != "Remove Liquidity" &&
        result.venue != "Wrap" &&
        result.venue != "Unwrap" &&
        result.dexActivityDetected) {
        if (!twoSidedFlow) {
            if (decoded.valid && decoded.swap)
                result.unknownReason = "DECODED_NO_RECEIPT_MATCH";
            else if (decoded.valid && decoded.permit2)
                result.unknownReason = "PERMIT2_NO_SWAP_MATCH";
            else if (decoded.valid && decoded.bridge)
                result.unknownReason = "BRIDGE_FLOW";
            else if (decoded.valid && decoded.aggregator)
                result.unknownReason = "AGGREGATOR_NO_FLOW_MATCH";
            else if (decoded.valid && decoded.v4)
                result.unknownReason = "V4_NO_FLOW_MATCH";
            else if (decoded.valid && decoded.targetedMulticall)
                result.unknownReason = "MULTICALL_NO_FLOW_MATCH";
            else if (decoded.valid && decoded.universalSubPlan)
                result.unknownReason = "SUBPLAN_NO_FLOW_MATCH";
            else if (decoded.valid && decoded.v4 && !decoded.v4ActionsDecoded)
                result.unknownReason = "V4_ACTIONS_UNDECODED";
            else if (result.transferGraphSeen && result.graphAmbiguous)
                result.unknownReason = "GRAPH_AMBIGUOUS_PATH";
            else if (result.transferGraphSeen &&
                     !result.graphPathToPool && !result.graphPathFromPool)
                result.unknownReason = "GRAPH_NO_POOL_PATH";
            else
                result.unknownReason = "NO_DIRECT_COUNTER_FLOW";
        } else if (parsed.hasSwapEvent && !routerCall) {
            result.unknownReason = "UNKNOWN_ROUTER";
        } else {
            result.unknownReason = "OTHER";
        }
    }

    applyDecoded(result);
    return result;
}
