#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <csignal>
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <sqlite3.h>
#include <boost/multiprecision/cpp_int.hpp>
#include "json.hpp"

using json = nlohmann::json;
using boost::multiprecision::cpp_int;

// ==================== КОНФИГУРАЦИЯ ====================
const std::string TG_TOKEN    = "8845630927:AAEb0Xhm7DUn7ihwPPhZVaaLN3C7hRM2FS0";
const std::string MY_CHAT_ID  = "546348566";
const std::string BSC_RPC     = "https://bsc-dataseed.bnbchain.org";
const std::string CONFIG_FILE = "config.json";
const std::string DB_FILE     = "whale_bot.db";

const long long FAST_SYNC_LAG   = 1000;
const long long REORG_ROLLBACK  = 10;
const long long TX_TTL_BLOCKS   = 200000; // ~7 дней BSC
constexpr int MAX_FAILS_BEFORE_REMOVE = 3;

const std::string SWAP_TOPIC = "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822";
const std::set<std::string> STABLECOINS = {
    "0x55d398326f99059ff775485246999027b3197955", // USDT
    "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d", // USDC
    "0xe9e7cea3dedca5984780bafc599bd69add087d56"  // BUSD
};

// ✅ ЦЕЛОЧИСЛЕННЫЙ ПОРОГ ($1 = 1e9 nanos)
std::atomic<uint64_t> THRESHOLD_NANOS{10000000000000ULL}; // $10,000

// ✅ GRACEFUL SHUTDOWN (async-signal-safe)
std::atomic<bool> running{true};
void signalHandler(int) {
    running.store(false, std::memory_order_relaxed);
}
// ✅ SHARED MUTEX FOR WHALES
std::shared_mutex whalesMutex;
std::vector<std::pair<std::string, std::string>> WHALES;

// ✅ SYNCHRONIZED SUBSCRIBERS
std::shared_mutex subsMutex;
std::shared_ptr<const std::set<std::string>> SUBSCRIBERS_PTR;

// ✅ FAIL COUNTER FOR SMART BROADCAST
std::mutex failCountMutex;
std::map<std::string, int> SUBSCRIBER_FAIL_COUNT;

std::mutex dbMutex;
std::mutex cacheMutex;
sqlite3* db = nullptr;

std::map<std::string, std::string> TOKEN_SYMBOLS;
std::map<std::string, int> TOKEN_DECIMALS;
std::map<std::string, std::pair<uint64_t, time_t>> PRICE_NANOS_CACHE;

// ==================== HTTP & RPC ====================
size_t WriteCB(void* c, size_t s, size_t n, std::string* d) {
    d->append((char*)c, s * n); return s * n;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s;
}

std::string http(const std::string& url, const std::string& post = "", int timeout = 15) {
    CURL* curl = curl_easy_init();
    std::string res;
    if (!curl) return "";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCB);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    if (!post.empty()) {
        struct curl_slist* h = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    }
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res;
}

json rpc(const std::string& method, json params, int maxRetries = 3) {    json r; r["jsonrpc"] = "2.0"; r["method"] = method; r["params"] = params; r["id"] = 1;
    std::string body = r.dump();
    for (int attempt = 0; attempt < maxRetries && running.load(std::memory_order_relaxed); attempt++) {
        auto res = http(BSC_RPC, body);
        try {
            auto parsed = json::parse(res);
            if (parsed.contains("result")) return parsed["result"];
            if (parsed.contains("error")) return nullptr;
        } catch (...) {}
        if (attempt < maxRetries - 1)
            std::this_thread::sleep_for(std::chrono::milliseconds((1 << attempt) * 500));
    }
    return nullptr;
}

// ==================== SQLITE ====================
void initDB() {
    if (sqlite3_open(DB_FILE.c_str(), &db) != SQLITE_OK) {
        std::cerr << "[DB] Cannot open: " << sqlite3_errmsg(db) << std::endl;
        return;
    }
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS subscribers (chat_id TEXT PRIMARY KEY);
        CREATE TABLE IF NOT EXISTS processed_tx (
            tx_hash TEXT PRIMARY KEY,
            block_number INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_processed_block ON processed_tx(block_number);
        CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS token_cache (
            address TEXT PRIMARY KEY, symbol TEXT, decimals INTEGER,
            price_nanos INTEGER, price_ts INTEGER
        );
        PRAGMA journal_mode=WAL;
        PRAGMA synchronous=NORMAL;
    )";
    char* err = nullptr;
    sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) { std::cerr << "[DB] Init error: " << err << std::endl; sqlite3_free(err); }
}

void walCheckpoint() {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
}

void cleanupOldTx(long long currentBlock) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    long long cutoff = currentBlock - TX_TTL_BLOCKS;    sqlite3_prepare_v2(db, "DELETE FROM processed_tx WHERE block_number < ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, cutoff);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void rollbackToBlock(long long targetBlock) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "DELETE FROM processed_tx WHERE block_number > ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, targetBlock);
    sqlite3_step(stmt);
    int deleted = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    std::cerr << "[REORG] Rolled back " << deleted << " txs above block " << targetBlock << std::endl;
}

void loadTokenCache() {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT address,symbol,decimals,price_nanos,price_ts FROM token_cache", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string addr = (const char*)sqlite3_column_text(stmt, 0);
        TOKEN_SYMBOLS[addr] = (const char*)sqlite3_column_text(stmt, 1);
        TOKEN_DECIMALS[addr] = sqlite3_column_int(stmt, 2);
        uint64_t pn = sqlite3_column_int64(stmt, 3);
        time_t ts = sqlite3_column_int64(stmt, 4);
        if (pn > 0) PRICE_NANOS_CACHE[addr] = {pn, ts};
    }
    sqlite3_finalize(stmt);
}

void saveTokenToCache(const std::string& addr, const std::string& sym, int dec, uint64_t pn) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO token_cache VALUES(?,?,?,?,?)", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, addr.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, sym.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, dec);
    sqlite3_bind_int64(stmt, 4, pn);
    sqlite3_bind_int64(stmt, 5, time(nullptr));
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

bool isTxProcessed(const std::string& hash) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT 1 FROM processed_tx WHERE tx_hash=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_STATIC);
    bool e = sqlite3_step(stmt) == SQLITE_ROW;    sqlite3_finalize(stmt); return e;
}

void markTxProcessed(const std::string& hash, long long blockNum) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO processed_tx(tx_hash, block_number) VALUES(?,?)", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, blockNum);
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

// ==================== SUBSCRIBERS ====================
std::shared_ptr<const std::set<std::string>> loadSubscribersFromDB() {
    std::lock_guard<std::mutex> lock(dbMutex);
    auto subs = std::make_shared<std::set<std::string>>();
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT chat_id FROM subscribers", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        subs->insert((const char*)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return subs;
}

void refreshSubscribers() {
    auto newSubs = loadSubscribersFromDB();
    std::unique_lock lock(subsMutex);
    SUBSCRIBERS_PTR = newSubs;
}

void addSubscriber(const std::string& chatId) {
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO subscribers(chat_id) VALUES(?)", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, chatId.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    refreshSubscribers();
}

void removeDeadSubscriber(const std::string& chatId) {
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "DELETE FROM subscribers WHERE chat_id=?", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, chatId.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    {        std::lock_guard<std::mutex> fl(failCountMutex);
        SUBSCRIBER_FAIL_COUNT.erase(chatId);
    }
    refreshSubscribers();
    std::cout << "[SUBS] Removed dead subscriber: " << chatId << std::endl;
}

// ==================== STATE ====================
long long getLastBlock() {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT value FROM state WHERE key='last_block'", -1, &stmt, nullptr);
    long long b = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) b = std::stoll((const char*)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt); return b;
}

void saveLastBlock(long long b) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO state(key,value) VALUES('last_block',?)", -1, &stmt, nullptr);
    std::string v = std::to_string(b);
    sqlite3_bind_text(stmt, 1, v.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

std::string getLastBlockHash() {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT value FROM state WHERE key='last_block_hash'", -1, &stmt, nullptr);
    std::string h;
    if (sqlite3_step(stmt) == SQLITE_ROW) h = (const char*)sqlite3_column_text(stmt, 0);
    sqlite3_finalize(stmt); return h;
}

void saveLastBlockHash(const std::string& h) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO state(key,value) VALUES('last_block_hash',?)", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, h.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

// ==================== CONFIG ====================
void loadConfig() {
    std::ifstream f(CONFIG_FILE);
    if (!f.is_open()) return;
    try {
        json j = json::parse(f);
        double thresh = j.value("threshold", 10000.0);        THRESHOLD_NANOS.store((uint64_t)(thresh * 1000000000.0));
        std::unique_lock lock(whalesMutex);
        WHALES.clear();
        for (auto& w : j["whales"]) WHALES.push_back({toLower(w["address"]), w["name"]});
        std::cout << "[CONFIG] Loaded " << WHALES.size() << " whales, threshold $" << thresh << std::endl;
    } catch (...) { std::cerr << "[CONFIG] Parse error!" << std::endl; }
}

void saveConfig() {
    json j;
    j["threshold"] = (double)THRESHOLD_NANOS.load() / 1000000000.0;
    j["whales"] = json::array();
    {
        std::shared_lock lock(whalesMutex);
        for (auto& [a, n] : WHALES) j["whales"].push_back({{"address", a}, {"name", n}});
    }
    std::ofstream(CONFIG_FILE) << j.dump(4);
}

// ==================== TELEGRAM ====================
struct SendResult { bool ok; bool deadUser; };

SendResult sendMsg(const std::string& chatId, const std::string& text) {
    json j; j["chat_id"] = chatId; j["text"] = text;
    j["parse_mode"] = "HTML"; j["disable_web_page_preview"] = true;
    auto res = http("https://api.telegram.org/bot" + TG_TOKEN + "/sendMessage", j.dump());
    try {
        auto r = json::parse(res);
        if (r.value("ok", false)) return {true, false};
        int code = r.value("error_code", 0);
        if (code == 429) return {false, false};       // Rate limit — не считаем dead
        if (code == 403 || code == 400) return {false, true}; // Blocked / deactivated
        return {false, false};
    } catch (...) { return {false, false}; }
}

bool broadcast(const std::string& text) {
    std::shared_ptr<const std::set<std::string>> subs;
    { std::shared_lock lock(subsMutex); subs = SUBSCRIBERS_PTR; }
    if (!subs || subs->empty()) return true;

    int total = subs->size();
    int success = 0;
    bool adminOk = false;
    std::vector<std::string> toRemove;

    for (auto& id : *subs) {
        auto [ok, dead] = sendMsg(id, text);
        if (ok) {
            success++;            if (id == MY_CHAT_ID) adminOk = true;
            std::lock_guard<std::mutex> fl(failCountMutex);
            SUBSCRIBER_FAIL_COUNT[id] = 0;
        } else if (dead) {
            std::lock_guard<std::mutex> fl(failCountMutex);
            SUBSCRIBER_FAIL_COUNT[id]++;
            if (SUBSCRIBER_FAIL_COUNT[id] >= MAX_FAILS_BEFORE_REMOVE)
                toRemove.push_back(id);
        }
    }

    for (auto& d : toRemove) removeDeadSubscriber(d);

    double ratio = (total > 0) ? (double)success / total : 1.0;
    return adminOk || (ratio >= 0.5);
}

// ==================== TOKENS & PRICES ====================
int getDecimals(const std::string& addr) {
    std::string a = toLower(addr);
    { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_DECIMALS.count(a)) return TOKEN_DECIMALS[a]; }
    auto res = rpc("eth_call", {{{"to", addr}, {"data", "0x313ce567"}}, "latest"});
    int dec = 18;
    if (res.is_string() && res.get<std::string>().length() >= 66)
        try { dec = std::stoi(res.get<std::string>().substr(2), nullptr, 16); } catch (...) {}
    { std::lock_guard<std::mutex> l(cacheMutex); TOKEN_DECIMALS[a] = dec; }
    saveTokenToCache(a, "", dec, 0);
    return dec;
}

// ✅ STRING + BYTES32 FALLBACK
std::string getSymbol(const std::string& addr) {
    std::string a = toLower(addr);
    { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_SYMBOLS.count(a)) return TOKEN_SYMBOLS[a]; }

    auto res = rpc("eth_call", {{{"to", addr}, {"data", "0x95d89b41"}}, "latest"});
    std::string sym;

    // Попытка 1: ABI-encoded string
    if (res.is_string() && res.get<std::string>().length() > 130) {
        try {
            std::string hex = res.get<std::string>().substr(2);
            int len = std::stoi(hex.substr(64, 64), nullptr, 16);
            if (len > 0 && len <= 32) {
                std::string strHex = hex.substr(128, len * 2);
                for (size_t i = 0; i < strHex.length(); i += 2)
                    sym += (char)std::stoi(strHex.substr(i, 2), nullptr, 16);
            }
        } catch (...) {}
    }
    // Попытка 2: bytes32 (старые/нестандартные контракты)
    if (sym.empty() && res.is_string() && res.get<std::string>().length() >= 66) {
        try {
            std::string hex = res.get<std::string>().substr(2, 64);
            for (size_t i = 0; i < hex.length(); i += 2) {
                char c = (char)std::stoi(hex.substr(i, 2), nullptr, 16);
                if (c == '\0') break;
                sym += c;
            }
        } catch (...) {}
    }

    if (sym.empty()) sym = "UNKNOWN";
    { std::lock_guard<std::mutex> l(cacheMutex); TOKEN_SYMBOLS[a] = sym; }
    saveTokenToCache(a, sym, getDecimals(a), 0);
    return sym;
}

uint64_t getPriceNanos(const std::string& token) {
    std::string a = toLower(token);
    {
        std::lock_guard<std::mutex> l(cacheMutex);
        if (PRICE_NANOS_CACHE.count(a) && time(nullptr) - PRICE_NANOS_CACHE[a].second < 300)
            return PRICE_NANOS_CACHE[a].first;
    }
    auto res = http("https://api.dexscreener.com/latest/dex/tokens/" + token);
    double p = 0;
    try {
        auto j = json::parse(res);
        if (j.contains("pairs") && !j["pairs"].empty())
            p = std::stod(j["pairs"][0]["priceUsd"].get<std::string>());
    } catch (...) {}

    uint64_t nanos = (uint64_t)(p * 1000000000.0);
    if (nanos > 0) {
        { std::lock_guard<std::mutex> l(cacheMutex); PRICE_NANOS_CACHE[a] = {nanos, time(nullptr)}; }
        saveTokenToCache(a, "", getDecimals(a), nanos);
    }
    return nanos;
}

// ==================== MATH ====================
cpp_int parseUint256(const std::string& hexData) {
    if (hexData.length() < 66) return 0;
    cpp_int raw = 0;
    for (char c : hexData.substr(2, 64)) {
        raw <<= 4;
        if (c >= '0' && c <= '9')      raw |= (c - '0');
        else if (c >= 'a' && c <= 'f') raw |= (c - 'a' + 10);        else if (c >= 'A' && c <= 'F') raw |= (c - 'A' + 10);
    }
    return raw;
}

std::string formatAmount(const cpp_int& raw, int decimals) {
    if (raw == 0) return "0.00";

    cpp_int div = 1;
    for (int i = 0; i < decimals; i++)
        div *= 10;

    cpp_int ip_val = raw / div;
    cpp_int fp_val = raw % div;

    std::string ip = ip_val.convert_to<std::string>();
    std::string fp = fp_val.convert_to<std::string>();

    while ((int)fp.length() < decimals)
        fp = "0" + fp;

    if (fp.length() > 2)
        fp = fp.substr(0, 2);

    return ip + "." + fp;
}

cpp_int calcUsdNanos(const cpp_int& raw, int decimals, uint64_t priceNanos) {
    if (priceNanos == 0) return 0;
    cpp_int div = 1; for (int i = 0; i < decimals; i++) div *= 10;
    return (raw * priceNanos) / div;
}

std::string formatUsd(const cpp_int& nanos) {
    std::string s = nanos.convert_to<std::string>();
    while (s.length() < 10) s = "0" + s;
    std::string dollars = s.substr(0, s.length() - 9);
    std::string cents = s.substr(s.length() - 9, 2);
    if (dollars.empty()) dollars = "0";
    return "$" + dollars + "." + cents;
}

// ==================== ANALYZE TX ====================
struct TxResult {
    bool valid, isSwap, isBuy;
    cpp_int rawAmount, usdNanos;
    std::string tokenAddr;
};

TxResult analyzeTx(const json& receipt, const std::string& whaleAddr) {
    TxResult r = {};
    if (receipt.is_null() || !receipt.contains("logs")) return r;

    bool hasSwapEvent = false;
    for (auto& log : receipt["logs"]) {
        if (log["topics"].size() >= 1 && log["topics"][0] == SWAP_TOPIC) {
            hasSwapEvent = true; break;
        }
    }

    struct TI { cpp_int amt; std::string tok; bool in; };
    std::vector<TI> trs;
    for (auto& log : receipt["logs"]) {
        if (log["topics"].size() < 3) continue;
        if (log["topics"][0] != "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef") continue;
        std::string from = "0x" + toLower(log["topics"][1].get<std::string>().substr(26));
        std::string to   = "0x" + toLower(log["topics"][2].get<std::string>().substr(26));
        std::string tok  = toLower(log["address"]);
        if (from != whaleAddr && to != whaleAddr) continue;
        trs.push_back({parseUint256(log["data"].get<std::string>()), tok, to == whaleAddr});
    }

    if (trs.empty()) return r;
    r.valid = true;

    bool hasIn = false, hasOut = false;
    for (auto& t : trs) { if (t.in) hasIn = true; else hasOut = true; }
    r.isSwap = hasSwapEvent && hasIn && hasOut;

    cpp_int stIn = 0, stOut = 0;
    std::string nsTok; cpp_int nsAmt; bool nsIn = false;
    for (auto& t : trs) {
        if (STABLECOINS.count(t.tok)) { if (t.in) stIn += t.amt; else stOut += t.amt; }
        else { nsTok = t.tok; nsAmt = t.amt; nsIn = t.in; }
    }

    if (r.isSwap) {
        if (stOut > 0 && stIn == 0)      { r.isBuy = true;  r.tokenAddr = nsTok.empty() ? trs.back().tok : nsTok; r.rawAmount = nsIn ? nsAmt : trs.front().amt; }
        else if (stIn > 0 && stOut == 0) { r.isBuy = false; r.tokenAddr = nsTok.empty() ? trs.front().tok : nsTok; r.rawAmount = nsIn ? nsAmt : trs.front().amt; }
        else                             { r.isBuy = stIn > stOut; r.tokenAddr = trs.front().tok; r.rawAmount = trs.front().amt; }
    } else {
        r.isBuy = hasIn;
        r.tokenAddr = trs.front().tok;
        r.rawAmount = trs.front().amt;
    }

    int dec = getDecimals(r.tokenAddr);
    uint64_t pn = getPriceNanos(r.tokenAddr);
    r.usdNanos = calcUsdNanos(r.rawAmount, dec, pn);
    return r;
}

// ==================== PROCESS BLOCK ====================
bool processBlock(long long blockNum) {
    std::stringstream ss; ss << "0x" << std::hex << blockNum;
    auto block = rpc("eth_getBlockByNumber", {ss.str(), true});
    if (block.is_null() || !block.contains("transactions")) return false;

    // ✅ REORG CHECK + ROLLBACK
    std::string parentHash = block.value("parentHash", "");
    std::string expectedParent = getLastBlockHash();    if (!expectedParent.empty() && parentHash != expectedParent && blockNum > 1) {
        std::cerr << "[REORG] Detected at block " << blockNum
                  << ", rolling back " << REORG_ROLLBACK << " blocks" << std::endl;
        rollbackToBlock(blockNum - REORG_ROLLBACK - 1);
        saveLastBlock(blockNum - REORG_ROLLBACK - 1);
        saveLastBlockHash("");
        return false;
    }

    for (auto& tx : block["transactions"]) {
        if (!running.load(std::memory_order_relaxed)) return false;

        std::string hash = tx["hash"].get<std::string>();
        if (isTxProcessed(hash)) continue;

        std::string from = toLower(tx["from"].get<std::string>());
        std::string to = tx["to"].is_null() ? "" : toLower(tx["to"].get<std::string>());

        std::string mA, mN;
        {
            std::shared_lock lock(whalesMutex);
            for (auto& [wA, wN] : WHALES) {
                if (from == wA || to == wA) { mA = wA; mN = wN; break; }
            }
        }

        if (mA.empty()) { markTxProcessed(hash, blockNum); continue; }

        auto receipt = rpc("eth_getTransactionReceipt", {hash});
        if (receipt.is_null()) continue; // Retry next cycle

        TxResult res = analyzeTx(receipt, mA);
        if (!res.valid) { markTxProcessed(hash, blockNum); continue; }

        // ✅ Целочисленное сравнение без float
        cpp_int threshNanos = (cpp_int)THRESHOLD_NANOS.load();
        if (res.usdNanos < threshNanos) { markTxProcessed(hash, blockNum); continue; }

        std::string msg = "🐋 <b>" + mN + "</b>\n\n";
        if (res.isSwap) {
            msg += res.isBuy ? "🟢 <b>ПОКУПКА</b>" : "🔴 <b>ПРОДАЖА</b>";
            msg += "\n💰 Сумма: <b>" + formatUsd(res.usdNanos) + "</b>\n";
            msg += "🪙 Монета: <b>" + getSymbol(res.tokenAddr) + "</b>\n";
            msg += "📦 Кол-во: <b>" + formatAmount(res.rawAmount, getDecimals(res.tokenAddr)) + "</b>\n";
        } else {
            msg += "📤 <b>ПЕРЕВОД</b>\n";
            msg += "💰 Сумма: <b>" + formatUsd(res.usdNanos) + "</b>\n";
            msg += "🪙 Монета: <b>" + getSymbol(res.tokenAddr) + "</b>\n";
            msg += "📦 Кол-во: <b>" + formatAmount(res.rawAmount, getDecimals(res.tokenAddr)) + "</b>\n";
        }        msg += "📜 Контракт: <code>" + res.tokenAddr + "</code>\n\n";
        msg += "🔗 <a href=\"https://bscscan.com/tx/" + hash + "\">Транзакция</a>";

        // ✅ Mark ONLY after successful broadcast
        if (broadcast(msg)) {
            markTxProcessed(hash, blockNum);
            std::cout << "[OK] " << mN << " " << formatUsd(res.usdNanos)
                      << " " << getSymbol(res.tokenAddr) << std::endl;
        } else {
            std::cerr << "[WARN] Broadcast failed for " << hash << ", will retry" << std::endl;
        }
    }

    saveLastBlockHash(block.value("hash", ""));
    return true;
}

// ==================== TELEGRAM LOOP ====================
void telegramLoop() {
    long offset = 0;
    while (running.load(std::memory_order_relaxed)) {
        try {
            auto raw = http("https://api.telegram.org/bot" + TG_TOKEN
                            + "/getUpdates?offset=" + std::to_string(offset));
            if (raw.empty()) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
            auto upd = json::parse(raw);
            for (auto& u : upd["result"]) {
                offset = u["update_id"].get<long>() + 1;
                if (!u.contains("message") || !u["message"].contains("text")) continue;
                std::string txt = u["message"]["text"];
                std::string cid = std::to_string(u["message"]["chat"]["id"].get<long>());

                addSubscriber(cid);
                if (txt == "/start") sendMsg(cid, "✅ Подписка активна!");

                if (cid == MY_CHAT_ID) {
                    if (txt.find("/add ") == 0) {
                        size_t p1 = txt.find(' '), p2 = txt.find(' ', p1 + 1);
                        if (p1 != std::string::npos && p2 != std::string::npos) {
                            std::unique_lock lock(whalesMutex);
                            WHALES.push_back({toLower(txt.substr(p1 + 1, p2 - p1 - 1)), txt.substr(p2 + 1)});
                            lock.unlock();
                            saveConfig(); sendMsg(cid, "✅ Кит добавлен");
                        } else sendMsg(cid, "❌ Формат: /add 0x... Имя");
                    }
                    else if (txt.find("/remove ") == 0) {
                        std::string addr = toLower(txt.substr(8));
                        std::unique_lock lock(whalesMutex);
                        WHALES.erase(std::remove_if(WHALES.begin(), WHALES.end(),
                            [&](auto& w) { return w.first == addr; }), WHALES.end());                        lock.unlock();
                        saveConfig(); sendMsg(cid, "✅ Кит удален");
                    }
                    else if (txt.find("/limit ") == 0) {
                        try {
                            double val = std::stod(txt.substr(7));
                            THRESHOLD_NANOS.store((uint64_t)(val * 1000000000.0));
                            saveConfig();
                            sendMsg(cid, "✅ Лимит: $" + std::to_string((uint64_t)val));
                        } catch (...) { sendMsg(cid, "❌ Укажи число"); }
                    }
                    else if (txt == "/list") {
                        std::shared_lock lock(whalesMutex);
                        uint64_t thresh = THRESHOLD_NANOS.load();
                        std::string msg = "💰 Лимит: $" + std::to_string(thresh / 1000000000ULL) + "\n\n";
                        for (auto& [a, n] : WHALES) msg += "• " + n + "\n<code>" + a + "</code>\n\n";
                        lock.unlock();
                        sendMsg(cid, msg);
                    }
                }
            }
        } catch (...) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ==================== MAIN ====================
int main() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    initDB(); loadConfig(); loadTokenCache();

    SUBSCRIBERS_PTR = loadSubscribersFromDB();
    if (SUBSCRIBERS_PTR->empty()) addSubscriber(MY_CHAT_ID);

    long long lastBlock = getLastBlock();
    if (lastBlock == 0) {
        auto b = rpc("eth_blockNumber", {});
        if (b.is_string()) lastBlock = std::stoll(b.get<std::string>(), nullptr, 16);
    }

    // ✅ SAFE FAST SYNC
    auto latestJson = rpc("eth_blockNumber", {});
    if (latestJson.is_string()) {
        long long latest = std::stoll(latestJson.get<std::string>(), nullptr, 16);
        if (latest - lastBlock > FAST_SYNC_LAG) {
            std::cout << "[FAST SYNC] Lag " << (latest - lastBlock)
                      << " blocks, skipping to latest-5" << std::endl;            lastBlock = latest - 5;
            saveLastBlock(lastBlock);
            saveLastBlockHash("");
        }
    }

    uint64_t threshDisplay = THRESHOLD_NANOS.load() / 1000000000ULL;
    std::cout << "🐋 Bot started. Block: " << lastBlock
              << ", Subs: " << SUBSCRIBERS_PTR->size()
              << ", Whales: " << WHALES.size()
              << ", Threshold: $" << threshDisplay << std::endl;

    std::thread tgThread(telegramLoop);
    long long blocksSinceCheckpoint = 0;

    while (running.load(std::memory_order_relaxed)) {
        try {
            auto lj = rpc("eth_blockNumber", {});
            if (!lj.is_string()) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
            long long latest = std::stoll(lj.get<std::string>(), nullptr, 16);

            while (lastBlock < latest && running.load(std::memory_order_relaxed)) {
                lastBlock++;
                if (processBlock(lastBlock)) {
                    saveLastBlock(lastBlock);
                    if (++blocksSinceCheckpoint >= 1000) {
                        walCheckpoint();
                        cleanupOldTx(lastBlock);
                        blocksSinceCheckpoint = 0;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // ✅ GRACEFUL SHUTDOWN SEQUENCE
    std::cout << "[SHUTDOWN] Signal received, stopping gracefully..." << std::endl;
    std::cout << "[SHUTDOWN] Waiting for Telegram thread..." << std::endl;
    tgThread.join();
    std::cout << "[SHUTDOWN] Final WAL checkpoint..." << std::endl;
    walCheckpoint();
    std::cout << "[SHUTDOWN] Closing database..." << std::endl;
    if (db) sqlite3_close(db);
    curl_global_cleanup();
    std::cout << "[SHUTDOWN] Clean exit." << std::endl;
    return 0;
}
