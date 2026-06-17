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
#include <cstdlib>
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <sqlite3.h>
#include <boost/multiprecision/cpp_int.hpp>
#include "json.hpp"

using json = nlohmann::json;
using boost::multiprecision::cpp_int;

// ==================== УТИЛИТЫ ====================
std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string safeColumnText(sqlite3_stmt* stmt, int col) {
    const unsigned char* p = sqlite3_column_text(stmt, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}

bool prepareOrLog(sqlite3* db, sqlite3_stmt** stmt, const char* sql) {
    int rc = sqlite3_prepare_v2(db, sql, -1, stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] prepare failed: " << sqlite3_errmsg(db)
                  << " | SQL: " << sql << std::endl;
        return false;
    }
    return true;
}

// ==================== КОНФИГУРАЦИЯ ====================
const std::string TG_TOKEN = []{
    const char* env = std::getenv("8845630927:AAEb0Xhm7DUn7ihwPPhZVaaLN3C7hRM2FS0");
    if (!env || std::string(env).empty()) {        std::cerr << "[FATAL] WHALE_TG_TOKEN environment variable not set!" << std::endl;
        std::exit(1);
    }
    return std::string(env);
}();

const std::string MY_CHAT_ID  = "546348566";
const std::string BSC_RPC     = "https://bsc-dataseed.bnbchain.org";
const std::string CONFIG_FILE = "config.json";
const std::string DB_FILE     = "whale_bot.db";

const long long FAST_SYNC_LAG   = 1000;
const long long REORG_ROLLBACK  = 10;
const long long TX_TTL_BLOCKS   = 200000;
constexpr int MAX_FAILS_BEFORE_REMOVE = 3;

const std::string SWAP_TOPIC = "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822";

const std::set<std::string> BASE_ASSETS = {
    "0x55d398326f99059ff775485246999027b3197955", // USDT
    "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d", // USDC
    "0xe9e7cea3dedca5984780bafc599bd69add087d56", // BUSD
    "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c", // WBNB
    "0xc5f0f7b66764f6ec8c8dff7ba683102295e16409"  // FDUSD
};

bool isBaseAsset(const std::string& addr) {
    return BASE_ASSETS.count(toLower(addr)) > 0;
}

std::atomic<uint64_t> THRESHOLD_NANOS{10000000000000ULL};

std::atomic<bool> running{true};
void signalHandler(int) { running.store(false, std::memory_order_relaxed); }

std::shared_mutex whalesMutex;
std::vector<std::pair<std::string, std::string>> WHALES;

std::shared_mutex subsMutex;
std::shared_ptr<const std::set<std::string>> SUBSCRIBERS_PTR;

std::mutex failCountMutex;
std::map<std::string, int> SUBSCRIBER_FAIL_COUNT;

std::mutex dbMutex;
std::mutex cacheMutex;
sqlite3* db = nullptr;

std::map<std::string, std::string> TOKEN_SYMBOLS;
std::map<std::string, int> TOKEN_DECIMALS;std::map<std::string, std::pair<uint64_t, time_t>> PRICE_NANOS_CACHE;

// ==================== HTTP & RPC ====================
size_t WriteCB(void* c, size_t s, size_t n, std::string* d) {
    d->append((char*)c, s * n); return s * n;
}

std::string http(const std::string& url, const std::string& post = "", int timeout = 15) {
    CURL* curl = curl_easy_init();
    std::string res;
    if (!curl) return "";

    struct curl_slist* h = nullptr;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCB);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    if (!post.empty()) {
        h = curl_slist_append(h, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    }

    CURLcode rc = curl_easy_perform(curl);

    auto safeUrl = [&]() -> std::string {
        if (url.find("api.telegram.org") != std::string::npos) return "telegram_api";
        if (url.length() <= 80) return url;
        return url.substr(0, 80);
    };

    if (rc != CURLE_OK) {
        std::cerr << "[HTTP] " << curl_easy_strerror(rc)
                  << " | URL: " << safeUrl() << std::endl;
    } else {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        if (httpCode >= 500) {
            std::cerr << "[HTTP] Server error " << httpCode
                      << " | URL: " << safeUrl() << std::endl;
        }
    }

    if (h) curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    return res;
}
json rpc(const std::string& method, json params, int maxRetries = 3) {
    json r; r["jsonrpc"] = "2.0"; r["method"] = method; r["params"] = params; r["id"] = 1;
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
        std::cerr << "[FATAL] Cannot open database: " << sqlite3_errmsg(db) << std::endl;
        std::exit(1);
    }
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS subscribers (chat_id TEXT PRIMARY KEY);
        CREATE TABLE IF NOT EXISTS processed_tx (
            tx_hash TEXT PRIMARY KEY, block_number INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_processed_block ON processed_tx(block_number);
        CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS token_cache (
            address TEXT PRIMARY KEY,
            symbol TEXT DEFAULT '',
            decimals INTEGER DEFAULT 0,
            price_nanos INTEGER DEFAULT 0,
            price_ts INTEGER DEFAULT 0
        );
        PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;
    )";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[FATAL] Database schema init failed: " << err << std::endl;
        sqlite3_free(err);
        sqlite3_close(db);
        std::exit(1);
    }
}

void walCheckpoint() {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);}

void cleanupOldTx(long long currentBlock) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "DELETE FROM processed_tx WHERE block_number < ?")) return;
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(currentBlock - TX_TTL_BLOCKS));
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

void rollbackToBlock(long long targetBlock) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "DELETE FROM processed_tx WHERE block_number > ?")) return;
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(targetBlock));
    sqlite3_step(stmt); sqlite3_finalize(stmt);
    std::cerr << "[REORG] Rolled back txs above block " << targetBlock << std::endl;
}

void loadTokenCache() {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "SELECT address,symbol,decimals,price_nanos,price_ts FROM token_cache")) return;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string addr = safeColumnText(stmt, 0);
        std::string sym = safeColumnText(stmt, 1);
        if (!sym.empty()) TOKEN_SYMBOLS[addr] = sym;
        int dec = sqlite3_column_int(stmt, 2);
        if (dec > 0) TOKEN_DECIMALS[addr] = dec;
        uint64_t pn = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        time_t ts = static_cast<time_t>(sqlite3_column_int64(stmt, 4));
        if (pn > 0) PRICE_NANOS_CACHE[addr] = {pn, ts};
    }
    sqlite3_finalize(stmt);
}

void saveTokenMetadata(const std::string& addr, const std::string& sym, int dec) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    const char* sql = R"(
        INSERT INTO token_cache(address, symbol, decimals) VALUES(?, ?, ?)
        ON CONFLICT(address) DO UPDATE SET
            symbol = CASE WHEN excluded.symbol != '' THEN excluded.symbol ELSE token_cache.symbol END,
            decimals = CASE WHEN excluded.decimals > 0 THEN excluded.decimals ELSE token_cache.decimals END
    )";
    if (!prepareOrLog(db, &stmt, sql)) return;
    sqlite3_bind_text(stmt, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, sym.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, dec);
    if (sqlite3_step(stmt) != SQLITE_DONE)        std::cerr << "[DB] saveTokenMetadata step failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(stmt);
}

void saveTokenPrice(const std::string& addr, uint64_t pn) {
    if (pn == 0) return;
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    const char* sql = R"(
        INSERT INTO token_cache(address, price_nanos, price_ts) VALUES(?, ?, ?)
        ON CONFLICT(address) DO UPDATE SET
            price_nanos = excluded.price_nanos,
            price_ts = excluded.price_ts
    )";
    if (!prepareOrLog(db, &stmt, sql)) return;
    sqlite3_bind_text(stmt, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(pn));
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(time(nullptr)));
    if (sqlite3_step(stmt) != SQLITE_DONE)
        std::cerr << "[DB] saveTokenPrice step failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(stmt);
}

bool isTxProcessed(const std::string& hash) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "SELECT 1 FROM processed_tx WHERE tx_hash=?")) return false;
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    bool e = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt); return e;
}

void markTxProcessed(const std::string& hash, long long blockNum) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "INSERT OR IGNORE INTO processed_tx(tx_hash,block_number) VALUES(?,?)")) return;
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(blockNum));
    if (sqlite3_step(stmt) != SQLITE_DONE)
        std::cerr << "[DB] markTxProcessed step failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(stmt);
}

// ==================== SUBSCRIBERS ====================
std::shared_ptr<const std::set<std::string>> loadSubscribersFromDB() {
    std::lock_guard<std::mutex> lock(dbMutex);
    auto subs = std::make_shared<std::set<std::string>>();
    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "SELECT chat_id FROM subscribers")) return subs;
    while (sqlite3_step(stmt) == SQLITE_ROW)        subs->insert(safeColumnText(stmt, 0));
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
        if (!prepareOrLog(db, &stmt, "INSERT OR IGNORE INTO subscribers(chat_id) VALUES(?)")) return;
        sqlite3_bind_text(stmt, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    refreshSubscribers();
}

void removeDeadSubscriber(const std::string& chatId) {
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        sqlite3_stmt* stmt;
        if (!prepareOrLog(db, &stmt, "DELETE FROM subscribers WHERE chat_id=?")) return;
        sqlite3_bind_text(stmt, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    { std::lock_guard<std::mutex> fl(failCountMutex); SUBSCRIBER_FAIL_COUNT.erase(chatId); }
    refreshSubscribers();
    std::cout << "[SUBS] Removed dead subscriber: " << chatId << std::endl;
}

// ==================== STATE ====================
long long getLastBlock() {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "SELECT value FROM state WHERE key='last_block'")) return 0;
    long long b = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string val = safeColumnText(stmt, 0);
        try { if (!val.empty()) b = std::stoll(val); } catch (...) {}
    }
    sqlite3_finalize(stmt); return b;
}

void saveLastBlock(long long b) {
    std::lock_guard<std::mutex> lock(dbMutex);    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "INSERT OR REPLACE INTO state(key,value) VALUES('last_block',?)")) return;
    std::string v = std::to_string(b);
    sqlite3_bind_text(stmt, 1, v.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE)
        std::cerr << "[DB] saveLastBlock step failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(stmt);
}

std::string getLastBlockHash() {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "SELECT value FROM state WHERE key='last_block_hash'")) return "";
    std::string h = (sqlite3_step(stmt) == SQLITE_ROW) ? safeColumnText(stmt, 0) : "";
    sqlite3_finalize(stmt); return h;
}

void saveLastBlockHash(const std::string& h) {
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* stmt;
    if (!prepareOrLog(db, &stmt, "INSERT OR REPLACE INTO state(key,value) VALUES('last_block_hash',?)")) return;
    sqlite3_bind_text(stmt, 1, h.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE)
        std::cerr << "[DB] saveLastBlockHash step failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(stmt);
}

// ==================== CONFIG ====================
void loadConfig() {
    std::ifstream f(CONFIG_FILE);
    if (!f.is_open()) return;
    try {
        json j = json::parse(f);
        double thresh = j.value("threshold", 10000.0);
        THRESHOLD_NANOS.store(static_cast<uint64_t>(thresh * 1000000000.0));
        std::unique_lock lock(whalesMutex);
        WHALES.clear();
        for (auto& w : j["whales"]) WHALES.push_back({toLower(w["address"]), w["name"]});
        std::cout << "[CONFIG] Loaded " << WHALES.size() << " whales, threshold $" << thresh << std::endl;
    } catch (...) { std::cerr << "[CONFIG] Parse error!" << std::endl; }
}

void saveConfig() {
    json j;
    j["threshold"] = static_cast<double>(THRESHOLD_NANOS.load()) / 1000000000.0;
    j["whales"] = json::array();
    {
        std::shared_lock lock(whalesMutex);
        for (auto& [a, n] : WHALES) j["whales"].push_back({{"address", a}, {"name", n}});
    }    std::ofstream(CONFIG_FILE) << j.dump(4);
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
        if (code == 429) return {false, false};
        if (code == 403 || code == 400) return {false, true};
        return {false, false};
    } catch (...) { return {false, false}; }
}

bool broadcast(const std::string& text) {
    std::shared_ptr<const std::set<std::string>> subs;
    { std::shared_lock lock(subsMutex); subs = SUBSCRIBERS_PTR; }
    if (!subs || subs->empty()) return true;

    int total = static_cast<int>(subs->size());
    int success = 0;
    bool adminOk = false;
    std::vector<std::string> toRemove;

    for (auto& id : *subs) {
        auto [ok, dead] = sendMsg(id, text);
        if (ok) {
            success++;
            if (id == MY_CHAT_ID) adminOk = true;
            std::lock_guard<std::mutex> fl(failCountMutex);
            SUBSCRIBER_FAIL_COUNT[id] = 0;
        } else if (dead) {
            std::lock_guard<std::mutex> fl(failCountMutex);
            if (++SUBSCRIBER_FAIL_COUNT[id] >= MAX_FAILS_BEFORE_REMOVE)
                toRemove.push_back(id);
        }
    }
    for (auto& d : toRemove) removeDeadSubscriber(d);
    return adminOk || ((total > 0) && (static_cast<double>(success) / total >= 0.5));
}

// ==================== TOKENS & PRICES ====================
int getDecimals(const std::string& addr) {
    std::string a = toLower(addr);    { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_DECIMALS.count(a)) return TOKEN_DECIMALS[a]; }
    auto res = rpc("eth_call", {{{"to", addr}, {"data", "0x313ce567"}}, "latest"});
    int dec = 18;
    if (res.is_string() && res.get<std::string>().length() >= 66)
        try { dec = std::stoi(res.get<std::string>().substr(2), nullptr, 16); } catch (...) {}
    { std::lock_guard<std::mutex> l(cacheMutex); TOKEN_DECIMALS[a] = dec; }
    saveTokenMetadata(a, "", dec);
    return dec;
}

std::string getSymbol(const std::string& addr) {
    std::string a = toLower(addr);
    { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_SYMBOLS.count(a)) return TOKEN_SYMBOLS[a]; }
    auto res = rpc("eth_call", {{{"to", addr}, {"data", "0x95d89b41"}}, "latest"});
    std::string sym;
    if (res.is_string() && res.get<std::string>().length() > 130) {
        try {
            std::string hex = res.get<std::string>().substr(2);
            int len = std::stoi(hex.substr(64, 64), nullptr, 16);
            if (len > 0 && len <= 32) {
                std::string strHex = hex.substr(128, static_cast<size_t>(len) * 2);
                for (size_t i = 0; i < strHex.length(); i += 2)
                    sym += static_cast<char>(std::stoi(strHex.substr(i, 2), nullptr, 16));
            }
        } catch (...) {}
    }
    if (sym.empty() && res.is_string() && res.get<std::string>().length() >= 66) {
        try {
            std::string hex = res.get<std::string>().substr(2, 64);
            for (size_t i = 0; i < hex.length(); i += 2) {
                char c = static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
                if (c == '\0') break;
                sym += c;
            }
        } catch (...) {}
    }
    if (sym.empty()) sym = "UNKNOWN";
    { std::lock_guard<std::mutex> l(cacheMutex); TOKEN_SYMBOLS[a] = sym; }
    saveTokenMetadata(a, sym, 0);
    return sym;
}

uint64_t getPriceNanos(const std::string& token) {
    std::string a = toLower(token);
    {
        std::lock_guard<std::mutex> l(cacheMutex);
        if (PRICE_NANOS_CACHE.count(a) && time(nullptr) - PRICE_NANOS_CACHE[a].second < 300)
            return PRICE_NANOS_CACHE[a].first;
    }
    auto res = http("https://api.dexscreener.com/latest/dex/tokens/" + token);    double p = 0;
    try {
        auto j = json::parse(res);
        if (j.contains("pairs") && !j["pairs"].empty())
            p = std::stod(j["pairs"][0]["priceUsd"].get<std::string>());
    } catch (...) {}
    uint64_t nanos = static_cast<uint64_t>(p * 1000000000.0);
    if (nanos > 0) {
        { std::lock_guard<std::mutex> l(cacheMutex); PRICE_NANOS_CACHE[a] = {nanos, time(nullptr)}; }
        saveTokenPrice(a, nanos);
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
        else if (c >= 'a' && c <= 'f') raw |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') raw |= (c - 'A' + 10);
    }
    return raw;
}

std::string formatAmount(const cpp_int& raw, int decimals) {
    if (raw == 0) return "0.00";
    cpp_int div = 1;
    for (int i = 0; i < decimals; i++) div *= 10;
    std::string ip = (raw / div).convert_to<std::string>();
    std::string fp = (raw % div).convert_to<std::string>();
    while (static_cast<int>(fp.length()) < decimals) fp = "0" + fp;
    if (fp.length() > 2) fp = fp.substr(0, 2);
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
    if (dollars.empty()) dollars = "0";    return "$" + dollars + "." + cents;
}

// ==================== ANALYZE TX ====================
struct TxResult {
    bool valid, isSwap, isBuy;
    cpp_int rawAmount, usdNanos;
    std::string tokenAddr;
};

TxResult analyzeTx(const json& receipt, const std::string& whaleAddr, const std::string& txHash) {
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

    cpp_int baseIn = 0, baseOut = 0;
    std::string nsTokIn, nsTokOut;
    cpp_int nsAmtIn = 0, nsAmtOut = 0;

    for (auto& t : trs) {
        if (isBaseAsset(t.tok)) {
            if (t.in) baseIn += t.amt; else baseOut += t.amt;
        } else {
            if (t.in) { nsTokIn = t.tok; nsAmtIn += t.amt; }
            else      { nsTokOut = t.tok; nsAmtOut += t.amt; }        }
    }

    if (r.isSwap) {
        if (baseOut > 0 && baseIn == 0) {
            r.isBuy = true;
            r.tokenAddr = nsTokIn.empty() ? trs.back().tok : nsTokIn;
            r.rawAmount = nsAmtIn > 0 ? nsAmtIn : trs.back().amt;
        } else if (baseIn > 0 && baseOut == 0) {
            r.isBuy = false;
            r.tokenAddr = nsTokOut.empty() ? trs.front().tok : nsTokOut;
            r.rawAmount = nsAmtOut > 0 ? nsAmtOut : trs.front().amt;
        } else if (!nsTokIn.empty() && !nsTokOut.empty()) {
            r.isBuy = true;
            r.tokenAddr = nsTokIn;
            r.rawAmount = nsAmtIn;
        } else {
            r.isBuy = baseIn > baseOut;
            r.tokenAddr = trs.front().tok;
            r.rawAmount = trs.front().amt;
        }
    } else {
        r.isBuy = hasIn;
        r.tokenAddr = trs.front().tok;
        r.rawAmount = trs.front().amt;
    }

    // ⏰ DEBUG SELL: УДАЛИТЬ ЧЕРЕЗ 24 ЧАСА ПОСЛЕ ДЕПЛОЯ
    if (r.isSwap && !r.isBuy) {
        std::cout << "[DEBUG SELL] tx=" << txHash
                  << " baseIn=" << baseIn.convert_to<std::string>()
                  << " baseOut=" << baseOut.convert_to<std::string>()
                  << " nsTokOut=" << nsTokOut
                  << " tokenAddr=" << r.tokenAddr
                  << " rawAmt=" << r.rawAmount.convert_to<std::string>()
                  << std::endl;
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
    std::string parentHash = block.value("parentHash", "");
    std::string expectedParent = getLastBlockHash();
    if (!expectedParent.empty() && parentHash != expectedParent && blockNum > 1) {
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
        if (receipt.is_null()) continue;

        TxResult res = analyzeTx(receipt, mA, hash);
        if (!res.valid) { markTxProcessed(hash, blockNum); continue; }

        if (res.isSwap && !res.isBuy) {
            if (isBaseAsset(res.tokenAddr)) {
                markTxProcessed(hash, blockNum);
                continue;
            }
        } else {
            if (isBaseAsset(res.tokenAddr)) {
                markTxProcessed(hash, blockNum);
                continue;
            }
        }

        cpp_int threshNanos = static_cast<cpp_int>(THRESHOLD_NANOS.load());
        if (res.usdNanos < threshNanos) {
            markTxProcessed(hash, blockNum);            continue;
        }

        std::string msg = "🐋 <b>" + mN + "</b>\n\n";
        if (res.isSwap) {
            msg += res.isBuy ? "🟢 <b>ПОКУПКА</b>" : "🔴 <b>ПРОДАЖА</b>";
        } else {
            msg += "📤 <b>ПЕРЕВОД</b>";
        }
        msg += "\n💰 Сумма: <b>" + formatUsd(res.usdNanos) + "</b>\n";
        msg += "🪙 Монета: <b>" + getSymbol(res.tokenAddr) + "</b>\n";
        msg += "📦 Кол-во: <b>" + formatAmount(res.rawAmount, getDecimals(res.tokenAddr)) + "</b>\n";
        msg += "📜 Контракт: <code>" + res.tokenAddr + "</code>\n\n";
        msg += "🔗 <a href=\"https://bscscan.com/tx/" + hash + "\">Транзакция</a>";

        if (broadcast(msg)) {
            markTxProcessed(hash, blockNum);
            std::cout << "[OK] " << mN << " "
                      << (res.isSwap ? (res.isBuy ? "BUY" : "SELL") : "TRANSFER")
                      << " " << formatUsd(res.usdNanos)
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
                    if (txt.find("/add ") == 0) {                        size_t p1 = txt.find(' '), p2 = txt.find(' ', p1 + 1);
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
                            [&](auto& w) { return w.first == addr; }), WHALES.end());
                        lock.unlock();
                        saveConfig(); sendMsg(cid, "✅ Кит удален");
                    }
                    else if (txt.find("/limit ") == 0) {
                        try {
                            double val = std::stod(txt.substr(7));
                            THRESHOLD_NANOS.store(static_cast<uint64_t>(val * 1000000000.0));
                            saveConfig();
                            sendMsg(cid, "✅ Лимит: $" + std::to_string(static_cast<uint64_t>(val)));
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
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::cerr << "[FATAL] curl_global_init failed" << std::endl;
        return 1;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    initDB(); loadConfig(); loadTokenCache();
    SUBSCRIBERS_PTR = loadSubscribersFromDB();
    if (SUBSCRIBERS_PTR->empty()) addSubscriber(MY_CHAT_ID);

    long long lastBlock = getLastBlock();
    if (lastBlock == 0) {
        auto b = rpc("eth_blockNumber", {});
        if (b.is_string()) lastBlock = std::stoll(b.get<std::string>(), nullptr, 16);
    }

    auto latestJson = rpc("eth_blockNumber", {});
    if (latestJson.is_string()) {
        long long latest = std::stoll(latestJson.get<std::string>(), nullptr, 16);
        if (latest - lastBlock > FAST_SYNC_LAG) {
            std::cout << "[FAST SYNC] Lag " << (latest - lastBlock)
                      << " blocks, skipping to latest-5" << std::endl;
            lastBlock = latest - 5;
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
            std::cerr << "[ERROR] " << e.what() << std::endl;        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "[SHUTDOWN] Signal received, stopping gracefully..." << std::endl;
    tgThread.join();
    walCheckpoint();
    if (db) sqlite3_close(db);
    curl_global_cleanup();
    std::cout << "[SHUTDOWN] Clean exit." << std::endl;
    return 0;
}
