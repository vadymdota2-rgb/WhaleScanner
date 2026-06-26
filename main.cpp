#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <thread>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <optional>
#include <csignal>
#include <cstdlib>
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <sqlite3.h>
#include <chrono>
#include <sys/statvfs.h>
#include <boost/multiprecision/cpp_int.hpp>
#include "json.hpp"

using json = nlohmann::json;
using boost::multiprecision::cpp_int;

// ============================================================================
// LOCK ORDER CONTRACT: dbMutex → cacheMutex → whalesMutex → subsMutex
// Never violate this order to avoid deadlocks
// ============================================================================

struct Stats {
    std::atomic<uint64_t> rpc_failures{0};
    std::atomic<uint64_t> price_fallbacks{0};
    std::atomic<uint64_t> reorg_verifications{0};
    std::atomic<uint64_t> tx_processed{0};
    std::atomic<uint64_t> alerts_sent{0};
    std::atomic<time_t> last_rpc_failure{0};
} g_stats;

const auto START_TIME = std::chrono::steady_clock::now();

std::string getUptime() {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - START_TIME).count();
    int d = secs / 86400; secs %= 86400;
    int h = secs / 3600;  secs %= 3600;
    int m = secs / 60;
    std::stringstream ss;
    if (d > 0) ss << d << "d ";
    ss << h << "h " << m << "m";
    return ss.str();
}

// ==================== UTILITIES ====================
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
        std::cerr << "[DB] prepare failed: " << sqlite3_errmsg(db) << " | SQL: " << sql << std::endl;
        return false;
    }
    return true;
}

std::string escapeHtml(const std::string& s) {
    std::string r; r.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  r += "&amp;";  break;
            case '<':  r += "&lt;";   break;
            case '>':  r += "&gt;";   break;
            case '"':  r += "&quot;"; break;
            case '\'': r += "&#39;";  break;
            default:   r += c;        break;
        }
    }
    return r;
}

std::string safeString(const std::string& s, size_t maxLen = 64) {
    std::string e = escapeHtml(s);
    if (e.size() > maxLen) e.resize(maxLen);
    return e;
}

// Safe logging: NEVER touches the network
void logCritical(const std::string& msg) {
    std::cerr << "[CRITICAL] " << msg << std::endl;
    try {
        std::ofstream("critical.log", std::ios::app)
            << "[" << time(nullptr) << "] " << msg << "\n";
    } catch (...) {}
}

// Correct free-space calculation using block sizes
int getDiskFreePercent() {
    struct statvfs st;
    if (statvfs(".", &st) != 0) return -1;
    uint64_t total = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
    uint64_t free  = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
    if (total == 0) return -1;
    return static_cast<int>((100.0 * free) / total);
}

// Safe hex parsing without exceptions
bool hexToLL(const std::string& hex, long long& out) {
    if (hex.empty()) return false;
    try {
        size_t pos = 0;
        out = std::stoll(hex, &pos, 16);
        return pos == hex.length();
    } catch (...) {
        return false;
    }
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\xc2\xa0");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n\xc2\xa0");
    return s.substr(a, b - a + 1);
}

// ==================== CONFIGURATION ====================
const std::string TG_TOKEN = []{
    const char* env = std::getenv("WHALE_TG_TOKEN");
    if (!env || std::string(env).empty()) {
        std::cerr << "[FATAL] WHALE_TG_TOKEN not set!\n"; std::exit(1);
    }
    return std::string(env);
}();

const std::string MY_CHAT_ID = "546348566";
// ID of the private Telegram channel for duplicating alerts, e.g. "-100xxxxxxxxxx".
// Optional: if the variable is not set, sending to the channel is simply skipped.
const std::string CHANNEL_ID = []{
    const char* env = std::getenv("WHALE_CHANNEL_ID");
    return env ? std::string(env) : std::string();
}();
const std::string CONFIG_FILE = "config.json";
const std::string DB_FILE = "whale_bot.db";

const std::vector<std::string> BSC_RPC_ENDPOINTS = {
    "https://bsc-dataseed.bnbchain.org",
    "https://bsc-dataseed1.defibit.io",
    "https://bsc-dataseed2.ninicoin.io",
    "https://bsc.publicnode.com",
    "https://binance.llamarpc.com"
};
std::atomic<size_t> rpcIndex{0};

const long long FAST_SYNC_LAG = 1000;
const long long REORG_ROLLBACK = 5;
const long long TX_TTL_BLOCKS = 20000;
constexpr time_t PRICE_TTL = 3600;
constexpr size_t MAX_SUBSCRIBERS = 5000;
constexpr size_t MAX_QUEUE_SIZE = 50000;

const std::set<std::string> ADMIN_IDS = {"546348566"};

const std::string SWAP_TOPIC = "0xd78ad95fa46c994b6551d0da85fc275fe613ce37657fb8d5e3d130840159d822";
const std::set<std::string> BASE_ASSETS = {
    "0x55d398326f99059ff775485246999027b3197955", // USDT
    "0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d", // USDC
    "0xe9e7cea3dedca5984780bafc599bd69add087d56", // BUSD
    "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c", // WBNB
    "0xc5f0f7b66764f6ec8c8dff7ba683102295e16409"  // FDUSD
};
bool isBaseAsset(const std::string& a) { return BASE_ASSETS.count(toLower(a)) > 0; }

// ==================== KNOWN ROUTERS / AGGREGATORS / DEXES ====================
// Address book of well-known router / aggregator contracts on BSC, used purely
// for labeling alerts (e.g. "via PancakeSwap V3"). This is NOT used for any
// trust/security decision — it only improves the message shown to the user.
// Every address below was verified against a block explorer at the time it was
// added. If a lookup misses, the bot silently falls back to no label; it never
// guesses or fabricates a venue name.
// NOTE on 0x10ed43c7...256024e: the address is 42 chars (0x + 40 hex). An earlier
// version of this table had it truncated to 41 chars, which would never match any
// real address and would simply make the label disappear — not a crash, but worth
// double-checking length explicitly given how easy it is to drop one hex digit.
const std::map<std::string, std::string> KNOWN_ROUTERS = {
    {"0x10ed43c718714eb63d5aa57b78b54704e256024e", "PancakeSwap V2"},
    {"0x13f4ea83d0bd40e75c8222255bc855a974568dd4", "PancakeSwap V3 (Smart Router)"},
    {"0x1b81d678ffb9c0263b24a97847620c99d213eb14", "PancakeSwap V3 (Swap Router)"},
    {"0x1a0a18ac4becddbd6389559687d1a73d8927e416", "PancakeSwap (Universal Router)"},
    {"0xd9c500dff816a1da21a48a732d3498bf09dc9aeb", "PancakeSwap (Universal Router 2)"},
    {"0x1111111254eeb25477b68fb85ed929f73a960582", "1inch"},
    {"0x9333c74bdd1e118634fe5664aca7a9710b108bab", "OKX DEX"},
    {"0x6015126d7d23648c2e4466693b8deab005ffaba8", "OKX DEX"},
    {"0x6131b5fae19ea4f9d964eac0408e4408b66337b5", "KyberSwap"},
    {"0xdf1a1b60f2d438842916c0adc43748768353ec25", "KyberSwap"},
    {"0x6352a56caadc4f1e25cd6c75970fa768a3304e64", "OpenOcean"},
};

// Best-effort label lookup for a router/aggregator address. Returns empty
// string if unknown — callers must treat that as "no label available", not as
// an error.
std::string lookupRouterLabel(const std::string& addr) {
    auto it = KNOWN_ROUTERS.find(toLower(addr));
    return it != KNOWN_ROUTERS.end() ? it->second : std::string();
}

std::atomic<uint64_t> THRESHOLD_NANOS{10000000000000ULL};
std::atomic<bool> running{true};
void signalHandler(int) { running.store(false, std::memory_order_relaxed); }

std::shared_mutex whalesMutex;
std::vector<std::pair<std::string, std::string>> WHALES;
std::shared_mutex subsMutex;
std::shared_ptr<const std::set<std::string>> SUBSCRIBERS_PTR;
std::mutex dbMutex, cacheMutex;
sqlite3* db = nullptr;
std::map<std::string, std::string> TOKEN_SYMBOLS;
std::map<std::string, int> TOKEN_DECIMALS;
std::map<std::string, std::pair<uint64_t, time_t>> PRICE_NANOS_CACHE;
// In-memory only (not persisted to SQLite, unlike TOKEN_SYMBOLS/DECIMALS): which
// token0/token1 a given pool contract holds. This is purely an optimization to
// avoid re-querying the same pool's token0()/token1() on every swap through it —
// losing this cache on restart just means re-fetching it once per pool, which is
// cheap and not worth a schema migration for.
std::map<std::string, std::pair<std::string, std::string>> POOL_TOKENS_CACHE;

// ==================== HTTP & RPC ====================
size_t WriteCB(void* c, size_t s, size_t n, std::string* d) {
    d->append((char*)c, s * n); return s * n;
}

std::string http(const std::string& url, const std::string& post = "", int timeout = 10) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string res; struct curl_slist* h = nullptr;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCB);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return running.load(std::memory_order_relaxed) ? 0 : 1; });
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (!post.empty()) {
        h = curl_slist_append(h, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    }
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        auto su = url.find("api.telegram.org") != std::string::npos ? "telegram_api" :
                  url.length() <= 80 ? url : url.substr(0, 80);
        std::cerr << "[HTTP] " << curl_easy_strerror(rc) << " | " << su << std::endl;
    }
    if (h) curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    return res;
}

json rpcOnEndpoint(size_t idx, const std::string& method, json params) {
    json r; r["jsonrpc"]="2.0"; r["method"]=method; r["params"]=params; r["id"]=1;
    auto res = http(BSC_RPC_ENDPOINTS[idx % BSC_RPC_ENDPOINTS.size()], r.dump());
    try { auto p = json::parse(res); if (p.contains("result") && !p["result"].is_null()) return p["result"]; } catch (...) {}
    return nullptr;
}

json rpc(const std::string& method, json params, int maxRetries = 3) {
    json r; r["jsonrpc"]="2.0"; r["method"]=method; r["params"]=params; r["id"]=1;
    std::string body = r.dump();
    for (int a = 0; a < maxRetries && running.load(std::memory_order_relaxed); a++) {
        size_t idx = rpcIndex.load(std::memory_order_relaxed) % BSC_RPC_ENDPOINTS.size();
        auto res = http(BSC_RPC_ENDPOINTS[idx], body);
        bool valid = false;
        try { auto p = json::parse(res); if (p.contains("result") && !p["result"].is_null()) { valid=true; return p["result"]; } } catch (...) {}
        if (!valid) {
            g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed);
            g_stats.last_rpc_failure.store(time(nullptr), std::memory_order_relaxed);
            size_t cur = rpcIndex.load(std::memory_order_relaxed);
            rpcIndex.store((cur+1) % BSC_RPC_ENDPOINTS.size(), std::memory_order_relaxed);
            std::cerr << "[RPC] Switching to " << ((cur+1)%BSC_RPC_ENDPOINTS.size()) << " after failure on " << BSC_RPC_ENDPOINTS[cur] << std::endl;
        }
        if (a < maxRetries-1) std::this_thread::sleep_for(std::chrono::milliseconds((1<<a)*500));
    }
    return nullptr;
}

// ==================== SQLITE ====================
void initDB() {
    if (sqlite3_open(DB_FILE.c_str(), &db) != SQLITE_OK) {
        std::cerr << "[FATAL] Cannot open DB: " << sqlite3_errmsg(db) << std::endl; std::exit(1);
    }
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    sqlite3_stmt* chk;
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &chk, nullptr) == SQLITE_OK) {
        if (sqlite3_step(chk) == SQLITE_ROW) {
            std::string mode = safeColumnText(chk, 0);
            std::cout << "[DB] Journal mode: " << mode << std::endl;
            if (mode != "wal") std::cerr << "[DB] ⚠️ WARNING: WAL mode NOT active!" << std::endl;
        }
        sqlite3_finalize(chk);
    }

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS subscribers (chat_id TEXT PRIMARY KEY);
        CREATE TABLE IF NOT EXISTS processed_tx (tx_hash TEXT PRIMARY KEY, block_number INTEGER NOT NULL);
        CREATE INDEX IF NOT EXISTS idx_processed_block ON processed_tx(block_number);
        CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS token_cache (
            address TEXT PRIMARY KEY, symbol TEXT DEFAULT '', decimals INTEGER DEFAULT 0,
            price_nanos INTEGER DEFAULT 0, price_ts INTEGER DEFAULT 0);
        CREATE TABLE IF NOT EXISTS alerts (id INTEGER PRIMARY KEY AUTOINCREMENT, message TEXT NOT NULL, created_at INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS deliveries (
            id INTEGER PRIMARY KEY AUTOINCREMENT, alert_id INTEGER NOT NULL, chat_id TEXT NOT NULL,
            status INTEGER DEFAULT 0, retry_count INTEGER DEFAULT 0, next_retry_at INTEGER DEFAULT 0,
            FOREIGN KEY(alert_id) REFERENCES alerts(id) ON DELETE CASCADE);
        CREATE INDEX IF NOT EXISTS idx_deliveries_queue ON deliveries(status, next_retry_at, id) WHERE status IN (0,3);
        CREATE INDEX IF NOT EXISTS idx_deliveries_terminal ON deliveries(status, alert_id) WHERE status IN (1,2,4);
        INSERT OR IGNORE INTO state(key,value) VALUES ('tg_offset','0');
    )";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[FATAL] Schema init failed: " << err << std::endl; sqlite3_free(err); sqlite3_close(db); std::exit(1);
    }
}

void walCheckpoint() { std::lock_guard<std::mutex> l(dbMutex); sqlite3_wal_checkpoint_v2(db,nullptr,SQLITE_CHECKPOINT_TRUNCATE,nullptr,nullptr); }
void cleanupOldTx(long long b) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"DELETE FROM processed_tx WHERE block_number<?")) return;
    sqlite3_bind_int64(s,1,b-TX_TTL_BLOCKS); sqlite3_step(s); sqlite3_finalize(s);
}
void rollbackToBlock(long long t) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"DELETE FROM processed_tx WHERE block_number>?")) return;
    sqlite3_bind_int64(s,1,t); sqlite3_step(s); sqlite3_finalize(s);
    std::cerr << "[REORG] Rolled back above block " << t << std::endl;
}
void loadTokenCache() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT address,symbol,decimals,price_nanos,price_ts FROM token_cache")) return;
    while (sqlite3_step(s)==SQLITE_ROW) {
        std::string a=safeColumnText(s,0), sym=safeColumnText(s,1);
        if (!sym.empty()) TOKEN_SYMBOLS[a]=sym;
        int d=sqlite3_column_int(s,2); if (d>0) TOKEN_DECIMALS[a]=d;
        uint64_t pn=sqlite3_column_int64(s,3); time_t ts=sqlite3_column_int64(s,4);
        if (pn>0) PRICE_NANOS_CACHE[a]={pn,ts};
    } sqlite3_finalize(s);
}
void saveTokenMetadata(const std::string& a, const std::string& sym, int dec) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT INTO token_cache(address,symbol,decimals) VALUES(?,?,?) ON CONFLICT(address) DO UPDATE SET symbol=CASE WHEN excluded.symbol!='' THEN excluded.symbol ELSE token_cache.symbol END, decimals=CASE WHEN excluded.decimals>0 THEN excluded.decimals ELSE token_cache.decimals END")) return;
    sqlite3_bind_text(s,1,a.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,sym.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int(s,3,dec);
    sqlite3_step(s); sqlite3_finalize(s);
}
void saveTokenPrice(const std::string& a, uint64_t pn) {
    if (!pn) return; std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT INTO token_cache(address,price_nanos,price_ts) VALUES(?,?,?) ON CONFLICT(address) DO UPDATE SET price_nanos=excluded.price_nanos, price_ts=excluded.price_ts")) return;
    sqlite3_bind_text(s,1,a.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,pn); sqlite3_bind_int64(s,3,time(nullptr));
    sqlite3_step(s); sqlite3_finalize(s);
}
bool isTxProcessed(const std::string& h) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT 1 FROM processed_tx WHERE tx_hash=?")) return false;
    sqlite3_bind_text(s,1,h.c_str(),-1,SQLITE_TRANSIENT); bool e=sqlite3_step(s)==SQLITE_ROW; sqlite3_finalize(s); return e;
}
void markTxProcessed(const std::string& h, long long b) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR IGNORE INTO processed_tx(tx_hash,block_number) VALUES(?,?)")) return;
    sqlite3_bind_text(s,1,h.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,b); sqlite3_step(s); sqlite3_finalize(s);
}
long getTgOffset() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT value FROM state WHERE key='tg_offset'")) return 0;
    long v=0; if (sqlite3_step(s)==SQLITE_ROW) try { v=std::stol(safeColumnText(s,0)); } catch (...) {} sqlite3_finalize(s); return v;
}
void saveTgOffset(long o) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR REPLACE INTO state(key,value) VALUES('tg_offset',?)")) return;
    std::string v=std::to_string(o); sqlite3_bind_text(s,1,v.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(s); sqlite3_finalize(s);
}
long long getLastBlock() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT value FROM state WHERE key='last_block'")) return 0;
    long long b=0; if (sqlite3_step(s)==SQLITE_ROW) { std::string v=safeColumnText(s,0); try { if (!v.empty()) b=std::stoll(v); } catch (...) {} } sqlite3_finalize(s); return b;
}
void saveLastBlock(long long b) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR REPLACE INTO state(key,value) VALUES('last_block',?)")) return;
    std::string v=std::to_string(b); sqlite3_bind_text(s,1,v.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(s); sqlite3_finalize(s);
}
std::string getLastBlockHash() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT value FROM state WHERE key='last_block_hash'")) return "";
    std::string h=(sqlite3_step(s)==SQLITE_ROW)?safeColumnText(s,0):""; sqlite3_finalize(s); return h;
}
void saveLastBlockHash(const std::string& h) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR REPLACE INTO state(key,value) VALUES('last_block_hash',?)")) return;
    sqlite3_bind_text(s,1,h.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(s); sqlite3_finalize(s);
}

// ==================== SUBSCRIBERS ====================
std::shared_ptr<const std::set<std::string>> loadSubscribersFromDB() {
    std::lock_guard<std::mutex> l(dbMutex); auto subs=std::make_shared<std::set<std::string>>(); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT chat_id FROM subscribers")) return subs;
    while (sqlite3_step(s)==SQLITE_ROW) subs->insert(safeColumnText(s,0)); sqlite3_finalize(s); return subs;
}
void refreshSubscribers() { auto n=loadSubscribersFromDB(); std::unique_lock l(subsMutex); SUBSCRIBERS_PTR=n; }
void addSubscriber(const std::string& c) {
    { std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
      if (!prepareOrLog(db,&s,"INSERT OR IGNORE INTO subscribers(chat_id) VALUES(?)")) return;
      sqlite3_bind_text(s,1,c.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(s); sqlite3_finalize(s); }
    refreshSubscribers();
}
void removeSubscriber(const std::string& c) {
    { std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
      if (!prepareOrLog(db,&s,"DELETE FROM subscribers WHERE chat_id=?")) return;
      sqlite3_bind_text(s,1,c.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(s); sqlite3_finalize(s); }
    refreshSubscribers(); std::cout << "[SUBS] Unsubscribed: " << c << std::endl;
}
void removeDeadSubscriber(const std::string& c) {
    removeSubscriber(c);
    std::cout << "[SUBS] Removed dead subscriber: " << c << std::endl;
}

// ==================== CONFIG ====================
void loadConfig() {
    std::ifstream f(CONFIG_FILE);
    if (!f.is_open()) {
        std::cout << "[CONFIG] Creating empty template..." << std::endl;
        json d; d["threshold"]=10000.0; d["whales"]=json::array();
        std::ofstream(CONFIG_FILE) << d.dump(4);
        THRESHOLD_NANOS.store(10000ULL*1000000000ULL);
        std::unique_lock l(whalesMutex); WHALES.clear();
        std::cout << "[CONFIG] No whales. Use /add in Telegram." << std::endl; return;
    }
    try {
        json j=json::parse(f); double t=j.value("threshold",10000.0);
        THRESHOLD_NANOS.store(static_cast<uint64_t>(t*1000000000.0));
        std::unique_lock l(whalesMutex); WHALES.clear();
        for (auto& w:j["whales"]) WHALES.push_back({toLower(w["address"]),w["name"]});
        std::cout << "[CONFIG] Loaded " << WHALES.size() << " whales, $" << t << std::endl;
    } catch (...) { std::cerr << "[CONFIG] Parse error!" << std::endl; }
}
void saveConfig() {
    json j; j["threshold"]=static_cast<double>(THRESHOLD_NANOS.load())/1000000000.0; j["whales"]=json::array();
    { std::shared_lock l(whalesMutex); for (auto& [a,n]:WHALES) j["whales"].push_back({{"address",a},{"name",n}}); }
    std::ofstream(CONFIG_FILE) << j.dump(4);
}

// ==================== RATE LIMITER ====================
class RateLimiter {
    std::mutex mtx; struct S { std::chrono::steady_clock::time_point last; std::deque<std::chrono::steady_clock::time_point> hist; };
    std::map<std::string,S> users; static constexpr int MIN_MS=1000, MAX_MIN=30, CLEANUP_H=24;
public:
    bool allow(const std::string& c) {
        std::lock_guard<std::mutex> l(mtx); auto now=std::chrono::steady_clock::now();
        static int cc=0; if (++cc%1000==0) for (auto it=users.begin();it!=users.end();)
            if (std::chrono::duration_cast<std::chrono::hours>(now-it->second.last).count()>CLEANUP_H) it=users.erase(it); else ++it;
        auto& s=users[c];
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now-s.last).count()<MIN_MS) return false;
        while (!s.hist.empty()&&std::chrono::duration_cast<std::chrono::seconds>(now-s.hist.front()).count()>60) s.hist.pop_front();
        if ((int)s.hist.size()>=MAX_MIN) return false;
        s.last=now; s.hist.push_back(now); return true;
    }
} g_rateLimiter;

// ==================== TELEGRAM ====================
struct SendResult { bool ok; bool deadUser; int retryAfterSec; };
SendResult sendMsg(const std::string& c, const std::string& t) {
    json j; j["chat_id"]=c; j["text"]=t; j["parse_mode"]="HTML"; j["disable_web_page_preview"]=true;
    auto r=http("https://api.telegram.org/bot"+TG_TOKEN+"/sendMessage",j.dump());
    try {
        auto p=json::parse(r); if (p.value("ok",false)) return {true,false,0};
        int code=p.value("error_code",0);
        if (code==429) { int ra=p.contains("parameters")&&p["parameters"].contains("retry_after")?p["parameters"]["retry_after"].get<int>():30; return {false,false,ra}; }
        // Telegram's 403 always means the bot was blocked/removed from the chat — the
        // user is definitely "dead". 400 is a generic request-error code (e.g. "message
        // is too long", "can't parse entities", invalid HTML) and does NOT always mean a
        // dead recipient. Previously any 400 was treated as deadUser and permanently
        // unsubscribed a LIVE subscriber because of an error in the message text itself
        // (and since the message is the same for everyone in a broadcast, this could mass
        // -unsubscribe all subscribers from a single failed broadcast). Now for 400 we
        // check the error description and only treat it as "dead" for explicit signs of a
        // removed/unreachable chat.
        std::string desc = toLower(p.value("description",""));
        bool chatGone = desc.find("chat not found")!=std::string::npos ||
                         desc.find("bot was blocked")!=std::string::npos ||
                         desc.find("user is deactivated")!=std::string::npos ||
                         desc.find("kicked")!=std::string::npos ||
                         desc.find("chat_id is empty")!=std::string::npos;
        if (code==403) return {false,true,0};
        if (code==400 && chatGone) return {false,true,0};
        if (code==400) { std::cerr << "[TG] 400 (not treated as dead user): " << desc << std::endl; return {false,false,0}; }
        return {false,false,0};
    } catch (...) { return {false,false,0}; }
}

// ==================== SAFE MESSAGE QUEUE ====================
class SafeMessageQueue {
    std::atomic<size_t> queueSize{0};
    std::atomic<time_t> globalRetryAfter{0};
    std::atomic<bool> qRunning{true};
    std::thread senderThread;
    static constexpr int SEND_MS=33;

    void initCounters() {
        std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
        if (prepareOrLog(db,&s,"SELECT COUNT(*) FROM deliveries WHERE status IN (0,3)")) {
            if (sqlite3_step(s)==SQLITE_ROW) queueSize.store(sqlite3_column_int64(s,0)); sqlite3_finalize(s); }
    }
    void updateStatus(int64_t id, int st, int rc, time_t nr) {
        std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
        if (!prepareOrLog(db,&s,"UPDATE deliveries SET status=?,retry_count=?,next_retry_at=? WHERE id=?")) return;
        sqlite3_bind_int(s,1,st); sqlite3_bind_int(s,2,rc); sqlite3_bind_int64(s,3,nr); sqlite3_bind_int64(s,4,id);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    void scheduleRetry(int64_t id) {
        std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
        if (!prepareOrLog(db,&s,"UPDATE deliveries SET status=CASE WHEN retry_count>=4 THEN 4 ELSE 3 END, retry_count=retry_count+1, next_retry_at=?+MIN(30*(1<<retry_count),600) WHERE id=? AND status!=4")) return;
        sqlite3_bind_int64(s,1,time(nullptr)); sqlite3_bind_int64(s,2,id); sqlite3_step(s);
        if (sqlite3_changes(db)>0) { sqlite3_stmt* c;
            if (prepareOrLog(db,&c,"SELECT status FROM deliveries WHERE id=?")) {
                sqlite3_bind_int64(c,1,id); if (sqlite3_step(c)==SQLITE_ROW&&sqlite3_column_int(c,0)==4) {
                    std::cerr << "[QUEUE] Delivery #" << id << " FAILED after 5 retries" << std::endl;
                    queueSize.fetch_sub(1,std::memory_order_relaxed); } sqlite3_finalize(c); } }
        sqlite3_finalize(s);
    }
    void senderLoop() {
        initCounters(); auto rec=queueSize.load();
        if (rec>0) std::cout << "[QUEUE] Recovered " << rec << " pending deliveries" << std::endl;
        while (qRunning.load(std::memory_order_relaxed)) {
            time_t ra=globalRetryAfter.load(std::memory_order_relaxed);
            if (ra>0&&time(nullptr)<ra) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
            if (queueSize.load(std::memory_order_relaxed)==0) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
            std::vector<std::tuple<int64_t,std::string,std::string>> batch;
            { std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
              if (!prepareOrLog(db,&s,"SELECT d.id,d.chat_id,a.message FROM deliveries d JOIN alerts a ON a.id=d.alert_id WHERE d.status IN (0,3) AND d.next_retry_at<=? ORDER BY d.id ASC LIMIT 100")) continue;
              sqlite3_bind_int64(s,1,time(nullptr));
              while (sqlite3_step(s)==SQLITE_ROW) batch.emplace_back(sqlite3_column_int64(s,0),safeColumnText(s,1),safeColumnText(s,2));
              sqlite3_finalize(s); }
            if (batch.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
            for (auto& [did,cid,msg]:batch) {
                auto res=sendMsg(cid,msg);
                if (res.ok) { updateStatus(did,1,0,0); queueSize.fetch_sub(1,std::memory_order_relaxed); }
                else if (res.retryAfterSec>0) { globalRetryAfter.store(time(nullptr)+res.retryAfterSec); scheduleRetry(did);
                    std::cerr << "[TG] 429: pausing " << res.retryAfterSec << "s" << std::endl; break; }
                else if (res.deadUser) { updateStatus(did,2,0,0); removeDeadSubscriber(cid); queueSize.fetch_sub(1,std::memory_order_relaxed); }
                else scheduleRetry(did);
                std::this_thread::sleep_for(std::chrono::milliseconds(SEND_MS));
            }
        }
    }
public:
    void start() { qRunning.store(true); senderThread=std::thread(&SafeMessageQueue::senderLoop,this); }
    void stop() { qRunning.store(false); if (senderThread.joinable()) senderThread.join(); }
    size_t size() { return queueSize.load(std::memory_order_relaxed); }
    void syncSize() {
        std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
        if (prepareOrLog(db,&s,"SELECT COUNT(*) FROM deliveries WHERE status IN (0,3)")) {
            if (sqlite3_step(s)==SQLITE_ROW) { size_t real=sqlite3_column_int64(s,0), atm=queueSize.load();
                if (real!=atm) { std::cerr << "[QUEUE] Size drift: atomic="<<atm<<" real="<<real<<", correcting" << std::endl; queueSize.store(real); } } sqlite3_finalize(s); }
    }
    bool enqueueBroadcast(const std::string& text) {
        std::shared_ptr<const std::set<std::string>> subs; { std::shared_lock l(subsMutex); subs=SUBSCRIBERS_PTR; }
        std::vector<std::string> recipients;
        if (subs) recipients.assign(subs->begin(), subs->end());
        if (!CHANNEL_ID.empty() && (!subs || !subs->count(CHANNEL_ID))) recipients.push_back(CHANNEL_ID);
        if (recipients.empty()) return true;

        size_t current = queueSize.load(std::memory_order_relaxed);
        size_t batchSize = recipients.size();
        if (current + batchSize > MAX_QUEUE_SIZE) {
            logCritical("Queue OVERLOAD (" + std::to_string(current) + "+" +
                        std::to_string(batchSize) + ">" + std::to_string(MAX_QUEUE_SIZE) +
                        ") — alert rejected!");
            return false;
        }

        auto txStart=std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr);
        sqlite3_stmt* s;
        if (!prepareOrLog(db,&s,"INSERT INTO alerts(message,created_at) VALUES(?,?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
        sqlite3_bind_text(s,1,text.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,time(nullptr));
        if (sqlite3_step(s)!=SQLITE_DONE) { sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
        int64_t aid=sqlite3_last_insert_rowid(db); sqlite3_finalize(s);
        if (!prepareOrLog(db,&s,"INSERT INTO deliveries(alert_id,chat_id,status,retry_count,next_retry_at) VALUES(?,?,0,0,0)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
        for (auto& c:recipients) { sqlite3_reset(s); sqlite3_bind_int64(s,1,aid); sqlite3_bind_text(s,2,c.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(s); }
        sqlite3_finalize(s); sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr);
        auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-txStart).count();
        if (ms>1000) std::cerr << "[DB] ⚠️ Slow enqueue: " << ms << "ms" << std::endl;
        queueSize.fetch_add(batchSize,std::memory_order_relaxed); return true;
    }
} g_msgQueue;

bool broadcast(const std::string& t) { return g_msgQueue.enqueueBroadcast(t); }

// ==================== TOKENS & PRICES ====================
int getDecimals(const std::string& addr) {
    std::string a=toLower(addr); { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_DECIMALS.count(a)) return TOKEN_DECIMALS[a]; }
    auto r=rpc("eth_call",{{{"to",addr},{"data","0x313ce567"}},"latest"});
    // If the RPC didn't respond at all (node unreachable/timeout), do NOT cache the
    // default of 18 forever: otherwise a temporary node outage on the first request
    // would permanently lock in the wrong decimals for the token, which silently
    // skews every later USD calculation for it (an error of orders of magnitude,
    // with no warning at all).
    if (!r.is_string()) { g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed); return 18; }
    int d=18;
    if (r.get<std::string>().length()>=66) try { d=std::stoi(r.get<std::string>().substr(2),nullptr,16); } catch (...) {}
    { std::lock_guard<std::mutex> l(cacheMutex); TOKEN_DECIMALS[a]=d; } saveTokenMetadata(a,"",d); return d;
}
std::string getSymbol(const std::string& addr) {
    std::string a=toLower(addr); { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_SYMBOLS.count(a)) return TOKEN_SYMBOLS[a]; }
    auto r=rpc("eth_call",{{{"to",addr},{"data","0x95d89b41"}},"latest"});
    // If the RPC didn't respond at all, don't cache "UNKNOWN" forever (see the comment
    // in getDecimals): otherwise the token would stay UNKNOWN in every future alert
    // after a single temporary node outage, even though the symbol is actually known
    // and would be fetched successfully on retry.
    if (!r.is_string()) { g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed); return "UNKNOWN"; }
    std::string sym;
    if (r.get<std::string>().length()>130) { try { std::string h=r.get<std::string>().substr(2); int len=std::stoi(h.substr(64,64),nullptr,16);
        if (len>0&&len<=32) { std::string sh=h.substr(128,len*2); for (size_t i=0;i<sh.length();i+=2) sym+=static_cast<char>(std::stoi(sh.substr(i,2),nullptr,16)); } } catch (...) {} }
    if (sym.empty()&&r.get<std::string>().length()>=66) { try { std::string h=r.get<std::string>().substr(2,64);
        for (size_t i=0;i<h.length();i+=2) { char c=static_cast<char>(std::stoi(h.substr(i,2),nullptr,16)); if (c=='\0') break; sym+=c; } } catch (...) {} }
    if (sym.empty()) sym="UNKNOWN"; { std::lock_guard<std::mutex> l(cacheMutex); TOKEN_SYMBOLS[a]=sym; } saveTokenMetadata(a,sym,0); return sym;
}
uint64_t getPriceNanos(const std::string& token) {
    std::string a=toLower(token);
    { std::lock_guard<std::mutex> l(cacheMutex); if (PRICE_NANOS_CACHE.count(a)&&time(nullptr)-PRICE_NANOS_CACHE[a].second<PRICE_TTL) return PRICE_NANOS_CACHE[a].first; }
    double p=0; auto r=http("https://api.dexscreener.com/latest/dex/tokens/"+token);
    try { auto j=json::parse(r); if (j.contains("pairs")&&j["pairs"].is_array()&&!j["pairs"].empty()&&j["pairs"][0].contains("priceUsd")&&j["pairs"][0]["priceUsd"].is_string()) p=std::stod(j["pairs"][0]["priceUsd"].get<std::string>()); } catch (...) {}
    if (p==0) { auto r2=http("https://api.coingecko.com/api/v3/simple/token_price/binance-smart-chain?contract_addresses="+token+"&vs_currencies=usd");
        try { auto j2=json::parse(r2); if (j2.contains(a)&&j2[a].contains("usd")&&j2[a]["usd"].is_number()) p=j2[a]["usd"].get<double>(); } catch (...) {} }
    uint64_t n=static_cast<uint64_t>(p*1000000000.0);
    if (n>0) { { std::lock_guard<std::mutex> l(cacheMutex); PRICE_NANOS_CACHE[a]={n,time(nullptr)}; } saveTokenPrice(a,n); }
    else { std::lock_guard<std::mutex> l(cacheMutex); if (PRICE_NANOS_CACHE.count(a)&&PRICE_NANOS_CACHE[a].first>0) { g_stats.price_fallbacks.fetch_add(1); std::cerr << "[PRICE] Stale cache: " << a << std::endl; return PRICE_NANOS_CACHE[a].first; } }
    return n;
}

// ==================== MATH ====================
cpp_int parseUint256(const std::string& h) {
    if (h.length()<66) return 0; cpp_int r=0;
    for (char c:h.substr(2,64)) { r<<=4; if (c>='0'&&c<='9') r|=(c-'0'); else if (c>='a'&&c<='f') r|=(c-'a'+10); else if (c>='A'&&c<='F') r|=(c-'A'+10); } return r;
}
// Extracts the wordIdx-th 32-byte (64 hex char) word from a multi-word ABI-encoded
// "data" hex string (e.g. a Swap event with several uint256/int256 fields packed
// back to back). Returns "" if the data is too short for that word index — callers
// must check for an empty result rather than assume success.
std::string nthWord(const std::string& h, size_t wordIdx) {
    size_t start = 2 + wordIdx*64; // skip "0x", then wordIdx full 32-byte words
    if (h.length() < start + 64) return "";
    return "0x" + h.substr(start, 64);
}
// Signed 256-bit two's-complement parse, needed for PancakeSwap/Uniswap V3-style
// Swap events whose amount0/amount1 fields are int256 (negative = token left the
// pool, positive = token entered the pool from the trader's perspective). Reusing
// the unsigned parseUint256 on a negative value would silently turn it into a huge
// positive number instead of the small negative one actually meant — this function
// exists specifically to avoid that misread.
cpp_int parseInt256(const std::string& h) {
    cpp_int u = parseUint256(h);
    // If the top bit (bit 255) is set, the value is negative in two's complement:
    // actual value = u - 2^256.
    cpp_int signBit = cpp_int(1) << 255;
    if (u >= signBit) {
        cpp_int modulus = cpp_int(1) << 256;
        return u - modulus;
    }
    return u;
}
std::string formatAmount(const cpp_int& raw, int dec) {
    if (raw==0) return "0.00"; cpp_int d=1; for (int i=0;i<dec;i++) d*=10;
    std::string ip=(raw/d).convert_to<std::string>(), fp=(raw%d).convert_to<std::string>();
    while ((int)fp.length()<dec) fp="0"+fp; if (fp.length()>2) fp=fp.substr(0,2); return ip+"."+fp;
}
cpp_int calcUsdNanos(const cpp_int& raw, int dec, uint64_t pn) { if (!pn) return 0; cpp_int d=1; for (int i=0;i<dec;i++) d*=10; return (raw*pn)/d; }
std::string formatUsd(const cpp_int& n) { std::string s=n.convert_to<std::string>(); while (s.length()<10) s="0"+s;
    std::string dl=s.substr(0,s.length()-9), ct=s.substr(s.length()-9,2); if (dl.empty()) dl="0"; return "$"+dl+"."+ct; }

// ==================== BALANCE-DIFF CROSS-CHECK ====================
// Queries an ERC-20 token's balanceOf(wallet) pinned to a specific historical
// block via eth_call. Used as an independent cross-check of the Transfer-log
// netflow result: comparing balance at block N-1 vs block N for the SAME token
// and SAME wallet gives the actual on-chain balance change over that block,
// which is immune to misleading Transfer events (fee-on-transfer tokens that
// emit a Transfer for the pre-fee amount, rebasing tokens, or any token whose
// Transfer event doesn't match what balanceOf later reports).
//
// IMPORTANT LIMITATION: this is a balance change over the WHOLE BLOCK, not over
// a single transaction — public RPC has no "balance immediately after tx X but
// before tx X+1 in the same block" view (that requires a debug/trace API this
// bot does not have access to). Callers MUST only treat this as a meaningful
// cross-check of one specific transaction's result when that whale had exactly
// one relevant transaction for this token in this block; otherwise the diff is
// a mix of multiple transactions and cannot be attributed to any single one.
// Returns std::nullopt on any RPC failure — callers must treat that as "no
// cross-check available", not as a zero balance or a contradiction.
std::optional<cpp_int> getTokenBalanceAtBlock(const std::string& token, const std::string& wallet, long long blockNumber) {
    if (blockNumber < 0) return std::nullopt; // block -1 (i.e. before genesis) has no defined balance
    std::string walletPadded = std::string(24,'0') + toLower(wallet).substr(2); // pad 20-byte address to 32-byte word
    std::stringstream bs; bs << "0x" << std::hex << blockNumber;
    auto r = rpc("eth_call", {{{"to",token},{"data","0x70a08231"+walletPadded}}, bs.str()});
    if (!r.is_string()) return std::nullopt;
    const std::string& hex = r.get<std::string>();
    if (hex.length() < 66) return std::nullopt;
    return parseUint256(hex);
}

// ==================== ANALYZE TX ====================
// Logic: compute the net flow (in-out) PER TOKEN separately, based on ALL Transfer
// logs in which the whale address participates (fr==wa or to==wa). This is robust
// against multi-hop routes and against multiple Transfer logs for the same token
// within one tx — previously such logs overwrote each other (nsIn/nsOut) instead of
// being summed, which could under-count a swap's amount or attribute it to the
// wrong token.
// counterAddr/counterAmount describe the OTHER side of a swap (e.g. for a BUY of
// KOMA paid with USDT, tokenAddr/rawAmount is the KOMA received and
// counterAddr/counterAmount is the USDT spent). They are only populated when
// isSwap is true and a counter asset with nonzero net flow was actually found;
// callers must check counterAddr.empty() before using counterAmount.
struct TxResult { bool valid,isSwap,isBuy; cpp_int rawAmount,usdNanos; std::string tokenAddr; std::string venue; std::string counterAddr; cpp_int counterAmount; };

TxResult analyzeTx(const json& receipt, const std::string& wa, const std::string&) {
    TxResult r={}; if (receipt.is_null()||!receipt.is_object()||!receipt.contains("logs")||!receipt["logs"].is_array()) return r;

    // Marker for "this is a swap" rather than a plain transfer — presence of at
    // least one Swap log in the receipt. We also remember which contract emitted it,
    // since that's usually the pool/router most relevant for labeling the venue.
    bool hasSwap=false;
    std::string swapLogAddr;
    size_t swapLogDataHexLen=0; // length of the hex digits in the Swap log's data field (excluding "0x"), used only for a generic V2/V3-style shape guess, never for brand naming
    for (auto& l:receipt["logs"]) {
        if (l.contains("topics")&&l["topics"].is_array()&&!l["topics"].empty()&&
            l["topics"][0].is_string()&&l["topics"][0].get<std::string>()==SWAP_TOPIC) {
            hasSwap=true;
            if (swapLogAddr.empty() && l.contains("address") && l["address"].is_string())
                swapLogAddr = toLower(l["address"].get<std::string>());
            if (l.contains("data") && l["data"].is_string()) {
                const std::string& d = l["data"].get_ref<const std::string&>();
                swapLogDataHexLen = d.size() >= 2 ? d.size() - 2 : 0;
            }
            break;
        }
    }

    // Net flow per token: positive = net inflow to wa, negative = net outflow.
    std::map<std::string, cpp_int> netFlow;
    std::vector<std::string> tokenOrder; // order of first appearance, for deterministic fallback
    auto touch = [&](const std::string& tok) {
        if (netFlow.find(tok) == netFlow.end()) { netFlow[tok] = 0; tokenOrder.push_back(tok); }
    };

    bool anyTransferForWallet = false;
    // First address the wallet directly interacted with via a Transfer log (its
    // "from" counterpart when sending, or "to" counterpart is irrelevant here) — used
    // only as a fallback label source if we don't otherwise find a known router.
    std::string firstCounterpartAddr;
    for (auto& l : receipt["logs"]) {
        if (!l.contains("topics")||!l["topics"].is_array()||l["topics"].size()<3) continue;
        if (!l["topics"][0].is_string()||l["topics"][0].get<std::string>()!="0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef") continue;
        if (!l.contains("data")||!l["data"].is_string()) continue;
        if (!l["topics"][1].is_string()||!l["topics"][2].is_string()||!l.contains("address")||!l["address"].is_string()) continue;

        // The address topic must be "0x" + 64 hex chars (a 32-byte word where the
        // address occupies the low 20 bytes = the last 40 chars starting at offset 26).
        // Previously substr(26) without a length check threw std::out_of_range on a
        // truncated/non-standard string from the RPC, which aborted analyzeTx without
        // being caught and could stall block processing forever (after the retry fix in
        // main, the block would be retried indefinitely hitting the same exception on
        // the same receipt).
        const std::string& t1 = l["topics"][1].get_ref<const std::string&>();
        const std::string& t2 = l["topics"][2].get_ref<const std::string&>();
        const std::string& addrField = l["address"].get_ref<const std::string&>();
        const std::string& dataField = l["data"].get_ref<const std::string&>();
        if (t1.length() < 66 || t2.length() < 66) continue;

        std::string fr = "0x"+toLower(t1.substr(26));
        std::string to = "0x"+toLower(t2.substr(26));
        std::string tk = toLower(addrField);
        if (fr != wa && to != wa) continue; // log doesn't touch the whale wallet directly

        cpp_int amt = parseUint256(dataField);
        touch(tk);
        anyTransferForWallet = true;
        if (firstCounterpartAddr.empty()) firstCounterpartAddr = (to == wa) ? fr : to;
        if (to == wa) netFlow[tk] += amt;   // arrived at the wallet
        if (fr == wa) netFlow[tk] -= amt;   // left the wallet (fr==to==wa cancels out — correct)
    }

    if (!anyTransferForWallet) return r;
    r.valid = true;

    // Best-effort venue label: prefer the contract that emitted the Swap log (the
    // pool/router actually doing the swapping), falling back to the first
    // counterpart address the wallet transferred with. This is purely cosmetic —
    // an unrecognized address just means no label is shown, never an error.
    if (!swapLogAddr.empty()) r.venue = lookupRouterLabel(swapLogAddr);
    if (r.venue.empty() && !firstCounterpartAddr.empty()) r.venue = lookupRouterLabel(firstCounterpartAddr);
    // If the swap went straight to a pool we don't recognize (no known router/
    // aggregator address matched), we deliberately do NOT guess a specific DEX
    // brand from the Swap event's field layout (V2-style amount0In/out vs V3-style
    // sqrtPriceX96/tick): many protocols fork the same ABI, so that would risk a
    // confident-looking but wrong brand name. Instead we only note the AMM
    // generation, which is inferable from the event shape itself, not from address
    // matching, and label it generically as "pool" rather than naming a DEX.
    if (r.venue.empty() && hasSwap && swapLogDataHexLen > 0) {
        // V2-style Swap(address,uint256,uint256,uint256,uint256,address) data is 4
        // packed uint256 fields = 256 hex chars; V3-style Swap(...) with
        // sqrtPriceX96/liquidity/tick is 5 fields = 320 hex chars. This is a shape
        // heuristic, not a brand identification — kept deliberately generic.
        if (swapLogDataHexLen == 256) r.venue = "unknown pool (V2-style)";
        else if (swapLogDataHexLen == 320) r.venue = "unknown pool (V3-style)";
    }
    if (r.venue.empty() && !firstCounterpartAddr.empty()) r.venue = lookupRouterLabel(firstCounterpartAddr);

    // Split net flow into "base" (stablecoins/WBNB) and "non-base" (the actual asset
    // of interest). Raw magnitudes aren't comparable across tokens with different
    // decimals, so we prioritize the token that ARRIVED at the wallet (the asset
    // bought); if there are no incoming non-base tokens, we take the one with the
    // largest |netflow| among outgoing ones.
    std::string bestNonBaseTok; cpp_int bestNonBaseAbs = -1; cpp_int bestNonBaseNet = 0;
    bool hasBaseIn=false, hasBaseOut=false;

    for (auto& tok : tokenOrder) {
        cpp_int net = netFlow[tok];
        if (isBaseAsset(tok)) {
            if (net > 0) hasBaseIn = true;
            if (net < 0) hasBaseOut = true;
        } else {
            if (net <= 0) continue; // first pass: incoming non-base tokens only
            if (net > bestNonBaseAbs) { bestNonBaseAbs = net; bestNonBaseTok = tok; bestNonBaseNet = net; }
        }
    }
    if (bestNonBaseTok.empty()) {
        for (auto& tok : tokenOrder) {
            if (isBaseAsset(tok)) continue;
            cpp_int net = netFlow[tok];
            cpp_int absNet = net >= 0 ? net : -net;
            if (absNet > bestNonBaseAbs) { bestNonBaseAbs = absNet; bestNonBaseTok = tok; bestNonBaseNet = net; }
        }
    }

    r.isSwap = (
        (!bestNonBaseTok.empty() && (hasBaseIn || hasBaseOut)) || // non-base <-> base
        (hasBaseIn && hasBaseOut)                                  // base <-> base (e.g. USDT->WBNB)
    );

    if (!bestNonBaseTok.empty()) {
        // There's a non-stable token with nonzero net flow — that's the trade's asset.
        r.tokenAddr = bestNonBaseTok;
        r.rawAmount = bestNonBaseAbs;
        r.isBuy = bestNonBaseNet > 0; // token arrived at the whale's wallet => buy

        // Counter side: the base asset that moved in the opposite direction. On a
        // buy we expect a base outflow (what was paid); on a sell, a base inflow
        // (what was received). If several base assets moved on that side (rare,
        // e.g. a router refunding leftover dust in a second token), we report the
        // one with the largest |netflow| — same tie-break rule as elsewhere here.
        if (r.isSwap) {
            std::string bestCounterTok; cpp_int bestCounterAbs = -1;
            for (auto& tok : tokenOrder) {
                if (!isBaseAsset(tok)) continue;
                cpp_int net = netFlow[tok];
                bool wantsOutflow = r.isBuy; // buy: we paid with base (net<0); sell: we received base (net>0)
                if (wantsOutflow && net >= 0) continue;
                if (!wantsOutflow && net <= 0) continue;
                cpp_int absNet = net >= 0 ? net : -net;
                if (absNet > bestCounterAbs) { bestCounterAbs = absNet; bestCounterTok = tok; }
            }
            if (!bestCounterTok.empty()) { r.counterAddr = bestCounterTok; r.counterAmount = bestCounterAbs; }
        }
    } else {
        // Only base assets were involved (e.g. USDT->WBNB). Comparing them by raw
        // amount is meaningless — different tokens have different decimals (the same
        // USD value can have very different "raw" numbers). So we take whichever
        // ARRIVED at the whale's wallet (that's the trade's result), and if multiple
        // arrived, the one with the largest |netflow| among them; if nothing arrived
        // (only an outflow was recorded), among the outgoing ones.
        std::string bestBaseTok; cpp_int bestBaseAbs = -1; cpp_int bestBaseNet = 0;
        for (auto& tok : tokenOrder) {
            if (!isBaseAsset(tok)) continue;
            cpp_int net = netFlow[tok];
            if (net <= 0) continue; // first pass: incoming only
            if (net > bestBaseAbs) { bestBaseAbs = net; bestBaseTok = tok; bestBaseNet = net; }
        }
        if (bestBaseTok.empty()) {
            for (auto& tok : tokenOrder) {
                if (!isBaseAsset(tok)) continue;
                cpp_int net = netFlow[tok];
                cpp_int absNet = net >= 0 ? net : -net;
                if (absNet > bestBaseAbs) { bestBaseAbs = absNet; bestBaseTok = tok; bestBaseNet = net; }
            }
        }
        if (!bestBaseTok.empty()) {
            r.tokenAddr = bestBaseTok;
            r.rawAmount = bestBaseAbs;
            r.isBuy = bestBaseNet > 0;

            // Counter side for a base<->base swap: the other base asset that moved
            // opposite to tokenAddr. Same logic as the non-base branch above, just
            // restricted to base assets other than tokenAddr itself.
            std::string bestCounterTok; cpp_int bestCounterAbs = -1;
            for (auto& tok : tokenOrder) {
                if (tok == r.tokenAddr || !isBaseAsset(tok)) continue;
                cpp_int net = netFlow[tok];
                bool wantsOutflow = r.isBuy;
                if (wantsOutflow && net >= 0) continue;
                if (!wantsOutflow && net <= 0) continue;
                cpp_int absNet = net >= 0 ? net : -net;
                if (absNet > bestCounterAbs) { bestCounterAbs = absNet; bestCounterTok = tok; }
            }
            if (!bestCounterTok.empty()) { r.counterAddr = bestCounterTok; r.counterAmount = bestCounterAbs; }
        } else {
            // Fallback: anyTransferForWallet=true guarantees at least one token in netFlow.
            r.tokenAddr = tokenOrder.front();
            cpp_int net = netFlow[r.tokenAddr];
            r.rawAmount = net >= 0 ? net : -net;
            r.isBuy = net > 0;
        }
    }

    r.usdNanos = calcUsdNanos(r.rawAmount, getDecimals(r.tokenAddr), getPriceNanos(r.tokenAddr));
    return r;
}

// ==================== PROCESS BLOCK ====================
bool processBlock(long long bn) {
    std::stringstream ss; ss << "0x" << std::hex << bn;
    auto block=rpc("eth_getBlockByNumber",{ss.str(),true});
    if (block.is_null()||!block.is_object()||!block.contains("transactions")||!block["transactions"].is_array()) return false;
    std::string ph=block.value("parentHash",""), ep=getLastBlockHash();
    if (!ep.empty()&&ph!=ep&&bn>1) {
        g_stats.reorg_verifications.fetch_add(1); std::cerr << "[REORG?] Mismatch at " << bn << ", verifying..." << std::endl;
        size_t ai=(rpcIndex.load(std::memory_order_relaxed)+1)%BSC_RPC_ENDPOINTS.size();
        auto vb=rpcOnEndpoint(ai,"eth_getBlockByNumber",{ss.str(),false});
        std::string vp=vb.is_object()?vb.value("parentHash",""): "";
        if (vp==ep) std::cerr << "[REORG] False positive" << std::endl;
        else if (vp==ph) { std::cerr << "[REORG] Confirmed! Rollback " << REORG_ROLLBACK << std::endl; rollbackToBlock(bn-REORG_ROLLBACK-1); saveLastBlock(bn-REORG_ROLLBACK-1); saveLastBlockHash(""); return false; }
        else { std::cerr << "[REORG] Both disagree, rollback" << std::endl; rollbackToBlock(bn-REORG_ROLLBACK-1); saveLastBlock(bn-REORG_ROLLBACK-1); saveLastBlockHash(""); return false; }
    }
    // Pre-pass: count how many transactions in this block touch each whale address
    // (from or to). This is needed before the balance-diff cross-check below: a
    // balance change over the whole block can only be attributed to one specific
    // transaction when that whale has exactly ONE relevant transaction in the
    // block — otherwise the diff mixes multiple transactions together and cannot
    // be assigned to any single one. No RPC calls here, just string comparisons
    // against the already-fetched block data.
    std::map<std::string, int> whaleTxCountInBlock;
    {
        std::shared_lock l(whalesMutex);
        for (auto& tx : block["transactions"]) {
            if (!tx.is_object()) continue;
            std::string from=tx.contains("from")&&tx["from"].is_string()?toLower(tx["from"].get<std::string>()):"";
            std::string to=(tx.contains("to")&&!tx["to"].is_null()&&tx["to"].is_string())?toLower(tx["to"].get<std::string>()):"";
            for (auto& [a,n]:WHALES) { (void)n; if (from==a||to==a) { whaleTxCountInBlock[a]++; break; } }
        }
    }
    for (auto& tx:block["transactions"]) {
        if (!running.load(std::memory_order_relaxed)) return false;
        if (!tx.is_object()||!tx.contains("hash")||!tx["hash"].is_string()) continue;
        std::string hash=tx["hash"].get<std::string>(); if (isTxProcessed(hash)) continue;
        g_stats.tx_processed.fetch_add(1);
        std::string from=tx.contains("from")&&tx["from"].is_string()?toLower(tx["from"].get<std::string>()):"";
        std::string to=(tx.contains("to")&&!tx["to"].is_null()&&tx["to"].is_string())?toLower(tx["to"].get<std::string>()):"";
        std::string mA,mN; { std::shared_lock l(whalesMutex); for (auto& [a,n]:WHALES) if (from==a||to==a) { mA=a; mN=n; break; } }
        if (mA.empty()) { markTxProcessed(hash,bn); continue; }
        if (from!=mA && to!=mA) {
            markTxProcessed(hash,bn);
            continue;
        }
        auto receipt=rpc("eth_getTransactionReceipt",{hash});
        if (receipt.is_null()) {
            std::cerr << "[RPC] receipt unavailable, will retry whole block: " << hash << std::endl;
            return false;
        }
        TxResult res=analyzeTx(receipt,mA,hash); if (!res.valid) { markTxProcessed(hash,bn); continue; }
        // A base asset (stablecoin/WBNB) as the resulting token is only interesting if
        // it's part of a swap (e.g. USDT->WBNB) — not just a plain stablecoin/WBNB
        // transfer between addresses, which by itself isn't a trading signal.
        if (isBaseAsset(res.tokenAddr) && !res.isSwap) { markTxProcessed(hash,bn); continue; }
        if (res.usdNanos<static_cast<cpp_int>(THRESHOLD_NANOS.load())) { markTxProcessed(hash,bn); continue; }
        std::string msg="🐋 <b>"+safeString(mN)+"</b>\n\n";
        msg+=res.isSwap?(res.isBuy?"🟢 <b>BUY</b>":"🔴 <b>SELL</b>"):"📤 <b>TRANSFER</b>";
        msg+="\n💰 Amount: <b>"+formatUsd(res.usdNanos)+"</b>\n";
        msg+="🪙 Token: <b>"+safeString(getSymbol(res.tokenAddr),32)+"</b>\n";
        msg+="📦 Qty: <b>"+formatAmount(res.rawAmount,getDecimals(res.tokenAddr))+"</b>\n";
        // Counter side of the trade (e.g. "Spent: 44,812.00 USDT" for a buy, or
        // "Received: 44,812.00 USDT" for a sell). Only shown when analyzeTx actually
        // found a counter asset with nonzero net flow — for plain transfers, or swaps
        // where the counter side couldn't be determined, this line is simply omitted
        // rather than guessing.
        if (res.isSwap && !res.counterAddr.empty()) {
            std::string counterLabel = res.isBuy ? "Spent" : "Received";
            msg += (res.isBuy ? "📉 " : "📈 ") + counterLabel + ": <b>" +
                   formatAmount(res.counterAmount, getDecimals(res.counterAddr)) + " " +
                   safeString(getSymbol(res.counterAddr), 16) + "</b>\n";
        }
        msg+="📜 Contract: <code>"+safeString(res.tokenAddr)+"</code>\n";
        if (!res.venue.empty()) msg+="🔁 DEX: <b>"+safeString(res.venue,48)+"</b>\n";
        msg+="🆔 TX: <code>"+safeString(hash,66)+"</code>\n";
        msg+="👛 Wallet: <b>"+safeString(mN)+"</b>\n\n";
        msg+="🔗 <a href=\"https://bscscan.com/tx/"+hash+"\">Transaction</a>";
        if (broadcast(msg)) { markTxProcessed(hash,bn); g_stats.alerts_sent.fetch_add(1);
            std::cout << "[OK] " << mN << " " << (res.isSwap?(res.isBuy?"BUY":"SELL"):"TRANSFER") << " " << formatUsd(res.usdNanos) << " " << getSymbol(res.tokenAddr) << std::endl;
        } else std::cerr << "[WARN] Broadcast failed for " << hash << std::endl;
    }
    saveLastBlockHash(block.is_object()?block.value("hash",""):""); return true;
}

// ==================== CLEANUP ====================
void cleanupOldAlerts() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (prepareOrLog(db,&s,"DELETE FROM alerts WHERE id IN (SELECT a.id FROM alerts a WHERE a.created_at<? AND NOT EXISTS (SELECT 1 FROM deliveries d WHERE d.alert_id=a.id AND d.status IN (0,3)))")) {
        sqlite3_bind_int64(s,1,time(nullptr)-30*86400); sqlite3_step(s); int da=sqlite3_changes(db); sqlite3_finalize(s);
        if (da>0) std::cout << "[CLEANUP] Removed " << da << " old alerts" << std::endl; }
    if (prepareOrLog(db,&s,"DELETE FROM deliveries WHERE status IN (1,2,4) AND id IN (SELECT d.id FROM deliveries d JOIN alerts a ON a.id=d.alert_id WHERE a.created_at<?)")) {
        sqlite3_bind_int64(s,1,time(nullptr)-14*86400); sqlite3_step(s); int dd=sqlite3_changes(db); sqlite3_finalize(s);
        if (dd>0) std::cout << "[CLEANUP] Removed " << dd << " terminal deliveries" << std::endl; }
}

// ==================== TELEGRAM LOOP ====================
void telegramLoop() {
    long offset=getTgOffset(); std::cout << "[TG] Restored offset: " << offset << std::endl;
    while (running.load(std::memory_order_relaxed)) {
        try {
            auto raw=http("https://api.telegram.org/bot"+TG_TOKEN+"/getUpdates?offset="+std::to_string(offset)+"&timeout=30&allowed_updates=%5B%22message%22%2C%22callback_query%22%5D","",35);
            if (raw.empty()) continue; auto upd=json::parse(raw);
            if (!upd.contains("result")||!upd["result"].is_array()) continue;
            int ub=0;
            for (auto& u:upd["result"]) {
                if (!u.contains("update_id")) continue; long cuid=u["update_id"].get<long>();
                if (u.contains("callback_query")&&u["callback_query"].is_object()) {
                    // The subscribe button was removed along with /start — there are no
                    // more callback buttons, but updates of this type still need to be
                    // acknowledged via the offset.
                    offset=cuid+1; if (++ub%5==0) saveTgOffset(offset); continue; }
                if (!u.contains("message")||!u["message"].is_object()||!u["message"].contains("text")||!u["message"]["text"].is_string()) { offset=cuid+1; if (++ub%5==0) saveTgOffset(offset); continue; }
                std::string txt=u["message"]["text"].get<std::string>(), cid=std::to_string(u["message"]["chat"]["id"].get<long>());
                if (ADMIN_IDS.count(cid)==0&&!g_rateLimiter.allow(cid)) { offset=cuid+1; if (++ub%5==0) saveTgOffset(offset); continue; }
                else if (txt=="/subscribe") { auto subs=loadSubscribersFromDB(); if (subs->size()>=MAX_SUBSCRIBERS) sendMsg(cid,"⚠️ Лимит достигнут"); else { addSubscriber(cid); sendMsg(cid,"✅ Вы подписаны!"); } }
                else if (txt=="/unsubscribe") { removeSubscriber(cid); sendMsg(cid,"❌ Вы отписались."); }
                else if (txt=="/health") {
                    size_t curIdx = rpcIndex.load(std::memory_order_relaxed) % BSC_RPC_ENDPOINTS.size();
                    int diskFree = getDiskFreePercent();
                    time_t lastFail = g_stats.last_rpc_failure.load(std::memory_order_relaxed);
                    bool rpcHealthy = (lastFail==0) || (time(nullptr)-lastFail > 300);
                    std::stringstream ss; ss << "✅ <b>OK</b>\n\n"
                        << "Block: <code>" << getLastBlock() << "</code>\n"
                        << "Queue: <b>" << g_msgQueue.size() << "</b>\n"
                        << "RPC: <b>" << (rpcHealthy?"healthy":"degraded") << "</b> (всего сбоев: " << g_stats.rpc_failures.load() << ")\n"
                        << "RPC endpoint: <code>" << safeString(BSC_RPC_ENDPOINTS[curIdx], 48) << "</code>\n"
                        << "DB: healthy\n";
                    if (diskFree >= 0) {
                        ss << "Disk: <b>" << diskFree << "% free</b>\n";
                        if (diskFree < 15) ss << "\n⚠️ <b>LOW DISK SPACE!</b>\n";
                    } else {
                        ss << "Disk: <b>unknown</b>\n";
                    }
                    ss << "Uptime: <b>" << getUptime() << "</b>";
                    sendMsg(cid,ss.str()); }
                else if (txt=="/stats") {
                    size_t qs=g_msgQueue.size(); auto subs=loadSubscribersFromDB(); int64_t fc=0;
                    { std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s; if (prepareOrLog(db,&s,"SELECT COUNT(*) FROM deliveries WHERE status=4")) { if (sqlite3_step(s)==SQLITE_ROW) fc=sqlite3_column_int64(s,0); sqlite3_finalize(s); } }
                    std::stringstream ss; ss << "📊 <b>Stats</b>\n\n👥 Subs: <b>" << subs->size() << "</b>\n📬 Queue: <b>" << qs << "</b>\n❌ Failed: <b>" << fc << "</b>\n⏱ Uptime: <b>" << getUptime() << "</b>\n\n"
                        << "⚙️ RPC fail: " << g_stats.rpc_failures.load() << "\n💰 Price fb: " << g_stats.price_fallbacks.load() << "\n🔄 REORG: " << g_stats.reorg_verifications.load() << "\n📨 Sent: " << g_stats.alerts_sent.load() << "\n🔍 TX: " << g_stats.tx_processed.load();
                    if (qs>1000) ss << "\n\n⚠️ <b>QUEUE HIGH!</b>"; if (fc>0) ss << "\n⚠️ <b>FAILED DELIVERIES!</b>";
                    sendMsg(cid,ss.str()); }
                else if (txt=="/help") { sendMsg(cid,"/subscribe — подписка\n/unsubscribe — отписка\n/stats — статистика\n/health — здоровье\n/help — справка"); }
                else if (ADMIN_IDS.count(cid)) {
                    if (txt.find("/add ")==0) { size_t p1=txt.find(' '),p2=txt.find(' ',p1+1); if (p1!=std::string::npos&&p2!=std::string::npos) {
                        std::string addr=toLower(trim(txt.substr(p1+1,p2-p1-1)));
                        std::unique_lock l(whalesMutex);
                        bool exists=false; for (auto& [a,n]:WHALES) if (a==addr) { exists=true; break; }
                        if (exists) { l.unlock(); sendMsg(cid,"⚠️ Этот кит уже есть в списке"); }
                        else { WHALES.push_back({addr,trim(txt.substr(p2+1))}); l.unlock(); saveConfig(); sendMsg(cid,"✅ Кит добавлен"); }
                    } else sendMsg(cid,"❌ /add 0x... Имя"); }
                    else if (txt.find("/remove ")==0) { std::string a=toLower(trim(txt.substr(8))); std::unique_lock l(whalesMutex);
                        size_t before=WHALES.size();
                        WHALES.erase(std::remove_if(WHALES.begin(),WHALES.end(),[&](auto&w){return w.first==a;}),WHALES.end());
                        bool removed=WHALES.size()<before; l.unlock();
                        if (removed) { saveConfig(); sendMsg(cid,"✅ Кит удален"); }
                        else sendMsg(cid,"⚠️ Адрес не найден в списке. Проверь /list"); }
                    else if (txt.find("/limit ")==0) { try { double v=std::stod(txt.substr(7)); THRESHOLD_NANOS.store(static_cast<uint64_t>(v*1000000000.0)); saveConfig(); sendMsg(cid,"✅ Лимит: $"+std::to_string(static_cast<uint64_t>(v))); } catch (...) { sendMsg(cid,"❌ Число"); } }
                    else if (txt=="/list") { std::shared_lock l(whalesMutex); uint64_t t=THRESHOLD_NANOS.load(); std::string m="💰 $"+std::to_string(t/1000000000ULL)+"\n\n"; for (auto& [a,n]:WHALES) m+="• "+safeString(n)+"\n<code>"+safeString(a)+"</code>\n\n"; l.unlock(); sendMsg(cid,m); }
                }
                offset=cuid+1; if (++ub%5==0) saveTgOffset(offset);
            }
            if (ub>0) saveTgOffset(offset);
        } catch (...) { std::this_thread::sleep_for(std::chrono::seconds(2)); }
    }
}

// ==================== MAIN ====================
int main() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) { std::cerr << "[FATAL] curl init failed" << std::endl; return 1; }
    std::signal(SIGINT,signalHandler); std::signal(SIGTERM,signalHandler);
    initDB(); loadConfig(); loadTokenCache();
    SUBSCRIBERS_PTR=loadSubscribersFromDB(); if (SUBSCRIBERS_PTR->empty()) addSubscriber(MY_CHAT_ID);
    long long lb=getLastBlock(); if (lb==0) { auto b=rpc("eth_blockNumber",{}); long long tmp; if (b.is_string()&&hexToLL(b.get<std::string>(),tmp)) lb=tmp; }
    auto lj=rpc("eth_blockNumber",{}); long long tmpLat;
    if (lj.is_string()&&hexToLL(lj.get<std::string>(),tmpLat)) { long long lat=tmpLat; if (lat-lb>FAST_SYNC_LAG) { std::cout << "[FAST SYNC] Lag " << (lat-lb) << ", skip to latest-5" << std::endl; lb=lat-5; saveLastBlock(lb); saveLastBlockHash(""); } }
    std::cout << "🐋 Started. Block: " << lb << ", Subs: " << SUBSCRIBERS_PTR->size() << ", Whales: " << WHALES.size() << ", $"<<THRESHOLD_NANOS.load()/1000000000ULL << std::endl;
    g_msgQueue.start(); std::thread tg(telegramLoop);
    long long bsc=0; auto lcp=std::chrono::steady_clock::now(), lst=std::chrono::steady_clock::now(), lsq=std::chrono::steady_clock::now(), lcl=std::chrono::steady_clock::now();
    while (running.load(std::memory_order_relaxed)) {
        try {
            auto lj=rpc("eth_blockNumber",{}); long long lat;
            if (!lj.is_string()||!hexToLL(lj.get<std::string>(),lat)) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
            while (lb<lat&&running.load(std::memory_order_relaxed)) {
                long long next = lb+1;
                if (!processBlock(next)) {
                    // Block not processed: either a temporary RPC failure (receipt/block
                    // not ready yet — last_block in the DB wasn't changed, so we need to
                    // retry the same next), or processBlock itself rolled back last_block
                    // due to a reorg (in which case the DB already holds an earlier
                    // number). In both cases, sync the in-memory lb with what's actually
                    // saved in the DB — previously lb advanced unconditionally and an
                    // unprocessed/rolled-back block was lost forever with no retry.
                    lb = getLastBlock();
                    break;
                }
                lb = next; saveLastBlock(lb);
                bool nc=(++bsc>=1000); if (!nc) { auto e=std::chrono::steady_clock::now()-lcp; nc=std::chrono::duration_cast<std::chrono::minutes>(e).count()>=30; }
                if (nc) { walCheckpoint(); cleanupOldTx(lb); bsc=0; lcp=std::chrono::steady_clock::now(); }
            }
            if (std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now()-lsq).count()>=5) { g_msgQueue.syncSize(); lsq=std::chrono::steady_clock::now(); }
            if (std::chrono::duration_cast<std::chrono::hours>(std::chrono::steady_clock::now()-lcl).count()>=24) { cleanupOldAlerts(); lcl=std::chrono::steady_clock::now(); }
            if (std::chrono::duration_cast<std::chrono::hours>(std::chrono::steady_clock::now()-lst).count()>=1) {
                std::cout << "[STATS] rpc_fail=" << g_stats.rpc_failures.load() << " price_fb=" << g_stats.price_fallbacks.load()
                    << " reorg=" << g_stats.reorg_verifications.load() << " tx=" << g_stats.tx_processed.load() << " sent=" << g_stats.alerts_sent.load()
                    << " queue=" << g_msgQueue.size() << " uptime=" << getUptime() << std::endl; lst=std::chrono::steady_clock::now(); }
        } catch (const std::exception& e) { std::cerr << "[ERROR] " << e.what() << std::endl; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "[SHUTDOWN] Stopping..." << std::endl; g_msgQueue.stop(); tg.join(); walCheckpoint(); if (db) sqlite3_close(db); curl_global_cleanup();
    std::cout << "[SHUTDOWN] Clean exit." << std::endl; return 0;
}
