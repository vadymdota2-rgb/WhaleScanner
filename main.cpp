#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <thread>
#include <deque>
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
#include <chrono>
#include <sys/statvfs.h>
#include <filesystem>
#include <boost/multiprecision/cpp_int.hpp>
#include "json.hpp"
#include "utils.h"
#include "ranking.h"
#include "alert_settings.h"
#include "premium.h"
#include "message_queue.h"
#include "tx_analyzer.h"

using json = nlohmann::json;
using boost::multiprecision::cpp_int;

struct Stats {
    std::atomic<uint64_t> rpc_failures{0};
    std::atomic<uint64_t> price_fallbacks{0};
    std::atomic<uint64_t> reorg_verifications{0};
    std::atomic<uint64_t> tx_processed{0};
    std::atomic<uint64_t> alerts_sent{0};
    std::atomic<time_t> last_rpc_failure{0};
    std::atomic<uint64_t> cov_buy{0};
    std::atomic<uint64_t> cov_sell{0};
    std::atomic<uint64_t> cov_lp_add{0};
    std::atomic<uint64_t> cov_lp_remove{0};
    std::atomic<uint64_t> cov_wrap{0};
    std::atomic<uint64_t> cov_unwrap{0};
    std::atomic<uint64_t> cov_transfer{0};
    std::atomic<uint64_t> cov_unknown{0};
    std::atomic<uint64_t> sig_swap_event{0};
    std::atomic<uint64_t> sig_universal_router{0};
    std::atomic<uint64_t> sig_multicall{0};
    std::atomic<uint64_t> sig_permit2{0};
    std::atomic<uint64_t> sig_lp_mint_burn{0};
    std::atomic<uint64_t> sig_lp_pool_identity{0};
    std::atomic<uint64_t> sig_lp_v3_event{0};
    std::atomic<uint64_t> unk_swap_no_wallet_flow{0};
    std::atomic<uint64_t> unk_only_base_flow{0};
    std::atomic<uint64_t> unk_unconfirmed_opposite{0};
    std::atomic<uint64_t> unk_lp_not_linked{0};
    std::atomic<uint64_t> unk_other{0};
    std::atomic<uint64_t> diag_swap_inferred{0};
    std::atomic<uint64_t> diag_native_counter{0};
} g_stats;

void recordCoverage(const TxResult& r) {
    if (r.venue == "Add Liquidity") g_stats.cov_lp_add.fetch_add(1, std::memory_order_relaxed);
    else if (r.venue == "Remove Liquidity") g_stats.cov_lp_remove.fetch_add(1, std::memory_order_relaxed);
    else if (r.venue == "Wrap") g_stats.cov_wrap.fetch_add(1, std::memory_order_relaxed);
    else if (r.venue == "Unwrap") g_stats.cov_unwrap.fetch_add(1, std::memory_order_relaxed);
    else if (r.isSwap) { if (r.isBuy) g_stats.cov_buy.fetch_add(1, std::memory_order_relaxed); else g_stats.cov_sell.fetch_add(1, std::memory_order_relaxed); }
    else if (r.dexActivityDetected) g_stats.cov_unknown.fetch_add(1, std::memory_order_relaxed);
    else g_stats.cov_transfer.fetch_add(1, std::memory_order_relaxed);
    if (r.hasSwapEvent) g_stats.sig_swap_event.fetch_add(1, std::memory_order_relaxed);
    if (r.isUniversalRouter) g_stats.sig_universal_router.fetch_add(1, std::memory_order_relaxed);
    if (r.isGenericMulticall) g_stats.sig_multicall.fetch_add(1, std::memory_order_relaxed);
    if (r.hasPermit2Signal) g_stats.sig_permit2.fetch_add(1, std::memory_order_relaxed);
    if (r.lpMintOrBurnSeen) g_stats.sig_lp_mint_burn.fetch_add(1, std::memory_order_relaxed);
    if (r.lpPoolIdentitySeen) g_stats.sig_lp_pool_identity.fetch_add(1, std::memory_order_relaxed);
    if (r.lpV3EventSeen) g_stats.sig_lp_v3_event.fetch_add(1, std::memory_order_relaxed);
    if (r.unknownReason == "SWAP_EVENT_WITHOUT_WALLET_FLOW" || r.unknownReason == "DEX_SIGNAL_WITHOUT_WALLET_FLOW") g_stats.unk_swap_no_wallet_flow.fetch_add(1, std::memory_order_relaxed);
    else if (r.unknownReason == "ONLY_BASE_ASSET_FLOW") g_stats.unk_only_base_flow.fetch_add(1, std::memory_order_relaxed);
    else if (r.unknownReason == "UNCONFIRMED_OPPOSITE_FLOW") g_stats.unk_unconfirmed_opposite.fetch_add(1, std::memory_order_relaxed);
    else if (r.unknownReason == "LP_EVENT_NOT_LINKED_TO_WALLET") g_stats.unk_lp_not_linked.fetch_add(1, std::memory_order_relaxed);
    else if (!r.unknownReason.empty()) g_stats.unk_other.fetch_add(1, std::memory_order_relaxed);
    if (r.diagnosticReason == "SWAP_INFERRED_FROM_FLOW") g_stats.diag_swap_inferred.fetch_add(1, std::memory_order_relaxed);
    else if (r.diagnosticReason == "NATIVE_COUNTER_REQUIRES_TRACE") g_stats.diag_native_counter.fetch_add(1, std::memory_order_relaxed);
}

const bool LOG_INVARIANT_VIOLATIONS = []() {
    const char* env = std::getenv("WHALE_LOG_INVARIANTS");
    return env && (std::string(env) == "1" || std::string(env) == "true");
}();
std::mutex invariantLogMutex;

void checkInvariants(const std::string& hash, const TxResult& r) {
    if (!LOG_INVARIANT_VIOLATIONS) return;
    std::vector<std::string> violations;
    if (r.rawAmount < 0) violations.push_back("rawAmount is negative");
    if (r.isSwap && r.tokenAddr.empty()) violations.push_back("isSwap=true but tokenAddr is empty");
    if ((r.venue == "Wrap" || r.venue == "Unwrap") && r.tokenAddr != chainCtx().wrappedNative)
        violations.push_back("Wrap/Unwrap venue but tokenAddr is not the chain's wrapped native");
    if (r.isSwap && !r.isBuy && r.counterAmount == 0 && r.diagnosticReason != "NATIVE_COUNTER_REQUIRES_TRACE") violations.push_back("SELL with zero counterAmount (unresolved counter side)");
    if (r.isSwap && r.counterAmount < 0) violations.push_back("counterAmount is negative");
    if ((r.venue == "Add Liquidity" || r.venue == "Remove Liquidity") && r.isSwap)
        violations.push_back("LP venue set but isSwap is still true");
    if (violations.empty()) return;

    std::stringstream ss;
    ss << "hash=" << hash << " venue=" << r.venue << " isSwap=" << (r.isSwap?1:0) << " isBuy=" << (r.isBuy?1:0)
       << " token=" << r.tokenAddr << " violations=[";
    for (size_t i=0;i<violations.size();i++) { if (i) ss << "; "; ss << violations[i]; }
    ss << "]";
    std::lock_guard<std::mutex> lk(invariantLogMutex);
    std::ofstream f("invariant_violations.log", std::ios::app);
    if (f) f << ss.str() << "\n";
}

const bool LOG_UNKNOWN_TX = []() {
    const char* env = std::getenv("WHALE_LOG_UNKNOWN");
    return env && (std::string(env) == "1" || std::string(env) == "true");
}();
const bool LOG_LOW_CONFIDENCE = []() {
    const char* env = std::getenv("WHALE_LOG_LOW_CONFIDENCE");
    return env && (std::string(env) == "1" || std::string(env) == "true");
}();
std::mutex diagLogMutex;

void appendDiagLog(const std::string& file, const std::string& hash, long long bn,
                    const nlohmann::json& tx, const nlohmann::json& receipt, const TxResult& res) {
    std::string from = (tx.contains("from") && tx["from"].is_string()) ? tx["from"].get<std::string>() : "";
    std::string to = (tx.contains("to") && !tx["to"].is_null() && tx["to"].is_string()) ? tx["to"].get<std::string>() : "";
    std::string input = (tx.contains("input") && tx["input"].is_string()) ? tx["input"].get<std::string>() : "";
    std::string selector = (input.size() >= 10) ? input.substr(0, 10) : "";
    std::set<std::string> topics0;
    if (receipt.is_object() && receipt.contains("logs") && receipt["logs"].is_array()) {
        for (auto& l : receipt["logs"]) {
            if (l.is_object() && l.contains("topics") && l["topics"].is_array() && !l["topics"].empty() && l["topics"][0].is_string())
                topics0.insert(l["topics"][0].get<std::string>());
        }
    }
    std::stringstream ss;
    ss << "hash=" << hash << " block=" << bn << " from=" << from << " to=" << to
       << " router=" << to << " selector=" << selector
       << " venue=" << res.venue << " isSwap=" << (res.isSwap ? 1 : 0) << " isBuy=" << (res.isBuy ? 1 : 0)
       << " token=" << res.tokenAddr << " counter=" << res.counterAddr
       << " usdNanos=" << res.usdNanos.convert_to<std::string>()
       << " whyUnknown=" << (res.unknownReason.empty() ? "-" : res.unknownReason)
       << " topics=[";
    bool first = true;
    for (auto& t : topics0) { if (!first) ss << ","; ss << t; first = false; }
    ss << "]";
    std::lock_guard<std::mutex> lk(diagLogMutex);
    std::ofstream f(file, std::ios::app);
    if (f) f << ss.str() << "\n";
}

void logUnknownTx(const std::string& hash, long long bn, const nlohmann::json& tx, const nlohmann::json& receipt, const TxResult& res) {
    if (LOG_UNKNOWN_TX) appendDiagLog("unknown_tx.log", hash, bn, tx, receipt, res);
}

void logLowConfidenceTx(const std::string& hash, long long bn, const nlohmann::json& tx, const nlohmann::json& receipt, const TxResult& res) {
    if (LOG_LOW_CONFIDENCE) appendDiagLog("low_confidence.log", hash, bn, tx, receipt, res);
}

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

void logCritical(const std::string& msg) {
    std::cerr << "[CRITICAL] " << msg << std::endl;
    try {
        std::ofstream("critical.log", std::ios::app)
            << "[" << time(nullptr) << "] " << msg << "\n";
    } catch (...) {}
}

int getDiskFreePercent() {
    struct statvfs st;
    if (statvfs(".", &st) != 0) return -1;
    uint64_t total = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
    uint64_t free  = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
    if (total == 0) return -1;
    return static_cast<int>((100.0 * free) / total);
}

uintmax_t fileSizeMB(const std::string& path) {
    try {
        std::error_code ec;
        auto sz = std::filesystem::file_size(path, ec);
        if (ec) return 0;
        return sz / (1024 * 1024);
    } catch (...) { return 0; }
}

const std::string TG_TOKEN = []{
    const char* env = std::getenv("WHALE_TG_TOKEN");
    if (!env || std::string(env).empty()) {
        std::cerr << "[FATAL] WHALE_TG_TOKEN not set!\n"; std::exit(1);
    }
    return std::string(env);
}();

const std::string OWNER_CHAT_ID = "546348566";
const std::string SERVICE_CHAT_ID = "7479880531";
const std::string CHANNEL_ID = []{
    const char* env = std::getenv("WHALE_CHANNEL_ID");
    return env ? std::string(env) : std::string();
}();
const std::string BOT_USERNAME = []{
    const char* env = std::getenv("WHALE_BOT_USERNAME");
    return (env && *env) ? std::string(env) : std::string("WalletTrackerAppBot");
}();
const std::string DB_FILE = "whale_bot.db";

const std::vector<std::string> BSC_RPC_ENDPOINTS = {
    "https://bsc-dataseed.bnbchain.org",
    "https://bsc-dataseed1.defibit.io",
    "https://bsc-dataseed2.ninicoin.io",
    "https://bsc.publicnode.com",
    "https://binance.llamarpc.com"
};
const std::vector<std::string> ETHEREUM_RPC_ENDPOINTS = {
    "https://eth.llamarpc.com",
    "https://ethereum.publicnode.com",
    "https://rpc.ankr.com/eth",
    "https://cloudflare-eth.com"
};
const std::vector<std::string> BASE_RPC_ENDPOINTS = {
    "https://mainnet.base.org",
    "https://base.publicnode.com"
};
const std::vector<std::string> ARBITRUM_RPC_ENDPOINTS = {
    "https://arbitrum.llamarpc.com",
    "https://arbitrum-one.publicnode.com"
};
std::vector<std::string> RPC_ENDPOINTS = BSC_RPC_ENDPOINTS;
std::atomic<size_t> rpcIndex{0};

const long long FAST_SYNC_LAG = 1000;
const long long REORG_ROLLBACK = 5;
const long long TX_TTL_BLOCKS = 1000;
constexpr time_t PRICE_TTL = 120;
constexpr size_t MAX_USERS = 1000000;

constexpr int DIGEST_HOUR_UTC = 12;
constexpr uint64_t DEFAULT_THRESHOLD_NANOS = 100ULL * 1000000000ULL;
uint64_t usdToNanos(double usd) { return static_cast<uint64_t>(usd * 1000000000.0 + 0.5); }
double nanosToUsd(uint64_t nanos) { return static_cast<double>(nanos) / 1000000000.0; }

std::atomic<bool> running{true};
void signalHandler(int) { running.store(false, std::memory_order_relaxed); }

std::mutex dbMutex, cacheMutex;
sqlite3* db = nullptr;
std::map<std::string, std::string> TOKEN_SYMBOLS;
std::map<std::string, int> TOKEN_DECIMALS;
std::map<std::string, std::pair<uint64_t, time_t>> PRICE_NANOS_CACHE;

struct Watcher {
    std::string chatId;
    std::string label;
    uint64_t thresholdNanos;
};
std::shared_mutex watchersMutex;
std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> WATCHERS_PTR =
    std::make_shared<const std::unordered_map<std::string, std::vector<Watcher>>>();

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
    auto res = http(RPC_ENDPOINTS[idx % RPC_ENDPOINTS.size()], r.dump());
    try { auto p = json::parse(res); if (p.contains("result") && !p["result"].is_null()) return p["result"]; } catch (...) {}
    return nullptr;
}

json rpc(const std::string& method, json params, int maxRetries = 3) {
    json r; r["jsonrpc"]="2.0"; r["method"]=method; r["params"]=params; r["id"]=1;
    std::string body = r.dump();
    for (int a = 0; a < maxRetries && running.load(std::memory_order_relaxed); a++) {
        size_t idx = rpcIndex.load(std::memory_order_relaxed) % RPC_ENDPOINTS.size();
        auto res = http(RPC_ENDPOINTS[idx], body);
        bool valid = false;
        try { auto p = json::parse(res); if (p.contains("result") && !p["result"].is_null()) { valid=true; return p["result"]; } } catch (...) {}
        if (!valid) {
            g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed);
            g_stats.last_rpc_failure.store(time(nullptr), std::memory_order_relaxed);
            size_t cur = rpcIndex.load(std::memory_order_relaxed);
            rpcIndex.store((cur+1) % RPC_ENDPOINTS.size(), std::memory_order_relaxed);
            std::cerr << "[RPC] Switching to " << ((cur+1)%RPC_ENDPOINTS.size()) << " after failure on " << RPC_ENDPOINTS[cur] << std::endl;
        }
        if (a < maxRetries-1) std::this_thread::sleep_for(std::chrono::milliseconds((1<<a)*500));
    }
    return nullptr;
}

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
        CREATE TABLE IF NOT EXISTS users (
            chat_id TEXT PRIMARY KEY,
            language TEXT NOT NULL DEFAULT 'en',
            threshold_nanos INTEGER NOT NULL DEFAULT 100000000000,
            created_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS whale_addresses (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            address TEXT UNIQUE NOT NULL
        );
        CREATE TABLE IF NOT EXISTS user_whales (
            user_id TEXT NOT NULL,
            whale_id INTEGER NOT NULL,
            label TEXT NOT NULL DEFAULT '',
            created_at INTEGER NOT NULL,
            PRIMARY KEY (user_id, whale_id),
            FOREIGN KEY(user_id) REFERENCES users(chat_id) ON DELETE CASCADE,
            FOREIGN KEY(whale_id) REFERENCES whale_addresses(id)
        );
        CREATE INDEX IF NOT EXISTS idx_user_whales_whale ON user_whales(whale_id);
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
long long getChannelDigestAt() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT value FROM state WHERE key='channel_digest_at'")) return 0;
    long long t=0; if (sqlite3_step(s)==SQLITE_ROW) { std::string v=safeColumnText(s,0); try { if (!v.empty()) t=std::stoll(v); } catch (...) {} } sqlite3_finalize(s); return t;
}
void saveChannelDigestAt(long long t) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR REPLACE INTO state(key,value) VALUES('channel_digest_at',?)")) return;
    std::string v=std::to_string(t); sqlite3_bind_text(s,1,v.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(s); sqlite3_finalize(s);
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

void ensureUser(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR IGNORE INTO users(chat_id,language,threshold_nanos,created_at) VALUES(?,?,?,?)")) return;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,"en",-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(s,3,static_cast<sqlite3_int64>(DEFAULT_THRESHOLD_NANOS));
    sqlite3_bind_int64(s,4,time(nullptr));
    sqlite3_step(s); sqlite3_finalize(s);
}

size_t countUsers() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT COUNT(*) FROM users")) return 0;
    size_t n=0; if (sqlite3_step(s)==SQLITE_ROW) n=sqlite3_column_int64(s,0); sqlite3_finalize(s); return n;
}
std::string shortAddress(const std::string& a) {
    if (a.length() <= 12) return a;
    return a.substr(0, 6) + "..." + a.substr(a.length() - 4);
}

bool isTrackingWallet(const std::string& chatId, const std::string& address) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT 1 FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id WHERE uw.user_id=? AND wa.address=?")) return false;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,address.c_str(),-1,SQLITE_TRANSIENT);
    bool e=sqlite3_step(s)==SQLITE_ROW; sqlite3_finalize(s); return e;
}

size_t countUserWhales(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT COUNT(*) FROM user_whales WHERE user_id=?")) return 0;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    size_t n=0; if (sqlite3_step(s)==SQLITE_ROW) n=sqlite3_column_int64(s,0); sqlite3_finalize(s); return n;
}

uint64_t getUserThresholdNanos(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT threshold_nanos FROM users WHERE chat_id=?")) return DEFAULT_THRESHOLD_NANOS;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    uint64_t v=DEFAULT_THRESHOLD_NANOS; if (sqlite3_step(s)==SQLITE_ROW) v=static_cast<uint64_t>(sqlite3_column_int64(s,0)); sqlite3_finalize(s); return v;
}

void refreshWatchers() {
    auto m = std::make_shared<std::unordered_map<std::string, std::vector<Watcher>>>();
    long long now = static_cast<long long>(time(nullptr));
    {
        std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;

        if (prepareOrLog(db,&s,
            "SELECT wa.address, uw.user_id, uw.label, u.threshold_nanos, "
            "       CASE WHEN u.is_premium=1 AND u.premium_expire>? THEN 1 ELSE 0 END "
            "FROM user_whales uw "
            "JOIN whale_addresses wa ON wa.id = uw.whale_id "
            "JOIN users u ON u.chat_id = uw.user_id "
            "ORDER BY uw.user_id ASC, uw.created_at ASC, uw.rowid ASC")) {
            sqlite3_bind_int64(s,1,now);
            std::string prevUser;
            size_t loadedForUser = 0;
            while (sqlite3_step(s)==SQLITE_ROW) {
                std::string addr = toLower(safeColumnText(s,0));
                std::string uid = safeColumnText(s,1);
                std::string label = safeColumnText(s,2);
                uint64_t nanos = static_cast<uint64_t>(sqlite3_column_int64(s,3));
                bool prem = sqlite3_column_int(s,4) != 0;
                if (uid != prevUser) { prevUser = uid; loadedForUser = 0; }
                if (!prem && uid != SERVICE_CHAT_ID && loadedForUser >= 1) continue;
                (*m)[addr].push_back(Watcher{uid,label,nanos});
                loadedForUser++;
            }
            sqlite3_finalize(s);
        }
    }
    std::unique_lock l(watchersMutex);
    WATCHERS_PTR = m;
}

enum class AddWhaleResult { OK, ALREADY_EXISTS, LIMIT_REACHED, BAD_ADDRESS, ERROR };

AddWhaleResult addUserWhale(const std::string& chatId, const std::string& address, const std::string& label) {
    if (!isValidAddress(address)) return AddWhaleResult::BAD_ADDRESS;
    ensureUser(chatId);

    if (chatId != SERVICE_CHAT_ID &&
        countUserWhales(chatId) >= premiumMaxWallets(chatId))
    {
        return AddWhaleResult::LIMIT_REACHED;
    }

    std::lock_guard<std::mutex> l(dbMutex);
    if (sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] addUserWhale BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return AddWhaleResult::ERROR;
    }
    sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR IGNORE INTO whale_addresses(address) VALUES(?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,address.c_str(),-1,SQLITE_TRANSIENT);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] whale_addresses INSERT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR;
    }
    sqlite3_finalize(s);
    long long whaleId=-1;
    if (!prepareOrLog(db,&s,"SELECT id FROM whale_addresses WHERE address=?")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,address.c_str(),-1,SQLITE_TRANSIENT);
    if (sqlite3_step(s)==SQLITE_ROW) whaleId=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);
    if (whaleId<0) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }

    if (!prepareOrLog(db,&s,"SELECT 1 FROM user_whales WHERE user_id=? AND whale_id=?")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,whaleId);
    bool exists = sqlite3_step(s)==SQLITE_ROW; sqlite3_finalize(s);
    if (exists) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ALREADY_EXISTS; }

    if (!prepareOrLog(db,&s,"INSERT INTO user_whales(user_id,whale_id,label,created_at) VALUES(?,?,?,?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,whaleId);
    sqlite3_bind_text(s,3,label.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,4,time(nullptr));
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] user_whales INSERT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR;
    }
    sqlite3_finalize(s);
    if (sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] addUserWhale COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
        return AddWhaleResult::ERROR;
    }
    return AddWhaleResult::OK;
}

bool removeUserWhale(const std::string& chatId, const std::string& address) {
    std::lock_guard<std::mutex> l(dbMutex);
    if (sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] removeUserWhale BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_stmt* s;
    long long whaleId=-1;
    if (prepareOrLog(db,&s,"SELECT id FROM whale_addresses WHERE address=?")) {
        sqlite3_bind_text(s,1,address.c_str(),-1,SQLITE_TRANSIENT);
        if (sqlite3_step(s)==SQLITE_ROW) whaleId=sqlite3_column_int64(s,0);
        sqlite3_finalize(s);
    }
    if (whaleId<0) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }

    if (!prepareOrLog(db,&s,"DELETE FROM user_whales WHERE user_id=? AND whale_id=?")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,whaleId);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] user_whales DELETE failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false;
    }
    bool removed = sqlite3_changes(db)>0; sqlite3_finalize(s);

    if (!prepareOrLog(db,&s,"DELETE FROM whale_addresses WHERE id=? AND NOT EXISTS (SELECT 1 FROM user_whales WHERE whale_id=?)")) {
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false;
    }
    sqlite3_bind_int64(s,1,whaleId); sqlite3_bind_int64(s,2,whaleId);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] whale_addresses cleanup failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false;
    }
    sqlite3_finalize(s);
    if (sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] removeUserWhale COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
        return false;
    }
    return removed;
}

void setUserThresholdNanos(const std::string& chatId, uint64_t nanos) {
    ensureUser(chatId);
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"UPDATE users SET threshold_nanos=? WHERE chat_id=?")) return;
    sqlite3_bind_int64(s,1,static_cast<sqlite3_int64>(nanos)); sqlite3_bind_text(s,2,chatId.c_str(),-1,SQLITE_TRANSIENT);
    if (sqlite3_step(s)!=SQLITE_DONE) std::cerr << "[DB] threshold UPDATE failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(s);
}

void setUserThreshold(const std::string& chatId, double usd) {
    setUserThresholdNanos(chatId, usdToNanos(usd));
}

std::string getUserLanguage(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT language FROM users WHERE chat_id=?")) return "en";
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    std::string lang = "en";
    if (sqlite3_step(s)==SQLITE_ROW) { std::string v = safeColumnText(s,0); if (!v.empty()) lang = v; }
    sqlite3_finalize(s);
    return lang;
}

void setUserLanguage(const std::string& chatId, const std::string& lang) {
    ensureUser(chatId);
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"UPDATE users SET language=? WHERE chat_id=?")) return;
    sqlite3_bind_text(s,1,lang.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,chatId.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_step(s); sqlite3_finalize(s);
}

std::string buildUserListText(const std::string& chatId) {
    double thr = nanosToUsd(getUserThresholdNanos(chatId));
    std::stringstream out;
    out << "💰 Your alert threshold: $" << formatThousands(static_cast<uint64_t>(thr)) << "\n\n";
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    bool any=false;
    if (prepareOrLog(db,&s,"SELECT wa.address, uw.label FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id WHERE uw.user_id=? ORDER BY uw.created_at")) {
        sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
        while (sqlite3_step(s)==SQLITE_ROW) {
            any=true;
            std::string addr = safeColumnText(s,0);
            std::string label = safeColumnText(s,1);
            if (toLower(label) == toLower(addr)) out << "💼 <b>Wallet</b>\n";
            else out << "💼 <b>" << safeString(label, 32) << "</b>\n";
            out << "<code>" << safeString(addr, 42) << "</code>\n\n";
        }
        sqlite3_finalize(s);
    }
    if (!any) out << "No wallets tracked yet. Use /add 0x... Name";
    return out.str();
}

void removeUser(const std::string& chatId) {

    if (chatId == SERVICE_CHAT_ID) {
        std::cout << "[USERS] Skip removing service account" << std::endl;
        return;
    }
    { std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
      if (!prepareOrLog(db,&s,"DELETE FROM users WHERE chat_id=?")) return;
      sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(s); sqlite3_finalize(s); }
    refreshWatchers();
    std::cout << "[USERS] Removed dead user: " << chatId << std::endl;
}

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

namespace TelegramUI {

UIMessage buildMainMenu(const std::string& chatId) {
    size_t walletCount = countUserWhales(chatId);
    double thresholdUsd = nanosToUsd(getUserThresholdNanos(chatId));

    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "➕ Add Wallet"}, {"callback_data", "menu:add_wallet"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "💼 My Wallets (" + std::to_string(walletCount) + ")"}, {"callback_data", "menu:my_wallets"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "💰 Alert Threshold ($" + formatThousands(static_cast<uint64_t>(thresholdUsd)) + ")"}, {"callback_data", "menu:alert_threshold"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "🏆 Top Traders"}, {"callback_data", "menu:toptrader"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "⭐ Premium"}, {"callback_data", "menu:premium"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "🌐 Languages"}, {"callback_data", "menu:languages"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "❓ Help"}, {"callback_data", "menu:help"}}
    }));

    std::stringstream text;
    text << "🚨 <b>Wallet Tracker</b>\n\n";
    if (walletCount == 0) {
        text << "You're not tracking any wallets yet.\nTap <b>Add Wallet</b> to start getting alerts.";
    } else {
        text << "Tracking <b>" << walletCount << "</b> wallet" << (walletCount == 1 ? "" : "s")
             << ", alerts above <b>$" << formatThousands(static_cast<uint64_t>(thresholdUsd)) << "</b>.";
    }
    return {text.str(), keyboard.dump()};
}

UIMessage buildWelcomeMessage(const std::string& chatId) {
    auto m = buildMainMenu(chatId);
    m.text = "🚨 <b>Welcome to Wallet Tracker!</b>\n\n"
             "Monitor whale wallets on " + chainCtx().displayName + " and get instant notifications for buys, sells and transfers.\n\n"
             "Tap a button below to get started:";
    return m;
}

std::string buildCancelButton() {
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "❌ Cancel"}, {"callback_data", "cancel"}}
    }));
    return keyboard.dump();
}

UIMessage buildWalletsList(const std::string& chatId) {

    bool premium = isPremium(chatId) || chatId == SERVICE_CHAT_ID;

    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT wa.address, uw.label FROM user_whales uw "
        "JOIN whale_addresses wa ON wa.id = uw.whale_id "
        "WHERE uw.user_id = ? ORDER BY uw.created_at")) {
        return {"❌ Error loading wallets.", ""};
    }
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    std::stringstream text;
    text << "💼 <b>My Wallets</b>\n\n";

    bool any = false;
    size_t idx = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        any = true;
        std::string address = safeColumnText(s, 0);
        std::string label = safeColumnText(s, 1);

        if (idx > 0) text << "━━━━━━━━━━━━━━\n";
        std::string status = premium ? "" : (idx == 0 ? " 🔔" : " ⏸");
        if (toLower(label) == address) {
            text << "💼 <b>Wallet</b>" << status << "\n";
        } else {
            text << "💼 <b>" << safeString(label, 32) << "</b>" << status << "\n";
        }
        text << "<code>" << safeString(address, 42) << "</code>\n\n";
        idx++;

        json row;
        row.push_back({{"text", "✏️ Rename"}, {"callback_data", "rename:" + address}});
        row.push_back({{"text", "🗑️ Remove"}, {"callback_data", "askremove:" + address}});
        keyboard["inline_keyboard"].push_back(row);
    }
    sqlite3_finalize(s);

    if (!any) {
        text << "No wallets tracked yet.\n\n";
        text << "Tap ➕ <b>Add Wallet</b> to start tracking.";
    } else if (!premium && idx > 1) {
        text << "ℹ️ Free plan: alerts are active only for your first wallet (🔔).\n";
        text << "Your other wallets are saved (⏸) and will re-activate with Premium.";
        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", "⭐ Upgrade to Premium"}, {"callback_data", "menu:premium"}}
        }));
    }

    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "← Back"}, {"callback_data", "menu:main"}}
    }));

    return {text.str(), keyboard.dump()};
}

UIMessage buildRemoveConfirm(const std::string& address, const std::string& label) {
    std::stringstream text;
    text << "🗑️ <b>Remove wallet?</b>\n\n";
    if (toLower(label) == toLower(address)) {
        text << "💼 <b>Wallet</b>\n";
    } else {
        text << "💼 <b>" << safeString(label, 32) << "</b>\n";
    }
    text << "<code>" << safeString(address, 42) << "</code>\n\n";
    text << "You'll stop receiving alerts for this wallet.";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "🗑️ Yes, remove"}, {"callback_data", "remove:" + address}},
        {{"text", "❌ Cancel"}, {"callback_data", "menu:my_wallets"}}
    }));
    return {text.str(), keyboard.dump()};
}

UIMessage buildLanguagesMenu(const std::string& chatId) {
    static const std::vector<std::pair<std::string, std::string>> LANGUAGES = {
        {"en", "🇺🇸 English"},
    };
    std::string current = getUserLanguage(chatId);

    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    for (const auto& lang : LANGUAGES) {
        std::string labelText = lang.second + (lang.first == current ? " ✅" : "");
        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", labelText}, {"callback_data", "lang:" + lang.first}}
        }));
    }
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "← Back"}, {"callback_data", "menu:main"}}
    }));

    std::string text = "🌐 <b>Languages</b>\n\nChoose your language:";
    return {text, keyboard.dump()};
}

UIMessage buildHelpMessage() {
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "← Back"}, {"callback_data", "menu:main"}}
    }));

    std::string text = "❓ <b>Help</b>\n\n";
    text += "🏆 Discover top on-chain traders, track wallets, and receive real-time trading alerts across supported networks.\n\n";
    text += "<b>Commands:</b>\n";
    text += "/top — Open TOP Traders\n";
    text += "/toptrader &lt;token&gt; — Top traders for a token\n";
    text += "/premium — View Premium plans\n";
    text += "/add &lt;wallet&gt; &lt;name&gt; — Track a wallet\n";
    text += "/remove &lt;wallet&gt; — Remove a wallet\n";
    text += "/list — My tracked wallets\n";
    text += "/limit &lt;USD&gt; — Set minimum alert amount\n\n";
    text += "⭐ <b>Premium</b>\n";
    text += "• Full access to Top 100 Traders\n";
    text += "• Track up to 50 wallets\n";
    text += "• Advanced features\n";
    text += "• Priority access to new updates\n\n";
    text += "📞 Support: @WalletTrackerHelp\n";
    text += "📢 Channel: t.me/WalletTrackerOfficial\n\n";
    text += "Use the main menu for quick access to all features.";

    return {text, keyboard.dump()};
}

}

UserSessionManager g_sessionManager;

SendResult sendMsg(const std::string& c, const std::string& t, const std::string& reply_markup) {
    json j;
    j["chat_id"] = c;
    j["text"] = t;
    j["parse_mode"] = "HTML";
    j["disable_web_page_preview"] = true;
    if (!reply_markup.empty()) {
        try { j["reply_markup"] = json::parse(reply_markup); } catch (...) {}
    }
    auto r = http("https://api.telegram.org/bot" + TG_TOKEN + "/sendMessage", j.dump());
    try {
        auto p = json::parse(r);
        if (p.value("ok", false)) return {true, false, 0};
        int code = p.value("error_code", 0);
        if (code == 429) {
            int ra = p.contains("parameters") && p["parameters"].contains("retry_after")
                     ? p["parameters"]["retry_after"].get<int>() : 30;
            return {false, false, ra};
        }
        std::string desc = toLower(p.value("description", ""));
        bool chatGone = desc.find("chat not found") != std::string::npos ||
                         desc.find("bot was blocked") != std::string::npos ||
                         desc.find("user is deactivated") != std::string::npos ||
                         desc.find("kicked") != std::string::npos ||
                         desc.find("chat_id is empty") != std::string::npos;
        if (code == 403) return {false, true, 0};
        if (code == 400 && chatGone) return {false, true, 0};
        if (code == 400) { std::cerr << "[TG] 400 (not treated as dead user): " << desc << std::endl; return {false, false, 0}; }
        return {false, false, 0};
    } catch (...) { return {false, false, 0}; }
}

bool editMsg(const std::string& c, long long messageId, const std::string& t, const std::string& reply_markup = "") {
    json j; j["chat_id"] = c; j["message_id"] = messageId;
    j["text"] = t;
    j["parse_mode"] = "HTML";
    j["disable_web_page_preview"] = true;
    if (!reply_markup.empty()) {
        try { j["reply_markup"] = json::parse(reply_markup); } catch (...) {}
    }
    auto r = http("https://api.telegram.org/bot" + TG_TOKEN + "/editMessageText", j.dump());
    try { auto p = json::parse(r); return p.value("ok", false); } catch (...) { return false; }
}

void replyInPlace(const std::string& chatId, long long messageId, const std::string& text, const std::string& keyboard) {
    std::string kb = keyboard.empty() ? "{\"inline_keyboard\":[]}" : keyboard;
    if (messageId <= 0 || !editMsg(chatId, messageId, text, kb)) {
        sendMsg(chatId, text, keyboard);
    }
}

void answerCallbackQuery(const std::string& callbackQueryId, const std::string& text = "", bool showAlert = false) {
    json j;
    j["callback_query_id"] = callbackQueryId;
    if (!text.empty()) j["text"] = text;
    if (showAlert) j["show_alert"] = true;
    http("https://api.telegram.org/bot" + TG_TOKEN + "/answerCallbackQuery", j.dump());
}

void setupBotCommands() {
    json cmds = json::array();
    cmds.push_back({{"command","start"},{"description","Open the main menu"}});
    cmds.push_back({{"command","add"},{"description","Track a wallet: /add 0x... Name"}});
    cmds.push_back({{"command","remove"},{"description","Stop tracking a wallet"}});
    cmds.push_back({{"command","list"},{"description","Show your tracked wallets"}});
    cmds.push_back({{"command","limit"},{"description","Set alert threshold in USD"}});
    cmds.push_back({{"command","toptrader"},{"description","Top traders for a token: /toptrader TOKEN"}});
    cmds.push_back({{"command","top"},{"description","Open TOP Traders rankings"}});
    cmds.push_back({{"command","premium"},{"description","Wallet Tracker Premium"}});
    cmds.push_back({{"command","help"},{"description","How this bot works"}});
    json j; j["commands"] = cmds;
    http("https://api.telegram.org/bot" + TG_TOKEN + "/setMyCommands", j.dump());
}

int getDecimals(const std::string& addr) {
    std::string a=toLower(addr); { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_DECIMALS.count(a)) return TOKEN_DECIMALS[a]; }
    auto r=rpc("eth_call",{{{"to",addr},{"data","0x313ce567"}},"latest"});
    if (!r.is_string()) { g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed); return 18; }
    int d=18;
    if (r.get<std::string>().length()>=66) try { d=std::stoi(r.get<std::string>().substr(2),nullptr,16); } catch (...) {}
    { std::lock_guard<std::mutex> l(cacheMutex); TOKEN_DECIMALS[a]=d; } saveTokenMetadata(a,"",d); return d;
}
std::string getSymbol(const std::string& addr) {
    std::string a=toLower(addr); { std::lock_guard<std::mutex> l(cacheMutex); if (TOKEN_SYMBOLS.count(a)) return TOKEN_SYMBOLS[a]; }
    auto r=rpc("eth_call",{{{"to",addr},{"data","0x95d89b41"}},"latest"});
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

std::string buildAlertMessage(const std::string& label, const TxResult& res, const std::string& hash) {
    bool tokenIsNative = (res.tokenAddr == chainCtx().nativeMarker);
    std::string tokenSymbol = tokenIsNative ? chainCtx().nativeSymbol : safeString(getSymbol(res.tokenAddr), 32);
    int tokenDecimals = tokenIsNative ? 18 : getDecimals(res.tokenAddr);
    std::string msg="💼 <b>"+safeString(label)+"</b>\n\n";
    if (res.venue == "Add Liquidity") msg+="🌊 <b>ADD LIQUIDITY</b>";
    else if (res.venue == "Remove Liquidity") msg+="🌊 <b>REMOVE LIQUIDITY</b>";
    else if (res.venue == "Wrap") msg+="🔄 <b>WRAP " + chainCtx().nativeSymbol + "</b>";
    else if (res.venue == "Unwrap") msg+="🔄 <b>UNWRAP " + chainCtx().nativeSymbol + "</b>";
    else msg+=res.isSwap?(res.isBuy?"🟢 <b>BUY</b>":"🚨 <b>SELL</b>"):"📤 <b>TRANSFER</b>";
    msg+="\n💰 Amount: <b>"+formatUsd(res.usdNanos)+"</b>\n";
    msg+="🪙 Token: <b>"+tokenSymbol+"</b>\n";
    msg+="📦 Qty: <b>"+formatAmount(res.rawAmount,tokenDecimals)+"</b>\n";
    if (res.isSwap) {
        cpp_int unitPriceNanos = calcUnitPriceNanos(res.usdNanos, res.rawAmount, tokenDecimals);
        std::string priceLabel = res.isBuy ? "Buy Price" : "Sell Price";
        msg += "💵 " + priceLabel + ": <b>" + formatPriceUsd(unitPriceNanos) + "</b>\n";
    }
    if (res.isSwap && !res.counterAddr.empty()) {
        std::string counterLabel = res.isBuy ? "Spent" : "Received";
        std::string counterAmountStr, counterSymbol;
        if (res.counterAddr == chainCtx().nativeMarker) {
            counterAmountStr = formatAmount(res.counterAmount, 18);
            counterSymbol = chainCtx().nativeSymbol;
        } else {
            counterAmountStr = formatAmount(res.counterAmount, getDecimals(res.counterAddr));
            counterSymbol = safeString(getSymbol(res.counterAddr), 16);
        }
        msg += (res.isBuy ? "📉 " : "📈 ") + counterLabel + ": <b>" +
               counterAmountStr + " " + counterSymbol + "</b>\n";
    }
    if (!tokenIsNative) msg+="📜 Contract: <code>"+safeString(res.tokenAddr)+"</code>\n";
    msg+="🆔 TX: <code>"+safeString(hash,66)+"</code>\n";
    msg+="💼 Wallet: <b>"+safeString(label)+"</b>\n\n";
    msg+="🔗 <a href=\""+chainCtx().explorerUrl+"/tx/"+hash+"\">Transaction</a>";
    return msg;
}

namespace {
constexpr long long AGGREGATION_WINDOW_SECONDS = 120;

struct PendingAlert {
    std::string wallet;
    TxResult agg;
    std::string hash;
    long long block = 0;
    long long blockTs = 0;
    std::chrono::steady_clock::time_point firstSeen{};
};

std::unordered_map<std::string, PendingAlert> g_pendingAlerts;
std::mutex g_pendingMutex;
}

void dispatchAlert(const std::string& mA, const TxResult& res, const std::string& hash) {
    std::map<std::string, std::vector<std::string>> byLabel;
    {
        std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> watchers;
        { std::shared_lock l(watchersMutex); watchers = WATCHERS_PTR; }
        if (!watchers) return;
        auto wit = watchers->find(mA);
        if (wit == watchers->end()) return;
        for (auto& w : wit->second) {
            if (res.usdNanos < static_cast<cpp_int>(w.thresholdNanos)) continue;
            if (w.chatId == SERVICE_CHAT_ID) continue;
            byLabel[w.label].push_back(w.chatId);
        }
    }
    if (byLabel.empty()) return;

    bool anySent = false;
    for (auto& [label, chatIds] : byLabel) {
        std::string msg = buildAlertMessage(label, res, hash);
        if (g_msgQueue.enqueueToRecipients(msg, chatIds)) anySent = true;
    }
    if (anySent) {
        g_stats.alerts_sent.fetch_add(byLabel.size());
        std::cout << "[OK] " << mA << " " << (res.isSwap?(res.isBuy?"BUY":"SELL"):"TRANSFER") << " "
                  << formatUsd(res.usdNanos) << " " << getSymbol(res.tokenAddr)
                  << " -> " << byLabel.size() << " label group(s)" << std::endl;
    } else std::cerr << "[WARN] Broadcast failed for " << hash << std::endl;
}

void bufferSwap(const std::string& mA, const TxResult& res, const std::string& hash,
                long long block, long long blockTs) {
    std::string key = mA + "|" + toLower(res.tokenAddr) + "|" + (res.isBuy ? "b" : "s");
    std::lock_guard<std::mutex> l(g_pendingMutex);
    auto it = g_pendingAlerts.find(key);
    if (it == g_pendingAlerts.end()) {
        g_pendingAlerts.emplace(key, PendingAlert{mA, res, hash, block, blockTs, std::chrono::steady_clock::now()});
        return;
    }
    TxResult& a = it->second.agg;
    a.usdNanos += res.usdNanos;
    a.rawAmount += res.rawAmount;
    if (a.counterAddr == res.counterAddr) a.counterAmount += res.counterAmount;
    else { a.counterAddr.clear(); a.counterAmount = 0; }
}

void flushPendingAlerts(bool force) {
    std::vector<PendingAlert> ready;
    {
        std::lock_guard<std::mutex> l(g_pendingMutex);
        auto now = std::chrono::steady_clock::now();
        for (auto it = g_pendingAlerts.begin(); it != g_pendingAlerts.end(); ) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.firstSeen).count();
            if (force || age >= AGGREGATION_WINDOW_SECONDS) {
                ready.push_back(std::move(it->second));
                it = g_pendingAlerts.erase(it);
            } else ++it;
        }
    }
    for (const PendingAlert& p : ready) {
        saveTrade(p.wallet, p.agg, p.hash, p.block, p.blockTs);
        dispatchAlert(p.wallet, p.agg, p.hash);
    }
}

bool processBlock(long long bn) {
    std::stringstream ss; ss << "0x" << std::hex << bn;
    auto block=rpc("eth_getBlockByNumber",{ss.str(),true});
    if (block.is_null()||!block.is_object()||!block.contains("transactions")||!block["transactions"].is_array()) return false;
    std::string ph=block.value("parentHash",""), ep=getLastBlockHash();
    long long blockTs = 0;
    if (block.contains("timestamp") && block["timestamp"].is_string())
        hexToLL(block["timestamp"].get<std::string>(), blockTs);
    if (!ep.empty()&&ph!=ep&&bn>1) {
        g_stats.reorg_verifications.fetch_add(1); std::cerr << "[REORG?] Mismatch at " << bn << ", verifying..." << std::endl;
        size_t ai=(rpcIndex.load(std::memory_order_relaxed)+1)%RPC_ENDPOINTS.size();
        auto vb=rpcOnEndpoint(ai,"eth_getBlockByNumber",{ss.str(),false});
        std::string vp=vb.is_object()?vb.value("parentHash",""): "";
        if (vp==ep) std::cerr << "[REORG] False positive" << std::endl;
        else if (vp==ph) { std::cerr << "[REORG] Confirmed! Rollback " << REORG_ROLLBACK << std::endl; rollbackToBlock(bn-REORG_ROLLBACK-1); saveLastBlock(bn-REORG_ROLLBACK-1); saveLastBlockHash(""); return false; }
        else { std::cerr << "[REORG] Both disagree, rollback" << std::endl; rollbackToBlock(bn-REORG_ROLLBACK-1); saveLastBlock(bn-REORG_ROLLBACK-1); saveLastBlockHash(""); return false; }
    }

    std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> watchers;
    { std::shared_lock l(watchersMutex); watchers = WATCHERS_PTR; }

    for (auto& tx:block["transactions"]) {
        if (!running.load(std::memory_order_relaxed)) return false;
        if (!tx.is_object()||!tx.contains("hash")||!tx["hash"].is_string()) continue;
        std::string hash=tx["hash"].get<std::string>(); if (isTxProcessed(hash)) continue;
        g_stats.tx_processed.fetch_add(1);
        std::string from=tx.contains("from")&&tx["from"].is_string()?toLower(tx["from"].get<std::string>()):"";
        std::string to=(tx.contains("to")&&!tx["to"].is_null()&&tx["to"].is_string())?toLower(tx["to"].get<std::string>()):"";
        std::string mA;
        if (watchers->count(from)) mA=from; else if (watchers->count(to)) mA=to;
        if (mA.empty()) { markTxProcessed(hash,bn); continue; }
        auto receipt=rpc("eth_getTransactionReceipt",{hash});
        if (receipt.is_null()) {
            std::cerr << "[RPC] receipt unavailable, will retry whole block: " << hash << std::endl;
            return false;
        }
        TxResult res=analyzeTx(tx,receipt,mA); if (!res.valid) { markTxProcessed(hash,bn); continue; }
        recordCoverage(res);
        checkInvariants(hash, res);
        if (!res.isSwap && !res.unknownReason.empty()) logUnknownTx(hash, bn, tx, receipt, res);
        if (res.isSwap && res.venue.empty()) logLowConfidenceTx(hash, bn, tx, receipt, res);

        if (res.tokenAddr.empty()) { markTxProcessed(hash,bn); continue; }
        if (isBaseAsset(res.tokenAddr) && !res.isSwap) { markTxProcessed(hash,bn); continue; }

        auto wit = watchers->find(mA);
        if (wit == watchers->end()) { markTxProcessed(hash,bn); continue; }

        if (res.isSwap) bufferSwap(mA, res, hash, bn, blockTs);
        else dispatchAlert(mA, res, hash);
        markTxProcessed(hash,bn);
    }
    saveLastBlockHash(block.is_object()?block.value("hash",""):""); return true;
}

void cleanupOldAlerts() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (prepareOrLog(db,&s,"DELETE FROM alerts WHERE id IN (SELECT a.id FROM alerts a WHERE a.created_at<? AND NOT EXISTS (SELECT 1 FROM deliveries d WHERE d.alert_id=a.id AND d.status IN (0,3)))")) {
        sqlite3_bind_int64(s,1,time(nullptr)-3*86400); sqlite3_step(s); int da=sqlite3_changes(db); sqlite3_finalize(s);
        if (da>0) std::cout << "[CLEANUP] Removed " << da << " old alerts" << std::endl; }
    if (prepareOrLog(db,&s,"DELETE FROM deliveries WHERE status IN (1,2,4) AND id IN (SELECT d.id FROM deliveries d JOIN alerts a ON a.id=d.alert_id WHERE a.created_at<?)")) {
        sqlite3_bind_int64(s,1,time(nullptr)-2*86400); sqlite3_step(s); int dd=sqlite3_changes(db); sqlite3_finalize(s);
        if (dd>0) std::cout << "[CLEANUP] Removed " << dd << " terminal deliveries" << std::endl; }
}

void handleCallbackQuery(const json& callbackQuery) {
    if (!callbackQuery.contains("data") || !callbackQuery["data"].is_string()) return;
    if (!callbackQuery.contains("from") || !callbackQuery["from"].contains("id")) return;

    std::string data = callbackQuery["data"].get<std::string>();
    std::string chatId = std::to_string(callbackQuery["from"]["id"].get<long>());
    std::string callbackQueryId = callbackQuery.contains("id") ? callbackQuery["id"].get<std::string>() : "";
    long long messageId = 0;
    if (callbackQuery.contains("message") && callbackQuery["message"].is_object() &&
        callbackQuery["message"].contains("message_id")) {
        messageId = callbackQuery["message"]["message_id"].get<long long>();
    }

    size_t colonPos = data.find(':');
    std::string action = colonPos != std::string::npos ? data.substr(0, colonPos) : data;
    std::string param = colonPos != std::string::npos ? data.substr(colonPos + 1) : "";

    if (action != "tt_track" && !callbackQueryId.empty()) {
        answerCallbackQuery(callbackQueryId);
    }

    if (action == "menu") {
        g_sessionManager.clearSession(chatId);

        if (param == "main") {
            auto msg = TelegramUI::buildMainMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "add_wallet") {
            g_sessionManager.setState(chatId, UserState::AWAITING_WALLET_ADDRESS);
            replyInPlace(chatId, messageId, "➕ <b>Add Wallet</b>\n\nPlease enter the wallet address (0x...):",
                    TelegramUI::buildCancelButton());
        }
        else if (param == "my_wallets") {
            auto msg = TelegramUI::buildWalletsList(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "alert_threshold") {
            uint64_t threshold = getUserThresholdNanos(chatId);
            auto msg = TelegramUI::buildAlertThresholdMenu(threshold);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "toptrader") {
            auto msg = buildGlobalTopMenu();
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "premium") {

            auto msg = buildPremiumPage(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "settings") {
            auto msg = TelegramUI::buildMainMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "languages") {
            auto msg = TelegramUI::buildLanguagesMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "help") {
            auto msg = TelegramUI::buildHelpMessage();
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
    }
    else if (action == "cancel") {
        g_sessionManager.clearSession(chatId);
        auto msg = TelegramUI::buildMainMenu(chatId);
        replyInPlace(chatId, messageId, "❌ Operation cancelled.\n\n" + msg.text, msg.keyboard);
    }
    else if (action == "lang") {
        static const std::set<std::string> SUPPORTED_LANGS = {"en"};
        if (SUPPORTED_LANGS.count(param)) {
            setUserLanguage(chatId, param);
            auto msg = TelegramUI::buildLanguagesMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
    }
    else if (action == "premium_buy") {

        if (!sendPremiumInvoice(chatId)) {
            replyInPlace(chatId, messageId,
                "❌ Could not create the invoice. Please try again later.", "");
        }
    }
    else if (action == "rename") {
        std::string address = toLower(param);
        if (!isValidAddress(address)) {
            replyInPlace(chatId, messageId, "❌ Invalid wallet address.", "");
            return;
        }

        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (!prepareOrLog(db, &s,
            "SELECT uw.label FROM user_whales uw "
            "JOIN whale_addresses wa ON wa.id = uw.whale_id "
            "WHERE uw.user_id = ? AND wa.address = ?")) {
            replyInPlace(chatId, messageId, "❌ Error loading wallet.", "");
            return;
        }
        sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, address.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(s) == SQLITE_ROW) {
            std::string currentLabel = safeColumnText(s, 0);
            sqlite3_finalize(s);

            g_sessionManager.setState(chatId, UserState::AWAITING_RENAME, address);
            replyInPlace(chatId, messageId, "✏️ <b>Rename Wallet</b>\n\nCurrent name: <b>" + safeString(currentLabel, 32) +
                    "</b>\n\nPlease enter a new name:", TelegramUI::buildCancelButton());
        } else {
            sqlite3_finalize(s);
            replyInPlace(chatId, messageId, "❌ Wallet not found in your list.", "");
        }
    }
    else if (action == "askremove") {
        std::string address = toLower(param);
        if (!isValidAddress(address)) {
            replyInPlace(chatId, messageId, "❌ Invalid wallet address.", "");
            return;
        }
        std::string label = address;
        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (prepareOrLog(db, &s,
                "SELECT uw.label FROM user_whales uw "
                "JOIN whale_addresses wa ON wa.id = uw.whale_id "
                "WHERE uw.user_id = ? AND wa.address = ?")) {
                sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, address.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(s) == SQLITE_ROW) label = safeColumnText(s, 0);
                sqlite3_finalize(s);
            }
        }
        auto msg = TelegramUI::buildRemoveConfirm(address, label);
        replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    }
    else if (action == "remove") {
        std::string address = toLower(param);
        if (!isValidAddress(address)) {
            replyInPlace(chatId, messageId, "❌ Invalid wallet address.", "");
            return;
        }

        bool removed = removeUserWhale(chatId, address);
        if (removed) {
            refreshWatchers();
            auto msg = TelegramUI::buildMainMenu(chatId);
            replyInPlace(chatId, messageId, "✅ Wallet removed.\n\n" + msg.text, msg.keyboard);
        } else {
            replyInPlace(chatId, messageId, "❌ Wallet not found in your list.", "");
        }
    }
    else if (action == "threshold") {
        handleThresholdCallback(chatId, param, messageId);
    }
    else if (action == "tt_page") {
        int page = 1;
        try { page = std::stoi(param); } catch (...) {}
        auto msg = buildTopPnlPage(chatId, page);
        replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    }
    else if (action == "tt_track") {
        std::string address = toLower(param);
        if (!isValidAddress(address)) {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, "❌ Invalid address.", true);
        }
        else if (isTrackingWallet(chatId, address)) {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, "✅ Already tracking", true);
        }
        else if (chatId != SERVICE_CHAT_ID && countUserWhales(chatId) >= premiumMaxWallets(chatId)) {
            std::string feedback = isPremium(chatId)
                ? "⚠️ Wallet limit reached (50)"
                : "⚠️ Free plan allows tracking only 1 wallet. Upgrade to Premium — tap ⭐ Premium in the menu.";
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, feedback, true);
        }
        else {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId);
            g_sessionManager.setState(chatId, UserState::AWAITING_TRACK_NAME, address);
            sendMsg(chatId, "🏷 Enter a name for this trader:", TelegramUI::buildCancelButton());
        }
    }
    else if (action == "tt_noop") {
    }
    else if (action == "gt_open") {
        GlobalRankKind kind;
        if (parseGlobalRankKind(param, kind)) {

            auto msg = buildGlobalTopMessage(chatId, kind,
                                             premiumTopTradersLimit(chatId),
                                             !isPremium(chatId));
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
    }
    else if (action == "gt_page") {
        size_t sep = param.find(':');
        if (sep != std::string::npos) {
            std::string kindStr = param.substr(0, sep);
            int page = 1;
            try { page = std::stoi(param.substr(sep + 1)); } catch (...) {}
            GlobalRankKind kind;
            if (parseGlobalRankKind(kindStr, kind)) {
                auto msg = buildGlobalTopPage(chatId, kind, page,
                                              premiumTopTradersLimit(chatId),
                                              !isPremium(chatId));
                replyInPlace(chatId, messageId, msg.text, msg.keyboard);
            }
        }
    }
    else if (action == "gt_token") {
        g_sessionManager.setState(chatId, UserState::AWAITING_TOPTRADER_TOKEN);

        replyInPlace(
            chatId,
            messageId,
            "🪙 <b>Top PnL by Token</b>\n\n"
            "Enter a token symbol (e.g. <code>CAKE</code>) "
            "or a contract address (<code>0x...</code>):",
            TelegramUI::buildCancelButton()
        );
    }
}

bool handleTextInput(const std::string& chatId, const std::string& text) {
    UserSession session = g_sessionManager.getSession(chatId);

    if (session.state == UserState::IDLE) {
        return false;
    }

    if (session.state == UserState::AWAITING_WALLET_ADDRESS) {
        std::string address = toLower(trim(text));

        if (!isValidAddress(address)) {
            sendMsg(chatId, "❌ Invalid BSC address.\n\nPlease enter a valid address or press Cancel.",
                    TelegramUI::buildCancelButton());
            return true;
        }

        g_sessionManager.setState(chatId, UserState::AWAITING_WALLET_NAME, address);
        sendMsg(chatId, "✅ Address accepted.\n\nNow enter a name for this wallet (e.g., \"Binance\"):",
                TelegramUI::buildCancelButton());
        return true;
    }

    if (session.state == UserState::AWAITING_WALLET_NAME) {
        std::string label = trim(text);

        if (label.empty()) {
            sendMsg(chatId, "❌ Name cannot be empty.\n\nPlease enter a name or press Cancel.",
                    TelegramUI::buildCancelButton());
            return true;
        }

        if (label.length() > 32) {
            sendMsg(chatId, "❌ Name is too long (max 32 characters).\n\nPlease enter a shorter name or press Cancel.",
                    TelegramUI::buildCancelButton());
            return true;
        }

        auto result = addUserWhale(chatId, session.pendingAddress, label);

        if (result == AddWhaleResult::OK) {
            refreshWatchers();
            g_sessionManager.clearSession(chatId);
            auto msg = TelegramUI::buildMainMenu(chatId);
            sendMsg(chatId, "✅ <b>Wallet added</b>\n\nName: <b>" + safeString(label, 32) +
                    "</b>\nAddress: <code>" + session.pendingAddress + "</code>\n\nTracking enabled.\n\n" + msg.text,
                    msg.keyboard);
        }
        else if (result == AddWhaleResult::ALREADY_EXISTS) {
            g_sessionManager.setState(chatId, UserState::AWAITING_WALLET_ADDRESS);
            sendMsg(chatId, "⚠️ You're already tracking this wallet.\n\nPlease enter a different address or press Cancel.",
                    TelegramUI::buildCancelButton());
        }
        else if (result == AddWhaleResult::LIMIT_REACHED) {
            g_sessionManager.clearSession(chatId);
            if (isPremium(chatId)) {
                auto msg = TelegramUI::buildMainMenu(chatId);
                sendMsg(chatId, "⚠️ You've reached the limit of 50 tracked wallets.\n\n" + msg.text, msg.keyboard);
            } else {

                auto lim = buildWalletLimitMessage();
                sendMsg(chatId, lim.text, lim.keyboard);
            }
        }
        else {
            sendMsg(chatId, "❌ Something went wrong, please try again.", TelegramUI::buildCancelButton());
        }

        return true;
    }

    if (session.state == UserState::AWAITING_TRACK_NAME) {
        std::string address = session.pendingAddress;
        std::string label = trim(text);

        if (label.empty() || label == "-" || label == "." || toLower(label) == address) {
            label = shortAddress(address);
        }

        if (label.length() > 32) {
            sendMsg(chatId, "❌ Name is too long (max 32 characters).\n\nPlease enter a shorter name or press Cancel.",
                    TelegramUI::buildCancelButton());
            return true;
        }

        auto result = addUserWhale(chatId, address, label);

        if (result == AddWhaleResult::OK) {
            refreshWatchers();
            g_sessionManager.clearSession(chatId);
            auto msg = TelegramUI::buildMainMenu(chatId);
            sendMsg(chatId, "✅ Wallet \"" + safeString(label, 32) + "\" is now being tracked.\n\n" + msg.text, msg.keyboard);
        }
        else if (result == AddWhaleResult::ALREADY_EXISTS) {
            g_sessionManager.clearSession(chatId);
            auto msg = TelegramUI::buildMainMenu(chatId);
            sendMsg(chatId, "⚠️ You're already tracking this wallet.\n\n" + msg.text, msg.keyboard);
        }
        else if (result == AddWhaleResult::LIMIT_REACHED) {
            g_sessionManager.clearSession(chatId);
            if (isPremium(chatId)) {
                auto msg = TelegramUI::buildMainMenu(chatId);
                sendMsg(chatId, "⚠️ You've reached the limit of 50 tracked wallets.\n\n" + msg.text, msg.keyboard);
            } else {
                auto lim = buildWalletLimitMessage();
                sendMsg(chatId, lim.text, lim.keyboard);
            }
        }
        else {
            sendMsg(chatId, "❌ Something went wrong, please try again.", TelegramUI::buildCancelButton());
        }
        return true;
    }

    if (session.state == UserState::AWAITING_RENAME) {
        std::string newLabel = trim(text);

        if (newLabel.empty()) {
            sendMsg(chatId, "❌ Name cannot be empty.\n\nPlease enter a name or press Cancel.",
                    TelegramUI::buildCancelButton());
            return true;
        }

        if (newLabel.length() > 32) {
            sendMsg(chatId, "❌ Name is too long (max 32 characters).\n\nPlease enter a shorter name or press Cancel.",
                    TelegramUI::buildCancelButton());
            return true;
        }

        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (prepareOrLog(db, &s,
                "UPDATE user_whales SET label = ? "
                "WHERE user_id = ? AND whale_id = (SELECT id FROM whale_addresses WHERE address = ?)")) {
                sqlite3_bind_text(s, 1, newLabel.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, chatId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, session.pendingAddress.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
        }

        refreshWatchers();
        g_sessionManager.clearSession(chatId);
        auto msg = TelegramUI::buildMainMenu(chatId);
        sendMsg(chatId, "✅ <b>Wallet renamed</b>\n\nNew name: <b>" + safeString(newLabel, 32) + "</b>.\n\n" + msg.text,
                msg.keyboard);
        return true;
    }

    if (session.state == UserState::AWAITING_CUSTOM_THRESHOLD)
        return handleThresholdText(chatId, text);

    if (session.state == UserState::AWAITING_TOPTRADER_TOKEN) {
        std::string tokenArg = trim(text);

        if (tokenArg.empty()) {
            sendMsg(chatId, "❌ Please enter a token symbol or contract address, or press Cancel.",
                    TelegramUI::buildCancelButton());
            return true;
        }

        RankingMessage result = buildTopPnlMessage(chatId, tokenArg, 1);
        g_sessionManager.clearSession(chatId);
        sendMsg(chatId, result.text, result.keyboard);
        return true;
    }

    return false;
}

void telegramLoop() {
    long offset=getTgOffset(); std::cout << "[TG] Restored offset: " << offset << std::endl;
    while (running.load(std::memory_order_relaxed)) {
        try {

            auto raw=http("https://api.telegram.org/bot"+TG_TOKEN+"/getUpdates?offset="+std::to_string(offset)+"&timeout=30&allowed_updates=%5B%22message%22%2C%22callback_query%22%2C%22pre_checkout_query%22%5D","",35);
            if (raw.empty()) continue; auto upd=json::parse(raw);
            if (!upd.contains("result")||!upd["result"].is_array()) continue;
            int ub=0;
            for (auto& u:upd["result"]) {
                if (!u.contains("update_id")) continue; long cuid=u["update_id"].get<long>();

                if (u.contains("callback_query")&&u["callback_query"].is_object()) {
                    handleCallbackQuery(u["callback_query"]);
                    offset=cuid+1; if (++ub%5==0) saveTgOffset(offset); continue;
                }

                if (u.contains("pre_checkout_query")&&u["pre_checkout_query"].is_object()) {
                    handlePreCheckoutQuery(u["pre_checkout_query"]);
                    offset=cuid+1; if (++ub%5==0) saveTgOffset(offset); continue;
                }

                if (u.contains("message")&&u["message"].is_object()&&u["message"].contains("successful_payment")
                    &&u["message"].contains("chat")&&u["message"]["chat"].is_object()&&u["message"]["chat"].contains("id")) {
                    std::string pcid=std::to_string(u["message"]["chat"]["id"].get<long>());
                    handleSuccessfulPayment(pcid, u["message"]["successful_payment"]);
                    offset=cuid+1; if (++ub%5==0) saveTgOffset(offset); continue;
                }
                if (!u.contains("message")||!u["message"].is_object()||!u["message"].contains("text")||!u["message"]["text"].is_string()) { offset=cuid+1; if (++ub%5==0) saveTgOffset(offset); continue; }
                std::string txt=u["message"]["text"].get<std::string>(), cid=std::to_string(u["message"]["chat"]["id"].get<long>());
                if (!g_rateLimiter.allow(cid)) { offset=cuid+1; if (++ub%5==0) saveTgOffset(offset); continue; }

                if (!txt.empty() && txt[0] == '/') {
                    g_sessionManager.clearSession(cid);

                    if (txt=="/start") {
                        bool isNewUser = false;
                        {
                            std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
                            if (prepareOrLog(db, &s, "SELECT 1 FROM users WHERE chat_id = ?")) {
                                sqlite3_bind_text(s, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
                                isNewUser = (sqlite3_step(s) != SQLITE_ROW);
                                sqlite3_finalize(s);
                            }
                        }

                        if (countUsers() >= MAX_USERS && isNewUser) {
                            sendMsg(cid, "⚠️ User limit reached. Please try again later.");
                        } else {
                            ensureUser(cid);
                            if (isNewUser) {
                                auto msg = TelegramUI::buildWelcomeMessage(cid);
                                sendMsg(cid, msg.text, msg.keyboard);
                            } else {
                                auto msg = TelegramUI::buildMainMenu(cid);
                                sendMsg(cid, msg.text, msg.keyboard);
                            }
                        }
                    }
                    else if (txt=="/health") {
                        if (cid != OWNER_CHAT_ID) {
                            sendMsg(cid, "Access denied.");
                        } else {
                            size_t curIdx = rpcIndex.load(std::memory_order_relaxed) % RPC_ENDPOINTS.size();
                            int diskFree = getDiskFreePercent();
                            time_t lastFail = g_stats.last_rpc_failure.load(std::memory_order_relaxed);
                            bool rpcHealthy = (lastFail==0) || (time(nullptr)-lastFail > 300);
                            std::stringstream ss2; ss2 << "✅ <b>OK</b>\n\n"
                                << "Block: <code>" << getLastBlock() << "</code>\n"
                                << "Queue: <b>" << g_msgQueue.size() << "</b>\n"
                                << "RPC: <b>" << (rpcHealthy?"healthy":"degraded") << "</b> (total failures: " << g_stats.rpc_failures.load() << ")\n"
                                << "RPC endpoint: <code>" << safeString(RPC_ENDPOINTS[curIdx], 48) << "</code>\n"
                                << "DB: <b>" << fileSizeMB(DB_FILE) << " MB</b> (WAL: " << fileSizeMB(DB_FILE + "-wal") << " MB)\n";
                            if (diskFree >= 0) {
                                ss2 << "Disk: <b>" << diskFree << "% free</b>\n";
                                if (diskFree < 15) ss2 << "\n⚠️ <b>LOW DISK SPACE!</b>\n";
                            } else {
                                ss2 << "Disk: <b>unknown</b>\n";
                            }
                            ss2 << "Uptime: <b>" << getUptime() << "</b>";
                            sendMsg(cid,ss2.str());
                        }
                    }
                    else if (txt=="/stats") {
                        if (cid != OWNER_CHAT_ID) {
                            sendMsg(cid, "Access denied.");
                        } else {
                            size_t qs=g_msgQueue.size(); size_t uc=countUsers(); int64_t fc=0;
                            { std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s; if (prepareOrLog(db,&s,"SELECT COUNT(*) FROM deliveries WHERE status=4")) { if (sqlite3_step(s)==SQLITE_ROW) fc=sqlite3_column_int64(s,0); sqlite3_finalize(s); } }
                            std::stringstream ss2; ss2 << "📊 <b>Stats</b>\n\n👥 Users: <b>" << uc << "</b>\n📬 Queue: <b>" << qs << "</b>\n❌ Failed: <b>" << fc << "</b>\n⏱ Uptime: <b>" << getUptime() << "</b>\n\n"
                                << "⚙️ RPC fail: " << g_stats.rpc_failures.load() << "\n💰 Price fb: " << g_stats.price_fallbacks.load() << "\n🔄 REORG: " << g_stats.reorg_verifications.load() << "\n📨 Sent: " << g_stats.alerts_sent.load() << "\n🔍 TX: " << g_stats.tx_processed.load();
                            {
                                uint64_t buy=g_stats.cov_buy.load(), sell=g_stats.cov_sell.load(), lpAdd=g_stats.cov_lp_add.load(),
                                         lpRemove=g_stats.cov_lp_remove.load(), wrap=g_stats.cov_wrap.load(), unwrap=g_stats.cov_unwrap.load(),
                                         xfer=g_stats.cov_transfer.load(), unk=g_stats.cov_unknown.load();
                                uint64_t total = buy+sell+lpAdd+lpRemove+wrap+unwrap+xfer+unk;
                                ss2 << "\n\n📈 <b>Coverage</b> (valid tx: " << total << ")\n"
                                    << "🟢 BUY: " << buy << "\n🚨 SELL: " << sell << "\n🌊 LP Add: " << lpAdd << "\n🌊 LP Remove: " << lpRemove
                                    << "\n🔄 Wrap: " << wrap << "\n🔄 Unwrap: " << unwrap
                                    << "\n📤 Transfer: " << xfer << "\n❓ Unknown: " << unk
                                    << "\n\n🔬 <b>Signals</b>\n💱 Swap Event: " << g_stats.sig_swap_event.load()
                                    << "\n🌐 Universal Router: " << g_stats.sig_universal_router.load()
                                    << "\n📦 Multicall: " << g_stats.sig_multicall.load()
                                    << "\n🔑 Permit2: " << g_stats.sig_permit2.load()
                                    << "\n\n🌊 <b>LP signals seen</b> (regardless of outcome)\n"
                                    << "Mint/Burn: " << g_stats.sig_lp_mint_burn.load()
                                    << "\nPool-identity: " << g_stats.sig_lp_pool_identity.load()
                                    << "\nV3 events: " << g_stats.sig_lp_v3_event.load()
                                    << "\n\n❓ <b>Unknown reasons</b>\n"
                                    << "Swap w/o wallet flow: " << g_stats.unk_swap_no_wallet_flow.load()
                                    << "\nOnly base flow: " << g_stats.unk_only_base_flow.load()
                                    << "\nUnconfirmed opposite: " << g_stats.unk_unconfirmed_opposite.load()
                                    << "\nLP not linked: " << g_stats.unk_lp_not_linked.load()
                                    << "\nOther: " << g_stats.unk_other.load()
                                    << "\n\n\xF0\x9F\xA9\xBA <b>Diagnostics</b>\n"
                                    << "Swap inferred from flow: " << g_stats.diag_swap_inferred.load()
                                    << "\nNative counter needs trace: " << g_stats.diag_native_counter.load();
                            }
                            if (qs>1000) ss2 << "\n\n⚠️ <b>QUEUE HIGH!</b>"; if (fc>0) ss2 << "\n⚠️ <b>FAILED DELIVERIES!</b>";
                            sendMsg(cid,ss2.str());
                        }
                    }
                    else if (txt=="/help") {
                        auto msg = TelegramUI::buildHelpMessage();
                        sendMsg(cid, msg.text, msg.keyboard);
                    }
                    else if (txt=="/premium") {
                        ensureUser(cid);
                        auto msg = buildPremiumPage(cid);
                        sendMsg(cid, msg.text, msg.keyboard);
                    }
                    else if (txt.find("/add ")==0) {
                        size_t p1=txt.find(' '),p2=txt.find(' ',p1+1);
                        if (p1==std::string::npos||p2==std::string::npos) { sendMsg(cid,"❌ Usage: /add 0x... Name"); }
                        else {
                            std::string addr=toLower(trim(txt.substr(p1+1,p2-p1-1)));
                            std::string label=trim(txt.substr(p2+1));
                            if (label.empty() || label == "-" || label == "." || toLower(label) == addr) label = shortAddress(addr);
                            if (label.length() > 32) { sendMsg(cid,"❌ Name is too long (max 32 characters)"); }
                            else {
                            auto res=addUserWhale(cid,addr,label);
                            switch (res) {
                                case AddWhaleResult::OK: refreshWatchers(); sendMsg(cid,"✅ Wallet added: "+safeString(label)); break;
                                case AddWhaleResult::ALREADY_EXISTS: sendMsg(cid,"⚠️ You're already tracking this wallet"); break;
                                case AddWhaleResult::LIMIT_REACHED: {
                                    if (isPremium(cid)) sendMsg(cid,"⚠️ You've reached the limit of 50 tracked wallets");
                                    else { auto lim = buildWalletLimitMessage(); sendMsg(cid, lim.text, lim.keyboard); }
                                    break;
                                }
                                case AddWhaleResult::BAD_ADDRESS: sendMsg(cid,"❌ That doesn't look like a valid address (expected 0x + 40 hex characters)"); break;
                                case AddWhaleResult::ERROR: sendMsg(cid,"❌ Something went wrong, please try again"); break;
                            }
                            }
                        }
                    }
                    else if (txt.find("/remove ")==0) {
                        std::string a=toLower(trim(txt.substr(8)));
                        bool removed=removeUserWhale(cid,a);
                        if (removed) { refreshWatchers(); sendMsg(cid,"✅ Wallet removed"); }
                        else sendMsg(cid,"⚠️ Address not found in your list. Check /list");
                    }
                    else if (txt.find("/limit ")==0) {
                        try { double v=std::stod(txt.substr(7));
                            if (v<0) { sendMsg(cid,"❌ Threshold must be positive"); }
                            else { setUserThreshold(cid,v); refreshWatchers(); sendMsg(cid,"✅ Threshold set to $"+formatThousands(static_cast<uint64_t>(v))); }
                        } catch (...) { sendMsg(cid,"❌ Usage: /limit 5000"); }
                    }
                    else if (txt.find("/language")==0) {
                        std::string rest = trim(txt.substr(9));
                        if (rest.empty()) { sendMsg(cid,"❌ Usage: /language en"); }
                        else { setUserLanguage(cid,toLower(rest)); sendMsg(cid,"✅ Language preference saved (message translation coming in a future version — alerts are currently in English)."); }
                    }
                    else if (txt=="/list") { sendMsg(cid,buildUserListText(cid)); }
                    else if (txt=="/top") {
                        auto msg = buildGlobalTopMenu();
                        sendMsg(cid, msg.text, msg.keyboard);
                    }
                    else if (txt.find("/toptrader ")==0) {
                        std::string arg = trim(txt.substr(11));
                        if (arg.empty()) sendMsg(cid, "❌ Usage: /toptrader TOKEN (symbol or contract address)");
                        else { auto msg = buildTopPnlMessage(cid, arg, 1); sendMsg(cid, msg.text, msg.keyboard); }
                    }
                    else {
                        sendMsg(cid, "🤔 Unknown command. Try /help or use the menu below.");
                        auto msg = TelegramUI::buildMainMenu(cid);
                        sendMsg(cid, msg.text, msg.keyboard);
                    }
                }
                else if (handleTextInput(cid, txt)) {
                }
                else {
                    auto msg = TelegramUI::buildMainMenu(cid);
                    sendMsg(cid, msg.text, msg.keyboard);
                }

                offset=cuid+1; if (++ub%5==0) saveTgOffset(offset);
            }
            if (ub>0) saveTgOffset(offset);
        } catch (...) { std::this_thread::sleep_for(std::chrono::seconds(2)); }
    }
}

int main() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) { std::cerr << "[FATAL] curl init failed" << std::endl; return 1; }
    std::signal(SIGINT,signalHandler); std::signal(SIGTERM,signalHandler);
    {
        const char* chainEnv = std::getenv("WHALE_CHAIN");
        std::string chainName = chainEnv ? toLower(std::string(chainEnv)) : "bsc";
        if (chainName == "ethereum" || chainName == "eth") { setChainContext(makeEthereumContext()); RPC_ENDPOINTS = ETHEREUM_RPC_ENDPOINTS; }
        else if (chainName == "base") { setChainContext(makeBaseContext()); RPC_ENDPOINTS = BASE_RPC_ENDPOINTS; }
        else if (chainName == "arbitrum" || chainName == "arb") { setChainContext(makeArbitrumContext()); RPC_ENDPOINTS = ARBITRUM_RPC_ENDPOINTS; }
        else if (chainName != "bsc") { std::cerr << "[FATAL] Unknown WHALE_CHAIN: " << chainName << std::endl; return 1; }
        std::cout << "[CHAIN] Running on " << chainName << " (native: " << chainCtx().nativeSymbol << ")" << std::endl;
    }
    initDB(); initRankingDB(); initPremium(TG_TOKEN, SERVICE_CHAT_ID); loadTokenCache();
    ensureUser(OWNER_CHAT_ID);
    refreshWatchers();
    setupBotCommands();
    size_t initialWatcherAddrs;
    { std::shared_lock l(watchersMutex); initialWatcherAddrs = WATCHERS_PTR->size(); }
    long long lb=getLastBlock(); if (lb==0) { auto b=rpc("eth_blockNumber",{}); long long tmp; if (b.is_string()&&hexToLL(b.get<std::string>(),tmp)) lb=tmp; }
    auto lj=rpc("eth_blockNumber",{}); long long tmpLat;
    if (lj.is_string()&&hexToLL(lj.get<std::string>(),tmpLat)) { long long lat=tmpLat; if (lat-lb>FAST_SYNC_LAG) { std::cout << "[FAST SYNC] Lag " << (lat-lb) << ", skip to latest-5" << std::endl; lb=lat-5; saveLastBlock(lb); saveLastBlockHash(""); } }
    std::cout << "🐋 Started. Block: " << lb << ", Users: " << countUsers() << ", Watched addresses: " << initialWatcherAddrs << std::endl;
    g_msgQueue.setDeadUserHandler([](const std::string& cid) {
        if (cid != SERVICE_CHAT_ID) removeUser(cid);
        else std::cout << "[USERS] Skip removing service account" << std::endl;
    });
    g_msgQueue.start(); std::thread tg(telegramLoop); std::thread rk(rankingCacheLoop);
    long long bsc=0; auto lcp=std::chrono::steady_clock::now(), lst=std::chrono::steady_clock::now(), lsq=std::chrono::steady_clock::now(), lcl=std::chrono::steady_clock::now();
    while (running.load(std::memory_order_relaxed)) {
        try {
            auto lj=rpc("eth_blockNumber",{}); long long lat;
            if (!lj.is_string()||!hexToLL(lj.get<std::string>(),lat)) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
            while (lb<lat&&running.load(std::memory_order_relaxed)) {
                long long next = lb+1;
                if (!processBlock(next)) {
                    lb = getLastBlock();
                    break;
                }
                lb = next; saveLastBlock(lb);
                bool nc=(++bsc>=200); if (!nc) { auto e=std::chrono::steady_clock::now()-lcp; nc=std::chrono::duration_cast<std::chrono::minutes>(e).count()>=5; }
                if (nc) { walCheckpoint(); cleanupOldTx(lb); bsc=0; lcp=std::chrono::steady_clock::now(); }
            }
            flushPendingAlerts(false);
            if (std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now()-lsq).count()>=5) {
                g_msgQueue.syncSize();
                if (!CHANNEL_ID.empty()) {
                    long long nowTs = static_cast<long long>(time(nullptr));
                    if (nowTs / 86400 > getChannelDigestAt() / 86400 && (nowTs % 86400) >= DIGEST_HOUR_UTC * 3600) {
                        auto digest = buildDailyChannelDigest();
                        if (!digest.text.empty()) {
                            if (!BOT_USERNAME.empty()) {
                                while (!digest.text.empty() && (digest.text.back() == '\n' || digest.text.back() == ' ')) digest.text.pop_back();
                                digest.text += "\n\n🤖 Track these wallets with @" + safeString(BOT_USERNAME, 32);
                            }
                            if (g_msgQueue.enqueueToRecipients(digest.text, {CHANNEL_ID})) {
                                saveChannelDigestAt(nowTs);
                                std::cout << "[CHANNEL] Daily Top 10 digest sent" << std::endl;
                            }
                        }
                    }
                }
                lsq=std::chrono::steady_clock::now();
            }
            if (std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now()-lcl).count()>=30) { cleanupOldAlerts(); cleanupOldTrades(); cleanupExpiredPremium(); lcl=std::chrono::steady_clock::now(); }
            if (std::chrono::duration_cast<std::chrono::hours>(std::chrono::steady_clock::now()-lst).count()>=1) {
                std::cout << "[STATS] rpc_fail=" << g_stats.rpc_failures.load() << " price_fb=" << g_stats.price_fallbacks.load()
                    << " reorg=" << g_stats.reorg_verifications.load() << " tx=" << g_stats.tx_processed.load() << " sent=" << g_stats.alerts_sent.load()
                    << " queue=" << g_msgQueue.size() << " uptime=" << getUptime() << std::endl; lst=std::chrono::steady_clock::now(); }
        } catch (const std::exception& e) { std::cerr << "[ERROR] " << e.what() << std::endl; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "[SHUTDOWN] Stopping..." << std::endl;
    flushPendingAlerts(true);
    g_msgQueue.stop();
    tg.join();
    rk.join();
    walCheckpoint();
    closeRankingDB();
    if (db) sqlite3_close(db);
    curl_global_cleanup();
    std::cout << "[SHUTDOWN] Clean exit." << std::endl; return 0;
}
