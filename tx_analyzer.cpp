#include "tx_analyzer.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils.h"

using json = nlohmann::json;

uint64_t getPriceNanos(const std::string& token);
int getDecimals(const std::string& addr);
std::string getSymbol(const std::string& addr);

namespace {

constexpr const char* ZERO_ADDRESS = "0x0000000000000000000000000000000000000000";
constexpr const char* DEAD_ADDRESS = "0x000000000000000000000000000000000000dead";

const std::string ERC20_TRANSFER_TOPIC =
    "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
const std::string ERC20_APPROVAL_TOPIC =
    "0x8c5be1e5ebec7d5bd14f714f9f7f5f5d5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5"; // diagnostic only
const std::string WRAPPED_DEPOSIT_TOPIC =
    "0xe1fffcc4923d04b559f4d29a8bfc6cda04eb5b0d3c460751c2402c5c5cc9109c";
const std::string WRAPPED_WITHDRAW_TOPIC =
    "0x7fcf532c15f0a6db0bd6d0e038bea71d30d808c7d98cb3bf7268a95bf5081b65";

const std::string V3_INCREASE_LIQUIDITY_TOPIC =
    "0x3067048beee31b25b2f1681f88dac838c8bba36af25bfb2b7cf7473a5847e35f";
const std::string V3_DECREASE_LIQUIDITY_TOPIC =
    "0x26f6a048ee9138f2c0ce266f322cb99228e8d619ae2bff30c67f8dcf9d2377b4";
const std::string V3_COLLECT_TOPIC =
    "0x40d0efd1a53d60ecbf40971b9daf7dc90178c3aadc7aab1765632738fa8b8f01";

const std::set<std::string> SWAP_EVENT_TOPICS = {
    "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822", // V2
    "0xc42079f94a6350d7e6235f29174924f928cc2ac818eb64fed8004e115fbcca67", // V3
    "0x19b47279256b2a23a1665c810c8d55a1758940ee09377d4f8d26497a3577dc83"  // V4-style
};

std::string lowerAddress(const std::string& value) {
    return toLower(value);
}

bool isHex(const std::string& value) {
    if (value.size() < 2 || value[0] != '0' || value[1] != 'x') return false;
    for (size_t i = 2; i < value.size(); ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

cpp_int absInt(const cpp_int& value) {
    return value < 0 ? -value : value;
}

std::string topicAddress(const std::string& topic) {
    if (topic.size() < 66) return {};
    return "0x" + toLower(topic.substr(topic.size() - 40));
}

enum class Operation {
    UNKNOWN,
    SWAP_EXACT_IN,
    SWAP_EXACT_OUT,
    WRAP,
    UNWRAP,
    ADD_LIQUIDITY,
    REMOVE_LIQUIDITY,
    TRANSFER,
    BRIDGE,
    STAKE,
    UNSTAKE,
    CLAIM,
    NFT,
    MULTICALL,
    PERMIT2
};

enum class Intent {
    UNKNOWN,
    BUY,
    SELL,
    TOKEN_SWAP,
    WRAP,
    UNWRAP,
    ADD_LIQUIDITY,
    REMOVE_LIQUIDITY,
    TRANSFER,
    BRIDGE,
    STAKE,
    UNSTAKE,
    CLAIM,
    NFT
};

struct DecodedCall {
    bool recognized = false;
    bool complete = false;
    bool malformed = false;

    std::string selector;
    std::string functionName;
    std::string target;
    std::string protocol;
    RouterType routerType = RouterType::UNKNOWN;
    Operation operation = Operation::UNKNOWN;

    std::string tokenIn;
    std::string tokenOut;
    std::string recipient;
    std::vector<std::string> path;

    cpp_int amountIn = 0;
    cpp_int amountOut = 0;
    cpp_int amountOutMin = 0;
    cpp_int amountInMax = 0;
    cpp_int nativeValue = 0;

    bool exactInput = false;
    bool exactOutput = false;
    bool feeOnTransfer = false;
    bool universalRouter = false;
    bool multicall = false;
    bool permit2 = false;

    std::vector<DecodedCall> children;
    std::vector<std::string> diagnostics;
};

struct ParsedReceipt {
    bool valid = false;
    bool hasWalletTransfer = false;
    bool hasSwapEvent = false;
    bool hasV3Increase = false;
    bool hasV3Decrease = false;
    bool hasV3Collect = false;

    cpp_int wrappedNative = 0;
    cpp_int unwrappedNative = 0;

    std::map<std::string, cpp_int> netFlow;
    std::vector<std::string> tokenOrder;
    std::map<std::string, std::set<std::string>> inSources;
    std::map<std::string, std::set<std::string>> outDestinations;

    std::set<std::string> swapPools;
    std::set<std::string> mintedTokens;
    std::set<std::string> burnedTokens;
    std::set<std::string> counterparties;
};

struct AssetFlow {
    std::string asset;
    cpp_int amount = 0;
    bool incoming = false;
    bool baseAsset = false;
    bool fromSwapPool = false;
    bool toSwapPool = false;
};

struct SemanticMatch {
    bool matched = false;
    bool isSwap = false;
    bool isBuy = false;
    bool isLpAdd = false;
    bool isLpRemove = false;
    bool isWrap = false;
    bool isUnwrap = false;

    std::string token;
    cpp_int tokenAmount = 0;
    std::string counterToken;
    cpp_int counterAmount = 0;

    std::string venue;
    std::string reason;
};

class AbiReader {
public:
    explicit AbiReader(std::string calldata)
        : data_(std::move(calldata)) {
        valid_ = isHex(data_) && data_.size() >= 10 && ((data_.size() - 2) % 2 == 0);
        if (valid_) selector_ = toLower(data_.substr(2, 8));
    }

    bool valid() const { return valid_; }
    const std::string& selector() const { return selector_; }
    const std::string& raw() const { return data_; }

    size_t argumentBytes() const {
        return data_.size() >= 10 ? (data_.size() - 10) / 2 : 0;
    }

    std::optional<std::string> word(size_t index) const {
        const size_t pos = 10 + index * 64;
        if (!valid_ || pos + 64 > data_.size()) return std::nullopt;
        return "0x" + data_.substr(pos, 64);
    }

    std::optional<cpp_int> uint256(size_t index) const {
        const auto w = word(index);
        if (!w) return std::nullopt;
        return hexToCppInt(*w);
    }

    std::optional<uint64_t> uint64Word(size_t index) const {
        const auto v = uint256(index);
        if (!v || *v < 0 || *v > std::numeric_limits<uint64_t>::max()) return std::nullopt;
        return v->convert_to<uint64_t>();
    }

    std::optional<std::string> address(size_t index) const {
        const auto w = word(index);
        if (!w || w->size() != 66) return std::nullopt;
        return "0x" + toLower(w->substr(26));
    }

    std::optional<size_t> dynamicOffsetBytes(size_t index) const {
        const auto value = uint256(index);
        if (!value || *value < 0 || *value > std::numeric_limits<size_t>::max()) return std::nullopt;
        const size_t offset = value->convert_to<size_t>();
        if (offset % 32 != 0 || offset > argumentBytes()) return std::nullopt;
        return offset;
    }

    std::optional<std::string> dynamicBytes(size_t index) const {
        const auto offset = dynamicOffsetBytes(index);
        if (!offset) return std::nullopt;
        return dynamicBytesAt(*offset);
    }

    std::optional<std::vector<std::string>> bytesArray(size_t index) const {
        const auto offset = dynamicOffsetBytes(index);
        if (!offset) return std::nullopt;

        const auto count = uint256AtByteOffset(*offset);
        if (!count || *count < 0 || *count > 256) return std::nullopt;
        const size_t n = count->convert_to<size_t>();

        std::vector<std::string> result;
        result.reserve(n);
        const size_t headStart = *offset + 32;

        for (size_t i = 0; i < n; ++i) {
            const auto relative = uint256AtByteOffset(headStart + i * 32);
            if (!relative || *relative < 0 || *relative > argumentBytes()) return std::nullopt;
            const size_t rel = relative->convert_to<size_t>();
            const size_t elementOffset = headStart + rel;
            const auto bytes = dynamicBytesAt(elementOffset);
            if (!bytes) return std::nullopt;
            result.push_back(*bytes);
        }
        return result;
    }

    std::optional<std::vector<std::string>> addressArray(size_t index) const {
        const auto offset = dynamicOffsetBytes(index);
        if (!offset) return std::nullopt;
        const auto count = uint256AtByteOffset(*offset);
        if (!count || *count < 0 || *count > 128) return std::nullopt;
        const size_t n = count->convert_to<size_t>();

        std::vector<std::string> result;
        result.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const auto w = wordAtByteOffset(*offset + 32 + i * 32);
            if (!w) return std::nullopt;
            result.push_back("0x" + toLower(w->substr(26)));
        }
        return result;
    }

    std::optional<std::string> tupleWord(size_t tupleArgIndex, size_t tupleWordIndex) const {
        const auto offset = dynamicOffsetBytes(tupleArgIndex);
        if (!offset) return std::nullopt;
        return wordAtByteOffset(*offset + tupleWordIndex * 32);
    }

    std::optional<std::string> tupleAddress(size_t tupleArgIndex, size_t tupleWordIndex) const {
        const auto w = tupleWord(tupleArgIndex, tupleWordIndex);
        if (!w) return std::nullopt;
        return "0x" + toLower(w->substr(26));
    }

    std::optional<cpp_int> tupleUint(size_t tupleArgIndex, size_t tupleWordIndex) const {
        const auto w = tupleWord(tupleArgIndex, tupleWordIndex);
        if (!w) return std::nullopt;
        return hexToCppInt(*w);
    }

    std::optional<std::string> tupleDynamicBytes(size_t tupleArgIndex, size_t tupleWordIndex) const {
        const auto tupleOffset = dynamicOffsetBytes(tupleArgIndex);
        if (!tupleOffset) return std::nullopt;
        const auto relWord = wordAtByteOffset(*tupleOffset + tupleWordIndex * 32);
        if (!relWord) return std::nullopt;
        const cpp_int relInt = hexToCppInt(*relWord);
        if (relInt < 0 || relInt > argumentBytes()) return std::nullopt;
        return dynamicBytesAt(*tupleOffset + relInt.convert_to<size_t>());
    }

private:
    std::optional<std::string> wordAtByteOffset(size_t byteOffset) const {
        const size_t pos = 10 + byteOffset * 2;
        if (!valid_ || pos + 64 > data_.size()) return std::nullopt;
        return "0x" + data_.substr(pos, 64);
    }

    std::optional<cpp_int> uint256AtByteOffset(size_t byteOffset) const {
        const auto w = wordAtByteOffset(byteOffset);
        if (!w) return std::nullopt;
        return hexToCppInt(*w);
    }

    std::optional<std::string> dynamicBytesAt(size_t byteOffset) const {
        const auto length = uint256AtByteOffset(byteOffset);
        if (!length || *length < 0 || *length > argumentBytes()) return std::nullopt;
        const size_t len = length->convert_to<size_t>();
        const size_t pos = 10 + (byteOffset + 32) * 2;
        if (pos + len * 2 > data_.size()) return std::nullopt;
        return "0x" + data_.substr(pos, len * 2);
    }

    std::string data_;
    std::string selector_;
    bool valid_ = false;
};

std::vector<std::string> decodeV3Path(const std::string& packedPath, bool reverse) {
    std::vector<std::string> path;
    if (!isHex(packedPath) || packedPath.size() < 2 + 40) return path;

    const std::string hex = packedPath.substr(2);
    if (hex.size() < 40) return path;

    size_t pos = 0;
    path.push_back("0x" + toLower(hex.substr(pos, 40)));
    pos += 40;

    while (pos + 6 + 40 <= hex.size()) {
        pos += 6; // uint24 fee
        path.push_back("0x" + toLower(hex.substr(pos, 40)));
        pos += 40;
    }

    if (reverse) std::reverse(path.begin(), path.end());
    return path;
}

using DecoderFn = std::function<DecodedCall(const json&, const AbiReader&, int)>;

class SelectorRegistry {
public:
    static SelectorRegistry& instance() {
        static SelectorRegistry registry;
        return registry;
    }

    void registerDecoder(const std::string& selector, DecoderFn decoder) {
        decoders_[toLower(selector)] = std::move(decoder);
    }

    const DecoderFn* find(const std::string& selector) const {
        const auto it = decoders_.find(toLower(selector));
        return it == decoders_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, DecoderFn> decoders_;
};

DecodedCall baseDecoded(const json& tx, const AbiReader& reader) {
    DecodedCall out;
    out.selector = reader.selector();
    if (tx.contains("to") && !tx["to"].is_null() && tx["to"].is_string()) {
        out.target = lowerAddress(tx["to"].get<std::string>());
        out.protocol = lookupRouterLabel(out.target);
        const auto it = chainCtx().protocols.find(out.target);
        if (it != chainCtx().protocols.end()) out.routerType = it->second.routerType;
    }
    if (tx.contains("value") && tx["value"].is_string()) {
        out.nativeValue = hexToCppInt(tx["value"].get<std::string>());
    }
    return out;
}

DecodedCall decodeCallRecursive(const json& tx, int depth);

DecodedCall decodeV2Swap(
    const json& tx,
    const AbiReader& r,
    const std::string& name,
    bool exactInput,
    bool nativeIn,
    bool nativeOut,
    bool feeOnTransfer,
    size_t amountA,
    size_t amountB,
    size_t pathIndex,
    size_t recipientIndex
) {
    DecodedCall out = baseDecoded(tx, r);
    out.recognized = true;
    out.functionName = name;
    out.operation = exactInput ? Operation::SWAP_EXACT_IN : Operation::SWAP_EXACT_OUT;
    out.exactInput = exactInput;
    out.exactOutput = !exactInput;
    out.feeOnTransfer = feeOnTransfer;

    const auto a = r.uint256(amountA);
    const auto b = r.uint256(amountB);
    const auto path = r.addressArray(pathIndex);
    const auto recipient = r.address(recipientIndex);

    if (nativeIn) {
        if (exactInput) {
            out.amountIn = out.nativeValue;
            if (a) out.amountOutMin = *a;
        } else {
            if (a) out.amountOut = *a;
            out.amountInMax = out.nativeValue;
        }
    } else {
        if (a) {
            if (exactInput) out.amountIn = *a;
            else out.amountOut = *a;
        }
        if (b) {
            if (exactInput) out.amountOutMin = *b;
            else out.amountInMax = *b;
        }
    }
    if (path) out.path = *path;
    if (recipient) out.recipient = *recipient;

    if (nativeIn) out.tokenIn = chainCtx().nativeMarker;
    else if (!out.path.empty()) out.tokenIn = out.path.front();

    if (nativeOut) out.tokenOut = chainCtx().nativeMarker;
    else if (!out.path.empty()) out.tokenOut = out.path.back();

    out.complete = !out.tokenIn.empty() && !out.tokenOut.empty();
    if (!out.complete) out.diagnostics.push_back("V2_PATH_DECODE_FAILED");
    return out;
}

DecodedCall decodeV3ExactInputSingle(
    const json& tx,
    const AbiReader& r,
    bool hasDeadline
) {
    DecodedCall out = baseDecoded(tx, r);
    out.recognized = true;
    out.functionName = "exactInputSingle";
    out.operation = Operation::SWAP_EXACT_IN;
    out.exactInput = true;

    // SwapRouter legacy:
    // tokenIn, tokenOut, fee, recipient, deadline, amountIn, amountOutMinimum, sqrtPriceLimitX96
    // SwapRouter02:
    // tokenIn, tokenOut, fee, recipient, amountIn, amountOutMinimum, sqrtPriceLimitX96
    const auto tokenIn = r.address(0);
    const auto tokenOut = r.address(1);
    const auto recipient = r.address(3);
    const auto amountIn = r.uint256(hasDeadline ? 5 : 4);
    const auto amountOutMin = r.uint256(hasDeadline ? 6 : 5);

    if (tokenIn) out.tokenIn = *tokenIn;
    if (tokenOut) out.tokenOut = *tokenOut;
    if (recipient) out.recipient = *recipient;
    if (amountIn) out.amountIn = *amountIn;
    if (amountOutMin) out.amountOutMin = *amountOutMin;
    if (!out.tokenIn.empty()) out.path.push_back(out.tokenIn);
    if (!out.tokenOut.empty()) out.path.push_back(out.tokenOut);
    out.complete = !out.tokenIn.empty() && !out.tokenOut.empty();
    return out;
}

DecodedCall decodeV3ExactOutputSingle(
    const json& tx,
    const AbiReader& r,
    bool hasDeadline
) {
    DecodedCall out = baseDecoded(tx, r);
    out.recognized = true;
    out.functionName = "exactOutputSingle";
    out.operation = Operation::SWAP_EXACT_OUT;
    out.exactOutput = true;

    const auto tokenIn = r.address(0);
    const auto tokenOut = r.address(1);
    const auto recipient = r.address(3);
    const auto amountOut = r.uint256(hasDeadline ? 5 : 4);
    const auto amountInMax = r.uint256(hasDeadline ? 6 : 5);

    if (tokenIn) out.tokenIn = *tokenIn;
    if (tokenOut) out.tokenOut = *tokenOut;
    if (recipient) out.recipient = *recipient;
    if (amountOut) out.amountOut = *amountOut;
    if (amountInMax) out.amountInMax = *amountInMax;
    if (!out.tokenIn.empty()) out.path.push_back(out.tokenIn);
    if (!out.tokenOut.empty()) out.path.push_back(out.tokenOut);
    out.complete = !out.tokenIn.empty() && !out.tokenOut.empty();
    return out;
}

DecodedCall decodeV3ExactInput(const json& tx, const AbiReader& r) {
    DecodedCall out = baseDecoded(tx, r);
    out.recognized = true;
    out.functionName = "exactInput";
    out.operation = Operation::SWAP_EXACT_IN;
    out.exactInput = true;

    const auto packedPath = r.tupleDynamicBytes(0, 0);
    const auto recipient = r.tupleAddress(0, 1);
    const auto amountIn = r.tupleUint(0, 3);
    const auto amountOutMin = r.tupleUint(0, 4);

    if (packedPath) out.path = decodeV3Path(*packedPath, false);
    if (recipient) out.recipient = *recipient;
    if (amountIn) out.amountIn = *amountIn;
    if (amountOutMin) out.amountOutMin = *amountOutMin;
    if (!out.path.empty()) {
        out.tokenIn = out.path.front();
        out.tokenOut = out.path.back();
    }
    out.complete = !out.tokenIn.empty() && !out.tokenOut.empty();
    return out;
}

DecodedCall decodeV3ExactOutput(const json& tx, const AbiReader& r) {
    DecodedCall out = baseDecoded(tx, r);
    out.recognized = true;
    out.functionName = "exactOutput";
    out.operation = Operation::SWAP_EXACT_OUT;
    out.exactOutput = true;

    const auto packedPath = r.tupleDynamicBytes(0, 0);
    const auto recipient = r.tupleAddress(0, 1);
    const auto amountOut = r.tupleUint(0, 3);
    const auto amountInMax = r.tupleUint(0, 4);

    if (packedPath) out.path = decodeV3Path(*packedPath, true);
    if (recipient) out.recipient = *recipient;
    if (amountOut) out.amountOut = *amountOut;
    if (amountInMax) out.amountInMax = *amountInMax;
    if (!out.path.empty()) {
        out.tokenIn = out.path.front();
        out.tokenOut = out.path.back();
    }
    out.complete = !out.tokenIn.empty() && !out.tokenOut.empty();
    return out;
}

DecodedCall decodeWrap(const json& tx, const AbiReader& r) {
    DecodedCall out = baseDecoded(tx, r);
    out.recognized = true;
    out.complete = true;
    out.functionName = "deposit";
    out.operation = Operation::WRAP;
    out.tokenIn = chainCtx().nativeMarker;
    out.tokenOut = chainCtx().wrappedNative;
    out.amountIn = out.nativeValue;
    return out;
}

DecodedCall decodeUnwrap(const json& tx, const AbiReader& r) {
    DecodedCall out = baseDecoded(tx, r);
    out.recognized = true;
    out.functionName = "withdraw";
    out.operation = Operation::UNWRAP;
    const auto amount = r.uint256(0);
    if (amount) out.amountIn = *amount;
    out.tokenIn = chainCtx().wrappedNative;
    out.tokenOut = chainCtx().nativeMarker;
    out.complete = amount.has_value();
    return out;
}

DecodedCall decodeMulticall(const json& tx, const AbiReader& r, int depth) {
    DecodedCall out = baseDecoded(tx, r);
    out.recognized = true;
    out.multicall = true;
    out.operation = Operation::MULTICALL;
    out.functionName = "multicall";

    if (depth >= 6) {
        out.malformed = true;
        out.diagnostics.push_back("MULTICALL_MAX_DEPTH");
        return out;
    }

    // multicall(bytes[]) or multicall(uint256,bytes[])
    std::optional<std::vector<std::string>> calls = r.bytesArray(0);
    if (!calls) calls = r.bytesArray(1);
    if (!calls) {
        out.diagnostics.push_back("MULTICALL_BYTES_ARRAY_DECODE_FAILED");
        return out;
    }

    for (const std::string& inner : *calls) {
        json nestedTx = tx;
        nestedTx["input"] = inner;
        DecodedCall child = decodeCallRecursive(nestedTx, depth + 1);
        out.children.push_back(std::move(child));
    }

    for (const auto& child : out.children) {
        if (child.recognized && child.operation != Operation::UNKNOWN &&
            child.operation != Operation::MULTICALL &&
            child.operation != Operation::PERMIT2) {
            out.operation = child.operation;
            out.tokenIn = child.tokenIn;
            out.tokenOut = child.tokenOut;
            out.path = child.path;
            out.amountIn = child.amountIn;
            out.amountOut = child.amountOut;
            out.amountOutMin = child.amountOutMin;
            out.amountInMax = child.amountInMax;
            out.recipient = child.recipient;
            out.exactInput = child.exactInput;
            out.exactOutput = child.exactOutput;
            out.complete = child.complete;
            break;
        }
    }
    return out;
}

struct UniversalCommandInfo {
    bool swap = false;
    bool v2 = false;
    bool v3 = false;
    bool exactOut = false;
    bool wrap = false;
    bool unwrap = false;
    bool sweep = false;
    bool transfer = false;
    bool permit2 = false;
};

UniversalCommandInfo universalCommandInfo(uint8_t command) {
    UniversalCommandInfo out;
    const uint8_t cmd = command & 0x3f;
    switch (cmd) {
        case 0x00: out.swap = true; out.v3 = true; break;                // V3_SWAP_EXACT_IN
        case 0x01: out.swap = true; out.v3 = true; out.exactOut = true; break;
        case 0x02: out.permit2 = true; break;
        case 0x03: out.permit2 = true; break;
        case 0x04: out.sweep = true; break;
        case 0x05: out.transfer = true; break;
        case 0x08: out.swap = true; out.v2 = true; break;
        case 0x09: out.swap = true; out.v2 = true; out.exactOut = true; break;
        case 0x0a: out.permit2 = true; break;
        case 0x0b: out.wrap = true; break;
        case 0x0c: out.unwrap = true; break;
        case 0x0d: out.permit2 = true; break;
        default: break;
    }
    return out;
}

std::vector<uint8_t> decodeByteString(const std::string& bytes) {
    std::vector<uint8_t> result;
    if (!isHex(bytes) || (bytes.size() - 2) % 2 != 0) return result;
    for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
        const int hi = nibble(bytes[i]);
        const int lo = nibble(bytes[i + 1]);
        if (hi < 0 || lo < 0) return {};
        result.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return result;
}

DecodedCall decodeUniversalRouter(const json& tx, const AbiReader& r, int depth) {
    DecodedCall out = baseDecoded(tx, r);
    out.recognized = true;
    out.universalRouter = true;
    out.functionName = "execute";
    out.routerType = RouterType::UNIVERSAL;

    const auto commandsBytes = r.dynamicBytes(0);
    const auto inputs = r.bytesArray(1);
    if (!commandsBytes || !inputs) {
        out.diagnostics.push_back("UNIVERSAL_EXECUTE_DECODE_FAILED");
        return out;
    }

    const std::vector<uint8_t> commands = decodeByteString(*commandsBytes);
    if (commands.empty() || commands.size() != inputs->size()) {
        out.diagnostics.push_back("UNIVERSAL_COMMAND_INPUT_COUNT_MISMATCH");
        return out;
    }

    if (depth >= 6) {
        out.malformed = true;
        out.diagnostics.push_back("UNIVERSAL_MAX_DEPTH");
        return out;
    }

    for (size_t i = 0; i < commands.size(); ++i) {
        const UniversalCommandInfo info = universalCommandInfo(commands[i]);
        if (info.permit2) out.permit2 = true;

        if (info.wrap) {
            DecodedCall child = out;
            child.children.clear();
            child.operation = Operation::WRAP;
            child.functionName = "WRAP_NATIVE";
            child.tokenIn = chainCtx().nativeMarker;
            child.tokenOut = chainCtx().wrappedNative;
            child.complete = true;
            out.children.push_back(std::move(child));
            continue;
        }
        if (info.unwrap) {
            DecodedCall child = out;
            child.children.clear();
            child.operation = Operation::UNWRAP;
            child.functionName = "UNWRAP_NATIVE";
            child.tokenIn = chainCtx().wrappedNative;
            child.tokenOut = chainCtx().nativeMarker;
            child.complete = true;
            out.children.push_back(std::move(child));
            continue;
        }
        if (!info.swap) continue;

        // Universal Router command inputs are ABI tuples without selector.
        // Decode the common fields safely:
        // V2 exact-in/out: recipient, amountIn/Out, amountOutMin/InMax, address[] path, payerIsUser
        // V3 exact-in/out: recipient, amountIn/Out, amountOutMin/InMax, bytes path, payerIsUser
        const std::string synthetic = "0x00000000" + inputs->at(i).substr(2);
        AbiReader ir(synthetic);
        DecodedCall child = out;
        child.children.clear();
        child.operation = info.exactOut ? Operation::SWAP_EXACT_OUT : Operation::SWAP_EXACT_IN;
        child.exactInput = !info.exactOut;
        child.exactOutput = info.exactOut;
        child.functionName = info.v3
            ? (info.exactOut ? "V3_SWAP_EXACT_OUT" : "V3_SWAP_EXACT_IN")
            : (info.exactOut ? "V2_SWAP_EXACT_OUT" : "V2_SWAP_EXACT_IN");

        const auto recipient = ir.address(0);
        const auto amountA = ir.uint256(1);
        const auto amountB = ir.uint256(2);
        if (recipient) child.recipient = *recipient;
        if (amountA) {
            if (info.exactOut) child.amountOut = *amountA;
            else child.amountIn = *amountA;
        }
        if (amountB) {
            if (info.exactOut) child.amountInMax = *amountB;
            else child.amountOutMin = *amountB;
        }

        if (info.v2) {
            const auto path = ir.addressArray(3);
            if (path) child.path = *path;
        } else {
            const auto pathBytes = ir.dynamicBytes(3);
            if (pathBytes) child.path = decodeV3Path(*pathBytes, info.exactOut);
        }

        if (!child.path.empty()) {
            child.tokenIn = child.path.front();
            child.tokenOut = child.path.back();
            child.complete = true;
        }
        out.children.push_back(std::move(child));
    }

    for (const auto& child : out.children) {
        if (child.operation == Operation::SWAP_EXACT_IN ||
            child.operation == Operation::SWAP_EXACT_OUT) {
            out.operation = child.operation;
            out.tokenIn = child.tokenIn;
            out.tokenOut = child.tokenOut;
            out.path = child.path;
            out.amountIn = child.amountIn;
            out.amountOut = child.amountOut;
            out.amountOutMin = child.amountOutMin;
            out.amountInMax = child.amountInMax;
            out.recipient = child.recipient;
            out.exactInput = child.exactInput;
            out.exactOutput = child.exactOutput;
            out.complete = child.complete;
            break;
        }
    }

    if (out.operation == Operation::UNKNOWN) {
        for (const auto& child : out.children) {
            if (child.operation == Operation::WRAP || child.operation == Operation::UNWRAP) {
                out.operation = child.operation;
                out.tokenIn = child.tokenIn;
                out.tokenOut = child.tokenOut;
                out.complete = child.complete;
                break;
            }
        }
    }
    return out;
}

void registerSelectors() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    auto& reg = SelectorRegistry::instance();

    // Uniswap V2-compatible routers.
    reg.registerDecoder("7ff36ab5", [](const json& tx, const AbiReader& r, int) {
        return decodeV2Swap(tx, r, "swapExactETHForTokens", true, true, false, false, 0, 0, 1, 2);
    });
    reg.registerDecoder("fb3bdb41", [](const json& tx, const AbiReader& r, int) {
        return decodeV2Swap(tx, r, "swapETHForExactTokens", false, true, false, false, 0, 0, 1, 2);
    });
    reg.registerDecoder("18cbafe5", [](const json& tx, const AbiReader& r, int) {
        return decodeV2Swap(tx, r, "swapExactTokensForETH", true, false, true, false, 0, 1, 2, 3);
    });
    reg.registerDecoder("4a25d94a", [](const json& tx, const AbiReader& r, int) {
        return decodeV2Swap(tx, r, "swapTokensForExactETH", false, false, true, false, 0, 1, 2, 3);
    });
    reg.registerDecoder("38ed1739", [](const json& tx, const AbiReader& r, int) {
        return decodeV2Swap(tx, r, "swapExactTokensForTokens", true, false, false, false, 0, 1, 2, 3);
    });
    reg.registerDecoder("8803dbee", [](const json& tx, const AbiReader& r, int) {
        return decodeV2Swap(tx, r, "swapTokensForExactTokens", false, false, false, false, 0, 1, 2, 3);
    });
    reg.registerDecoder("b6f9de95", [](const json& tx, const AbiReader& r, int) {
        return decodeV2Swap(tx, r, "swapExactETHForTokensSupportingFeeOnTransferTokens", true, true, false, true, 0, 0, 1, 2);
    });
    reg.registerDecoder("791ac947", [](const json& tx, const AbiReader& r, int) {
        return decodeV2Swap(tx, r, "swapExactTokensForETHSupportingFeeOnTransferTokens", true, false, true, true, 0, 1, 2, 3);
    });
    reg.registerDecoder("5c11d795", [](const json& tx, const AbiReader& r, int) {
        return decodeV2Swap(tx, r, "swapExactTokensForTokensSupportingFeeOnTransferTokens", true, false, false, true, 0, 1, 2, 3);
    });

    // Uniswap V3-compatible routers.
    reg.registerDecoder("414bf389", [](const json& tx, const AbiReader& r, int) {
        return decodeV3ExactInputSingle(tx, r, true);
    });
    reg.registerDecoder("04e45aaf", [](const json& tx, const AbiReader& r, int) {
        return decodeV3ExactInputSingle(tx, r, false);
    });
    reg.registerDecoder("db3e2198", [](const json& tx, const AbiReader& r, int) {
        return decodeV3ExactOutputSingle(tx, r, true);
    });
    reg.registerDecoder("5023b4df", [](const json& tx, const AbiReader& r, int) {
        return decodeV3ExactOutputSingle(tx, r, false);
    });
    reg.registerDecoder("c04b8d59", [](const json& tx, const AbiReader& r, int) {
        return decodeV3ExactInput(tx, r);
    });
    reg.registerDecoder("b858183f", [](const json& tx, const AbiReader& r, int) {
        return decodeV3ExactInput(tx, r);
    });
    reg.registerDecoder("f28c0498", [](const json& tx, const AbiReader& r, int) {
        return decodeV3ExactOutput(tx, r);
    });
    reg.registerDecoder("09b81346", [](const json& tx, const AbiReader& r, int) {
        return decodeV3ExactOutput(tx, r);
    });

    // Wrapped native.
    reg.registerDecoder("d0e30db0", [](const json& tx, const AbiReader& r, int) {
        return decodeWrap(tx, r);
    });
    reg.registerDecoder("2e1a7d4d", [](const json& tx, const AbiReader& r, int) {
        return decodeUnwrap(tx, r);
    });

    // Uniswap V3 multicall variants.
    reg.registerDecoder("ac9650d8", [](const json& tx, const AbiReader& r, int depth) {
        return decodeMulticall(tx, r, depth);
    });
    reg.registerDecoder("5ae401dc", [](const json& tx, const AbiReader& r, int depth) {
        return decodeMulticall(tx, r, depth);
    });

    // Universal Router execute(bytes,bytes[]) and execute(bytes,bytes[],uint256).
    reg.registerDecoder("24856bc3", [](const json& tx, const AbiReader& r, int depth) {
        return decodeUniversalRouter(tx, r, depth);
    });
    reg.registerDecoder("3593564c", [](const json& tx, const AbiReader& r, int depth) {
        return decodeUniversalRouter(tx, r, depth);
    });
}

DecodedCall decodeCallRecursive(const json& tx, int depth) {
    registerSelectors();

    DecodedCall out;
    if (!tx.is_object() || !tx.contains("input") || !tx["input"].is_string()) return out;

    AbiReader reader(tx["input"].get<std::string>());
    if (!reader.valid()) return out;

    out = baseDecoded(tx, reader);
    const DecoderFn* decoder = SelectorRegistry::instance().find(reader.selector());
    if (decoder) return (*decoder)(tx, reader, depth);

    // Known protocol but unsupported function: retain protocol context and use receipt fallback.
    if (!out.protocol.empty() ||
        chainCtx().aggregators.count(out.target) ||
        chainCtx().bridges.count(out.target) ||
        chainCtx().staking.count(out.target)) {
        out.recognized = true;
        out.functionName = "unknown_protocol_call";
        if (chainCtx().aggregators.count(out.target)) out.routerType = RouterType::AGGREGATOR;
        if (chainCtx().bridges.count(out.target)) {
            out.routerType = RouterType::BRIDGE;
            out.operation = Operation::BRIDGE;
        }
        if (chainCtx().staking.count(out.target)) {
            out.routerType = RouterType::STAKING;
            out.operation = Operation::STAKE;
        }
    }
    return out;
}

DecodedCall decodeCall(const json& tx) {
    return decodeCallRecursive(tx, 0);
}

Intent resolveIntent(const DecodedCall& decoded) {
    switch (decoded.operation) {
        case Operation::SWAP_EXACT_IN:
        case Operation::SWAP_EXACT_OUT:
            if (decoded.tokenIn == chainCtx().nativeMarker || isBaseAsset(decoded.tokenIn)) {
                return Intent::BUY;
            }
            if (decoded.tokenOut == chainCtx().nativeMarker || isBaseAsset(decoded.tokenOut)) {
                return Intent::SELL;
            }
            return Intent::TOKEN_SWAP;
        case Operation::WRAP: return Intent::WRAP;
        case Operation::UNWRAP: return Intent::UNWRAP;
        case Operation::ADD_LIQUIDITY: return Intent::ADD_LIQUIDITY;
        case Operation::REMOVE_LIQUIDITY: return Intent::REMOVE_LIQUIDITY;
        case Operation::TRANSFER: return Intent::TRANSFER;
        case Operation::BRIDGE: return Intent::BRIDGE;
        case Operation::STAKE: return Intent::STAKE;
        case Operation::UNSTAKE: return Intent::UNSTAKE;
        case Operation::CLAIM: return Intent::CLAIM;
        case Operation::NFT: return Intent::NFT;
        default: return Intent::UNKNOWN;
    }
}

ParsedReceipt parseReceipt(const json& receipt, const std::string& walletAddress) {
    ParsedReceipt out;
    if (!receipt.is_object() || !receipt.contains("logs") || !receipt["logs"].is_array()) {
        return out;
    }

    const std::string wallet = lowerAddress(walletAddress);
    out.valid = true;

    auto touch = [&](const std::string& token) {
        if (!out.netFlow.count(token)) {
            out.netFlow[token] = 0;
            out.tokenOrder.push_back(token);
        }
    };

    for (const auto& log : receipt["logs"]) {
        if (!log.is_object() ||
            !log.contains("topics") ||
            !log["topics"].is_array() ||
            log["topics"].empty() ||
            !log["topics"][0].is_string()) {
            continue;
        }

        const std::string topic0 = toLower(log["topics"][0].get<std::string>());
        const std::string contract =
            (log.contains("address") && log["address"].is_string())
            ? lowerAddress(log["address"].get<std::string>())
            : std::string();

        if (SWAP_EVENT_TOPICS.count(topic0)) {
            out.hasSwapEvent = true;
            if (!contract.empty()) out.swapPools.insert(contract);
            continue;
        }

        if (contract == chainCtx().wrappedNative &&
            (topic0 == WRAPPED_DEPOSIT_TOPIC || topic0 == WRAPPED_WITHDRAW_TOPIC)) {
            if (log.contains("data") && log["data"].is_string()) {
                const cpp_int amount = parseUint256(log["data"].get<std::string>());
                if (topic0 == WRAPPED_DEPOSIT_TOPIC) out.wrappedNative += amount;
                else out.unwrappedNative += amount;
            }
            continue;
        }

        if (topic0 == V3_INCREASE_LIQUIDITY_TOPIC) {
            out.hasV3Increase = true;
            continue;
        }
        if (topic0 == V3_DECREASE_LIQUIDITY_TOPIC) {
            out.hasV3Decrease = true;
            continue;
        }
        if (topic0 == V3_COLLECT_TOPIC) {
            out.hasV3Collect = true;
            continue;
        }

        if (topic0 != ERC20_TRANSFER_TOPIC ||
            log["topics"].size() != 3 ||
            !log["topics"][1].is_string() ||
            !log["topics"][2].is_string() ||
            !log.contains("data") ||
            !log["data"].is_string() ||
            contract.empty()) {
            continue;
        }

        const std::string from = topicAddress(log["topics"][1].get<std::string>());
        const std::string to = topicAddress(log["topics"][2].get<std::string>());
        if (from != wallet && to != wallet) continue;

        const cpp_int amount = parseUint256(log["data"].get<std::string>());
        if (amount <= 0) continue;

        touch(contract);
        out.hasWalletTransfer = true;

        if (to == wallet) {
            out.netFlow[contract] += amount;
            out.inSources[contract].insert(from);
            if (from == ZERO_ADDRESS) out.mintedTokens.insert(contract);
            else out.counterparties.insert(from);
        }
        if (from == wallet) {
            out.netFlow[contract] -= amount;
            out.outDestinations[contract].insert(to);
            if (to == ZERO_ADDRESS || to == DEAD_ADDRESS) out.burnedTokens.insert(contract);
            else out.counterparties.insert(to);
        }
    }
    return out;
}

std::vector<AssetFlow> buildFlows(const ParsedReceipt& receipt) {
    std::vector<AssetFlow> flows;
    flows.reserve(receipt.netFlow.size());

    for (const auto& [asset, amount] : receipt.netFlow) {
        if (amount == 0) continue;
        AssetFlow flow;
        flow.asset = asset;
        flow.amount = absInt(amount);
        flow.incoming = amount > 0;
        flow.baseAsset = isBaseAsset(asset);

        if (flow.incoming) {
            const auto it = receipt.inSources.find(asset);
            if (it != receipt.inSources.end()) {
                for (const auto& source : it->second) {
                    if (receipt.swapPools.count(source)) {
                        flow.fromSwapPool = true;
                        break;
                    }
                }
            }
        } else {
            const auto it = receipt.outDestinations.find(asset);
            if (it != receipt.outDestinations.end()) {
                for (const auto& destination : it->second) {
                    if (receipt.swapPools.count(destination)) {
                        flow.toSwapPool = true;
                        break;
                    }
                }
            }
        }
        flows.push_back(std::move(flow));
    }
    return flows;
}

std::string resolveVenue(const DecodedCall& decoded, const ParsedReceipt& receipt) {
    if (!decoded.protocol.empty()) return decoded.protocol;

    for (const auto& pool : receipt.swapPools) {
        const std::string label = lookupRouterLabel(pool);
        if (!label.empty()) return label;
    }

    if (decoded.universalRouter) {
        for (const auto& child : decoded.children) {
            if (child.functionName.find("V3_") == 0) return "Universal Router (V3-style)";
            if (child.functionName.find("V2_") == 0) return "Universal Router (V2-style)";
        }
        return "Universal Router";
    }

    if (receipt.hasSwapEvent) return "Unknown DEX pool";
    return {};
}

std::optional<AssetFlow> findFlow(
    const std::vector<AssetFlow>& flows,
    const std::string& asset,
    bool incoming
) {
    const std::string wanted = lowerAddress(asset);
    for (const auto& flow : flows) {
        if (lowerAddress(flow.asset) == wanted && flow.incoming == incoming) return flow;
    }
    return std::nullopt;
}

std::optional<AssetFlow> bestFlow(
    const std::vector<AssetFlow>& flows,
    bool incoming,
    bool baseOnly,
    bool nonBaseOnly,
    bool preferPool
) {
    std::optional<AssetFlow> best;
    for (const auto& flow : flows) {
        if (flow.incoming != incoming) continue;
        if (baseOnly && !flow.baseAsset) continue;
        if (nonBaseOnly && flow.baseAsset) continue;

        if (!best) {
            best = flow;
            continue;
        }

        const bool pool = incoming ? flow.fromSwapPool : flow.toSwapPool;
        const bool bestPool = incoming ? best->fromSwapPool : best->toSwapPool;
        if (preferPool && pool != bestPool) {
            if (pool) best = flow;
            continue;
        }
        if (flow.amount > best->amount) best = flow;
    }
    return best;
}

SemanticMatch semanticMatch(
    const json& tx,
    const DecodedCall& decoded,
    Intent intent,
    const ParsedReceipt& receipt,
    const std::vector<AssetFlow>& flows
) {
    SemanticMatch out;
    out.venue = resolveVenue(decoded, receipt);

    if (intent == Intent::WRAP || intent == Intent::UNWRAP) {
        out.matched = true;
        out.isWrap = intent == Intent::WRAP;
        out.isUnwrap = intent == Intent::UNWRAP;
        out.token = chainCtx().wrappedNative;
        out.tokenAmount = out.isWrap ? receipt.wrappedNative : receipt.unwrappedNative;
        if (out.tokenAmount == 0) {
            out.tokenAmount = decoded.amountIn > 0 ? decoded.amountIn : decoded.nativeValue;
        }
        return out;
    }

    if (intent == Intent::ADD_LIQUIDITY || intent == Intent::REMOVE_LIQUIDITY) {
        out.matched = true;
        out.isLpAdd = intent == Intent::ADD_LIQUIDITY;
        out.isLpRemove = intent == Intent::REMOVE_LIQUIDITY;
        return out;
    }

    if (intent != Intent::BUY && intent != Intent::SELL && intent != Intent::TOKEN_SWAP) {
        return out;
    }

    bool isBuy = intent == Intent::BUY;
    if (intent == Intent::TOKEN_SWAP) {
        // For token-to-token swaps choose direction relative to the most relevant non-base flow.
        const auto incomingNonBase = bestFlow(flows, true, false, true, true);
        const auto outgoingNonBase = bestFlow(flows, false, false, true, true);
        if (incomingNonBase && !outgoingNonBase) isBuy = true;
        else if (!incomingNonBase && outgoingNonBase) isBuy = false;
        else if (!decoded.tokenOut.empty()) {
            const auto expectedOut = findFlow(flows, decoded.tokenOut, true);
            isBuy = expectedOut.has_value();
        }
    }

    std::optional<AssetFlow> tokenFlow;
    std::optional<AssetFlow> counterFlow;

    if (isBuy) {
        if (!decoded.tokenOut.empty() && decoded.tokenOut != chainCtx().nativeMarker) {
            tokenFlow = findFlow(flows, decoded.tokenOut, true);
        }
        if (!tokenFlow) tokenFlow = bestFlow(flows, true, false, true, true);

        if (!decoded.tokenIn.empty() && decoded.tokenIn != chainCtx().nativeMarker) {
            counterFlow = findFlow(flows, decoded.tokenIn, false);
        }
        if (!counterFlow) counterFlow = bestFlow(flows, false, true, false, false);

        if (!counterFlow && decoded.tokenIn == chainCtx().nativeMarker && decoded.nativeValue > 0) {
            AssetFlow native;
            native.asset = chainCtx().nativeMarker;
            native.amount = decoded.nativeValue;
            native.incoming = false;
            native.baseAsset = true;
            counterFlow = native;
        }
    } else {
        if (!decoded.tokenIn.empty() && decoded.tokenIn != chainCtx().nativeMarker) {
            tokenFlow = findFlow(flows, decoded.tokenIn, false);
        }
        if (!tokenFlow) tokenFlow = bestFlow(flows, false, false, true, true);

        if (!decoded.tokenOut.empty() && decoded.tokenOut != chainCtx().nativeMarker) {
            counterFlow = findFlow(flows, decoded.tokenOut, true);
        }
        if (!counterFlow) counterFlow = bestFlow(flows, true, true, false, false);

        // Native output generally does not appear in ERC-20 Transfer logs. WETH withdrawal is the strongest receipt signal.
        if (!counterFlow && decoded.tokenOut == chainCtx().nativeMarker && receipt.unwrappedNative > 0) {
            AssetFlow native;
            native.asset = chainCtx().nativeMarker;
            native.amount = receipt.unwrappedNative;
            native.incoming = true;
            native.baseAsset = true;
            counterFlow = native;
        }
    }

    if (!tokenFlow) {
        out.reason = "DECODED_TOKEN_NOT_FOUND_IN_RECEIPT";
        return out;
    }

    out.matched = true;
    out.isSwap = true;
    out.isBuy = isBuy;
    out.token = tokenFlow->asset;
    out.tokenAmount = tokenFlow->amount;
    if (counterFlow) {
        out.counterToken = counterFlow->asset;
        out.counterAmount = counterFlow->amount;
    }
    return out;
}

SemanticMatch fallbackAnalyze(
    const json& tx,
    const std::string& walletAddress,
    const ParsedReceipt& receipt,
    const std::vector<AssetFlow>& flows
) {
    SemanticMatch out;

    bool anyIn = false;
    bool anyOut = false;
    bool baseIn = false;
    bool baseOut = false;
    for (const auto& flow : flows) {
        if (flow.incoming) {
            anyIn = true;
            if (flow.baseAsset) baseIn = true;
        } else {
            anyOut = true;
            if (flow.baseAsset) baseOut = true;
        }
    }

    bool walletIsSender = false;
    cpp_int nativeOut = 0;
    std::string txTo;
    if (tx.is_object()) {
        if (tx.contains("from") && tx["from"].is_string()) {
            walletIsSender =
                lowerAddress(tx["from"].get<std::string>()) ==
                lowerAddress(walletAddress);
        }
        if (tx.contains("value") && tx["value"].is_string()) {
            nativeOut = hexToCppInt(tx["value"].get<std::string>());
        }
        if (tx.contains("to") && !tx["to"].is_null() && tx["to"].is_string()) {
            txTo = lowerAddress(tx["to"].get<std::string>());
        }
    }

    const bool knownProtocol = !lookupRouterLabel(txTo).empty() ||
                               chainCtx().aggregators.count(txTo) ||
                               chainCtx().bridges.count(txTo);
    const bool swapSignal = receipt.hasSwapEvent || knownProtocol;
    const bool nativeBuySignal = walletIsSender && nativeOut > 0 && swapSignal;
    const bool nativeSellSignal = receipt.unwrappedNative > 0 && receipt.hasSwapEvent;

    // LP stays before swap fallback.
    const bool sentBase = baseOut;
    const bool gotBase = baseIn;
    bool sentNonBase = false;
    bool gotNonBase = false;
    for (const auto& flow : flows) {
        if (flow.baseAsset) continue;
        if (flow.incoming) gotNonBase = true;
        else sentNonBase = true;
    }

    bool lpAdd = receipt.hasV3Increase;
    bool lpRemove = receipt.hasV3Decrease || receipt.hasV3Collect;
    if (!lpAdd) {
        for (const auto& token : receipt.mintedTokens) {
            if (receipt.netFlow.count(token) && receipt.netFlow.at(token) > 0 &&
                sentBase && sentNonBase) {
                lpAdd = true;
                break;
            }
        }
    }
    if (!lpRemove) {
        for (const auto& token : receipt.burnedTokens) {
            if (receipt.netFlow.count(token) && receipt.netFlow.at(token) < 0 &&
                gotBase && gotNonBase) {
                lpRemove = true;
                break;
            }
        }
    }

    if (lpAdd || lpRemove) {
        out.matched = true;
        out.isLpAdd = lpAdd;
        out.isLpRemove = lpRemove;
        out.venue = lpAdd ? "Add Liquidity" : "Remove Liquidity";
        return out;
    }

    const bool isSwap = (anyIn && anyOut) ||
                        (swapSignal && (baseIn || baseOut || nativeBuySignal || nativeSellSignal));

    if (!isSwap) {
        const auto transfer = bestFlow(flows, true, false, false, false)
            ? bestFlow(flows, true, false, false, false)
            : bestFlow(flows, false, false, false, false);
        if (transfer) {
            out.matched = true;
            out.token = transfer->asset;
            out.tokenAmount = transfer->amount;
            out.isBuy = transfer->incoming;
        }
        return out;
    }

    auto buyToken = bestFlow(flows, true, false, true, true);
    auto sellToken = bestFlow(flows, false, false, true, true);

    bool isBuy = false;
    std::optional<AssetFlow> tokenFlow;
    std::optional<AssetFlow> counterFlow;

    if (buyToken && (baseOut || nativeBuySignal)) {
        isBuy = true;
        tokenFlow = buyToken;
        counterFlow = bestFlow(flows, false, true, false, false);
        if (!counterFlow && nativeBuySignal) {
            AssetFlow native;
            native.asset = chainCtx().nativeMarker;
            native.amount = nativeOut;
            native.incoming = false;
            native.baseAsset = true;
            counterFlow = native;
        }
    } else if (sellToken && (baseIn || nativeSellSignal)) {
        isBuy = false;
        tokenFlow = sellToken;
        counterFlow = bestFlow(flows, true, true, false, false);
        if (!counterFlow && nativeSellSignal) {
            AssetFlow native;
            native.asset = chainCtx().nativeMarker;
            native.amount = receipt.unwrappedNative;
            native.incoming = true;
            native.baseAsset = true;
            counterFlow = native;
        }
    } else if (buyToken && sellToken) {
        // Token-to-token: prefer a flow directly connected to a swap pool.
        if (buyToken->fromSwapPool && !sellToken->toSwapPool) {
            isBuy = true;
            tokenFlow = buyToken;
            counterFlow = sellToken;
        } else {
            isBuy = false;
            tokenFlow = sellToken;
            counterFlow = buyToken;
        }
    }

    if (!tokenFlow) {
        out.reason = "NO_COUNTER_FLOW";
        return out;
    }

    out.matched = true;
    out.isSwap = true;
    out.isBuy = isBuy;
    out.token = tokenFlow->asset;
    out.tokenAmount = tokenFlow->amount;
    if (counterFlow) {
        out.counterToken = counterFlow->asset;
        out.counterAmount = counterFlow->amount;
    }
    out.venue = receipt.hasSwapEvent ? "Unknown DEX pool" : lookupRouterLabel(txTo);
    return out;
}

TxResult buildResult(
    const DecodedCall& decoded,
    const ParsedReceipt& receipt,
    const SemanticMatch& match
) {
    TxResult result;
    result.valid = receipt.valid &&
                   (receipt.hasWalletTransfer ||
                    receipt.wrappedNative > 0 ||
                    receipt.unwrappedNative > 0 ||
                    match.matched);

    result.hasSwapEvent = receipt.hasSwapEvent;
    result.isUniversalRouter = decoded.universalRouter;
    result.isGenericMulticall = decoded.multicall;
    result.hasPermit2Signal = decoded.permit2;
    result.dexActivityDetected =
        receipt.hasSwapEvent ||
        decoded.recognized ||
        !decoded.protocol.empty() ||
        decoded.universalRouter ||
        decoded.multicall ||
        decoded.permit2;

    result.lpMintOrBurnSeen =
        !receipt.mintedTokens.empty() || !receipt.burnedTokens.empty();
    result.lpV3EventSeen =
        receipt.hasV3Increase || receipt.hasV3Decrease || receipt.hasV3Collect;

    for (const auto& [token, sources] : receipt.inSources) {
        for (const auto& source : sources) {
            if (receipt.swapPools.count(source)) result.lpPoolIdentitySeen = true;
        }
    }
    for (const auto& [token, destinations] : receipt.outDestinations) {
        for (const auto& destination : destinations) {
            if (receipt.swapPools.count(destination)) result.lpPoolIdentitySeen = true;
        }
    }

    if (!match.matched) {
        result.unknownReason = match.reason.empty() ? "OTHER" : match.reason;
        return result;
    }

    if (match.isLpAdd || match.isLpRemove) {
        result.venue = match.isLpAdd ? "Add Liquidity" : "Remove Liquidity";
        result.isSwap = false;
        result.isBuy = match.isLpAdd;
    } else if (match.isWrap || match.isUnwrap) {
        result.venue = match.isWrap ? "Wrap" : "Unwrap";
        result.isSwap = false;
        result.isBuy = match.isWrap;
        result.tokenAddr = chainCtx().wrappedNative;
        result.rawAmount = match.tokenAmount;
    } else {
        result.venue = match.venue;
        result.isSwap = match.isSwap;
        result.isBuy = match.isBuy;
        result.tokenAddr = match.token;
        result.rawAmount = match.tokenAmount;
        result.counterAddr = match.counterToken;
        result.counterAmount = match.counterAmount;
    }

    if (!result.tokenAddr.empty()) {
        const bool native = result.tokenAddr == chainCtx().nativeMarker;
        const int decimals = native ? 18 : getDecimals(result.tokenAddr);
        const uint64_t price =
            native ? getPriceNanos(chainCtx().wrappedNative)
                   : getPriceNanos(result.tokenAddr);
        result.usdNanos = calcUsdNanos(result.rawAmount, decimals, price);
    }

    if (!result.isSwap &&
        result.venue.empty() &&
        result.dexActivityDetected) {
        result.unknownReason = "NO_COUNTER_FLOW";
    }
    return result;
}

ProtocolInfo protocol(
    std::string name,
    std::string version,
    RouterType type,
    bool v2 = false,
    bool v3 = false,
    bool v4 = false,
    bool permit2 = false,
    bool multicall = false,
    bool universal = false
) {
    ProtocolInfo p;
    p.protocol = std::move(name);
    p.version = std::move(version);
    p.routerType = type;
    p.supportsV2 = v2;
    p.supportsV3 = v3;
    p.supportsV4 = v4;
    p.supportsPermit2 = permit2;
    p.supportsMulticall = multicall;
    p.supportsUniversalRouter = universal;
    return p;
}

void addProtocol(
    ChainContext& c,
    const std::string& address,
    const ProtocolInfo& info
) {
    const std::string a = lowerAddress(address);
    c.protocols[a] = info;
    c.routers[a] = info.protocol +
        (info.version.empty() ? std::string() : " " + info.version);
    if (info.routerType == RouterType::AGGREGATOR) c.aggregators.insert(a);
    if (info.routerType == RouterType::BRIDGE) c.bridges.insert(a);
    if (info.routerType == RouterType::STAKING) c.staking.insert(a);
    if (info.supportsPermit2) c.permit2.insert(a);
    if (info.supportsMulticall) c.multicall.insert(a);
}

ChainContext g_chain;

} // namespace

cpp_int parseUint256(const std::string& h) {
    if (!isHex(h) || h.size() < 66) return 0;
    cpp_int result = 0;
    for (size_t i = 2; i < 66; ++i) {
        const int n = nibble(h[i]);
        if (n < 0) return 0;
        result <<= 4;
        result |= n;
    }
    return result;
}

cpp_int hexToCppInt(const std::string& h) {
    if (!isHex(h)) return 0;
    cpp_int result = 0;
    for (size_t i = 2; i < h.size(); ++i) {
        const int n = nibble(h[i]);
        if (n < 0) return 0;
        result <<= 4;
        result |= n;
    }
    return result;
}

std::string formatAmount(const cpp_int& raw, int dec) {
    if (raw == 0) return "0.00";
    cpp_int divisor = 1;
    for (int i = 0; i < dec; ++i) divisor *= 10;
    std::string integer = (raw / divisor).convert_to<std::string>();
    std::string fraction = (raw % divisor).convert_to<std::string>();
    while (static_cast<int>(fraction.size()) < dec) fraction = "0" + fraction;
    if (fraction.size() > 2) fraction.resize(2);
    return integer + "." + fraction;
}

cpp_int calcUsdNanos(const cpp_int& raw, int dec, uint64_t priceNanos) {
    if (!priceNanos) return 0;
    cpp_int divisor = 1;
    for (int i = 0; i < dec; ++i) divisor *= 10;
    return (raw * priceNanos) / divisor;
}

std::string formatUsd(const cpp_int& nanos) {
    std::string s = nanos.convert_to<std::string>();
    while (s.size() < 10) s = "0" + s;
    std::string dollars = s.substr(0, s.size() - 9);
    std::string cents = s.substr(s.size() - 9, 2);
    if (dollars.empty()) dollars = "0";
    return "$" + dollars + "." + cents;
}

cpp_int calcUnitPriceNanos(
    const cpp_int& usdNanos,
    const cpp_int& rawAmount,
    int dec
) {
    if (rawAmount <= 0) return 0;
    cpp_int divisor = 1;
    for (int i = 0; i < dec; ++i) divisor *= 10;
    return (usdNanos * divisor) / rawAmount;
}

std::string formatPriceUsd(const cpp_int& nanos) {
    const bool negative = nanos < 0;
    const cpp_int absolute = negative ? -nanos : nanos;
    std::string s = absolute.convert_to<std::string>();
    while (s.size() < 10) s = "0" + s;

    std::string dollars = s.substr(0, s.size() - 9);
    std::string fraction = s.substr(s.size() - 9);
    if (dollars.empty()) dollars = "0";

    const size_t lastNonZero = fraction.find_last_not_of('0');
    size_t keep = lastNonZero == std::string::npos ? 0 : lastNonZero + 1;
    if (keep < 2) keep = 2;
    fraction.resize(keep);

    return std::string(negative ? "-$" : "$") + dollars + "." + fraction;
}

const std::string WBNB_ADDR =
    "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c";
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
    c.baseAssets.insert(c.wrappedNative);
    c.baseAssets.insert("0xc5f0f7b66764f6ec8c8dff7ba683102295e16409");

    addProtocol(c, "0x10ed43c718714eb63d5aa57b78b54704e256024e",
                protocol("PancakeSwap", "V2", RouterType::V2, true));
    addProtocol(c, "0x13f4ea83d0bd40e75c8222255bc855a974568dd4",
                protocol("PancakeSwap", "V3 Smart Router", RouterType::V3, true, true, false, false, true));
    addProtocol(c, "0x1b81d678ffb9c0263b24a97847620c99d213eb14",
                protocol("PancakeSwap", "V3 Swap Router", RouterType::V3, false, true, false, false, true));
    addProtocol(c, "0x1a0a18ac4becddbd6389559687d1a73d8927e416",
                protocol("PancakeSwap", "Universal Router", RouterType::UNIVERSAL, true, true, false, true, true, true));
    addProtocol(c, "0xd9c500dff816a1da21a48a732d3498bf09dc9aeb",
                protocol("PancakeSwap", "Universal Router 2", RouterType::UNIVERSAL, true, true, true, true, true, true));
    addProtocol(c, "0x5dc88340e1c5c6366864ee415d6034cadd1a9897",
                protocol("Uniswap", "Universal Router", RouterType::UNIVERSAL, true, true, false, true, true, true));
    addProtocol(c, "0xec8b0f7ffe3ae75d7ffab09429e3675bb63503e4",
                protocol("Uniswap", "Universal Router", RouterType::UNIVERSAL, true, true, false, true, true, true));
    addProtocol(c, "0x1906c1d672b88cd1b9ac7593301ca990f94eae07",
                protocol("Uniswap", "V4 Universal Router", RouterType::UNIVERSAL, false, false, true, true, true, true));

    addProtocol(c, "0x1111111254eeb25477b68fb85ed929f73a960582",
                protocol("1inch", "", RouterType::AGGREGATOR));
    addProtocol(c, "0x9333c74bdd1e118634fe5664aca7a9710b108bab",
                protocol("OKX DEX", "", RouterType::AGGREGATOR));
    addProtocol(c, "0x6015126d7d23648c2e4466693b8deab005ffaba8",
                protocol("OKX DEX", "", RouterType::AGGREGATOR));
    addProtocol(c, "0x6131b5fae19ea4f9d964eac0408e4408b66337b5",
                protocol("KyberSwap", "", RouterType::AGGREGATOR));
    addProtocol(c, "0xdf1a1b60f2d438842916c0adc43748768353ec25",
                protocol("KyberSwap", "", RouterType::AGGREGATOR));
    addProtocol(c, "0x6352a56caadc4f1e25cd6c75970fa768a3304e64",
                protocol("OpenOcean", "", RouterType::AGGREGATOR));
    addProtocol(c, "0x9f138be5aa5cc442ea7cc7d18cd9e30593ed90b9",
                protocol("Odos", "", RouterType::AGGREGATOR));
    addProtocol(c, "0x8f8dd7db1bda5ed3da8c9daf3bfa471c12d58486",
                protocol("DODO", "", RouterType::AGGREGATOR));

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

    addProtocol(c, "0x7a250d5630b4cf539739df2c5dacb4c659f2488d",
                protocol("Uniswap", "V2", RouterType::V2, true));
    addProtocol(c, "0xe592427a0aece92de3edee1f18e0157c05861564",
                protocol("Uniswap", "V3", RouterType::V3, false, true, false, false, true));
    addProtocol(c, "0xef1c6e67703c7bd7107eed8303fbe6ec2554bf6b",
                protocol("Uniswap", "Universal Router", RouterType::UNIVERSAL, true, true, false, true, true, true));
    addProtocol(c, "0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad",
                protocol("Uniswap", "Universal Router", RouterType::UNIVERSAL, true, true, false, true, true, true));
    addProtocol(c, "0x66a9893cc07d91d95644aedd05d03f95e1dba8af",
                protocol("Uniswap", "V4 Universal Router", RouterType::UNIVERSAL, false, false, true, true, true, true));

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

    addProtocol(c, "0x198ef79f1f515f02dfe9e3115ed9fc07183f02fc",
                protocol("Uniswap", "Universal Router", RouterType::UNIVERSAL, true, true, false, true, true, true));
    addProtocol(c, "0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad",
                protocol("Uniswap", "Universal Router", RouterType::UNIVERSAL, true, true, false, true, true, true));
    addProtocol(c, "0x6ff5693b99212da76ad316178a184ab56d299b43",
                protocol("Uniswap", "V4 Universal Router", RouterType::UNIVERSAL, false, false, true, true, true, true));

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

    addProtocol(c, "0x4c60051384bd2d3c01bfc845cf5f4b44bcbe9de5",
                protocol("Uniswap", "Universal Router", RouterType::UNIVERSAL, true, true, false, true, true, true));
    addProtocol(c, "0x3fc91a3afd70395cd496c647d5a6cc9d4b2b7fad",
                protocol("Uniswap", "Universal Router", RouterType::UNIVERSAL, true, true, false, true, true, true));
    addProtocol(c, "0xe592427a0aece92de3edee1f18e0157c05861564",
                protocol("Uniswap", "V3", RouterType::V3, false, true, false, false, true));
    addProtocol(c, "0x68b3465833fb72a70ecdf485e0e4c7bd8665fc45",
                protocol("Uniswap", "V3 Router 2", RouterType::V3, false, true, false, false, true));

    return c;
}

const ChainContext& chainCtx() {
    if (g_chain.displayName.empty()) g_chain = makeBscContext();
    return g_chain;
}

void setChainContext(const ChainContext& ctx) {
    g_chain = ctx;
}

bool isBaseAsset(const std::string& address) {
    return chainCtx().baseAssets.count(lowerAddress(address)) > 0;
}

bool isStablecoin(const std::string& address) {
    return chainCtx().stablecoins.count(lowerAddress(address)) > 0;
}

std::string lookupRouterLabel(const std::string& address) {
    const std::string normalized = lowerAddress(address);
    const auto it = chainCtx().routers.find(normalized);
    return it == chainCtx().routers.end() ? std::string() : it->second;
}

TxResult analyzeTx(
    const json& tx,
    const json& receipt,
    const std::string& walletAddress
) {
    // 1. Decode calldata first.
    const DecodedCall decoded = decodeCall(tx);

    // 2. Resolve requested intent from calldata.
    const Intent intent = resolveIntent(decoded);

    // 3. Parse actual receipt and wallet-relative flows.
    const ParsedReceipt parsed = parseReceipt(receipt, walletAddress);
    if (!parsed.valid) return {};

    // 4. Build normalized asset flows.
    const std::vector<AssetFlow> flows = buildFlows(parsed);

    // 5. Match requested intent to what actually happened.
    SemanticMatch match;
    if (decoded.recognized && intent != Intent::UNKNOWN) {
        match = semanticMatch(tx, decoded, intent, parsed, flows);
    }

    // 6. Existing receipt/net-flow approach remains the safety net.
    if (!match.matched) {
        SemanticMatch fallback = fallbackAnalyze(tx, walletAddress, parsed, flows);
        if (fallback.matched || match.reason.empty()) match = std::move(fallback);
    }

    // 7. Build the unchanged public TxResult.
    return buildResult(decoded, parsed, match);
}
