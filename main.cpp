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
#include <cmath>
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
#include "rpc_client.h"
#include "chains.h"
#include "wallet_menu.h"
#include "ru.h"
#include "premium.h"
#include "message_queue.h"
#include "tx_analyzer.h"
#include "beneficiary_stats.h"
#include "hyperliquid.h"

using json = nlohmann::json;
using boost::multiprecision::cpp_int;

struct Stats {
    std::atomic<uint64_t> rpc_failures{0};
    std::atomic<uint64_t> price_fallbacks{0};
    std::atomic<uint64_t> reorg_verifications{0};
    std::atomic<uint64_t> tx_processed{0};
    std::atomic<uint64_t> alerts_sent{0};
    std::atomic<time_t> last_rpc_failure{0};
    std::atomic<int64_t> current_lag{0};
    std::atomic<int64_t> max_lag_seen{0};

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
    std::atomic<uint64_t> diag_native_unwrap{0};
    std::atomic<uint64_t> diag_native_refund{0};
    std::atomic<uint64_t> diag_vault_flow_attributed{0};
} g_stats;

struct CoverageSet {
    std::atomic<uint64_t> buy{0}, sell{0}, lp_add{0}, lp_remove{0}, wrap{0}, unwrap{0},
                          transfer{0}, interaction{0}, arbitrage{0}, unknown{0};
};
CoverageSet g_covUser, g_covSvc;

void recordCoverage(const TxResult& r, bool serviceOnly) {
    CoverageSet& c = serviceOnly ? g_covSvc : g_covUser;
    if (r.venue == "Add Liquidity") c.lp_add.fetch_add(1, std::memory_order_relaxed);
    else if (r.venue == "Remove Liquidity") c.lp_remove.fetch_add(1, std::memory_order_relaxed);
    else if (r.venue == "Wrap") c.wrap.fetch_add(1, std::memory_order_relaxed);
    else if (r.venue == "Unwrap") c.unwrap.fetch_add(1, std::memory_order_relaxed);
    else if (r.isSwap) { if (r.isBuy) c.buy.fetch_add(1, std::memory_order_relaxed); else c.sell.fetch_add(1, std::memory_order_relaxed); }
    else if (r.venue == "DEX interaction") c.interaction.fetch_add(1, std::memory_order_relaxed);
    else if (r.venue == "Arbitrage") c.arbitrage.fetch_add(1, std::memory_order_relaxed);
    else if (!r.unknownReason.empty()) c.unknown.fetch_add(1, std::memory_order_relaxed);
    else c.transfer.fetch_add(1, std::memory_order_relaxed);
    if (r.hasSwapEvent) g_stats.sig_swap_event.fetch_add(1, std::memory_order_relaxed);
    if (r.isUniversalRouter) g_stats.sig_universal_router.fetch_add(1, std::memory_order_relaxed);
    if (r.isGenericMulticall) g_stats.sig_multicall.fetch_add(1, std::memory_order_relaxed);
    if (r.hasPermit2Signal) g_stats.sig_permit2.fetch_add(1, std::memory_order_relaxed);
    if (r.erc20MintOrBurnSeen) g_stats.sig_lp_mint_burn.fetch_add(1, std::memory_order_relaxed);
    if (r.lpPoolIdentitySeen) g_stats.sig_lp_pool_identity.fetch_add(1, std::memory_order_relaxed);
    if (r.lpV3EventSeen) g_stats.sig_lp_v3_event.fetch_add(1, std::memory_order_relaxed);
    if (r.diagnosticReason == "SWAP_EVENT_WITHOUT_WALLET_FLOW" || r.diagnosticReason == "DEX_SIGNAL_WITHOUT_WALLET_FLOW") g_stats.unk_swap_no_wallet_flow.fetch_add(1, std::memory_order_relaxed);
    else if (r.diagnosticReason == "ONLY_BASE_ASSET_FLOW") g_stats.unk_only_base_flow.fetch_add(1, std::memory_order_relaxed);
    if (r.unknownReason == "UNCONFIRMED_OPPOSITE_FLOW") g_stats.unk_unconfirmed_opposite.fetch_add(1, std::memory_order_relaxed);
    else if (r.unknownReason == "LP_EVENT_NOT_LINKED_TO_WALLET") g_stats.unk_lp_not_linked.fetch_add(1, std::memory_order_relaxed);
    else if (!r.unknownReason.empty()) g_stats.unk_other.fetch_add(1, std::memory_order_relaxed);
    if (r.diagnosticReason == "SWAP_INFERRED_FROM_FLOW") g_stats.diag_swap_inferred.fetch_add(1, std::memory_order_relaxed);
    else if (r.diagnosticReason == "NATIVE_COUNTER_REQUIRES_TRACE") g_stats.diag_native_counter.fetch_add(1, std::memory_order_relaxed);
    else if (r.diagnosticReason == "NATIVE_COUNTER_FROM_ROUTER_UNWRAP") g_stats.diag_native_unwrap.fetch_add(1, std::memory_order_relaxed);
    else if (r.diagnosticReason == "NATIVE_REFUND_ADJUSTED") g_stats.diag_native_refund.fetch_add(1, std::memory_order_relaxed);
    else if (r.diagnosticReason == "VAULT_FLOW_ATTRIBUTED") g_stats.diag_vault_flow_attributed.fetch_add(1, std::memory_order_relaxed);
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

const bool LOG_BENEFICIARY = []() {
    const char* env = std::getenv("WHALE_LOG_BENEFICIARY");
    return env && (std::string(env) == "1" || std::string(env) == "true");
}();
std::mutex beneficiaryLogMutex;
void logBeneficiaries(const std::string& hash, const nlohmann::json& tx, const TxResult& res) {
    if (!LOG_BENEFICIARY || res.flowBeneficiaries.empty()) return;
    std::string to = (tx.is_object() && tx.contains("to") && tx["to"].is_string()) ? tx["to"].get<std::string>() : "";
    std::stringstream ss;
    ss << "hash=" << hash << " to=" << to << " beneficiaries=[" << res.flowBeneficiaries << "]";
    std::lock_guard<std::mutex> lk(beneficiaryLogMutex);
    std::ofstream f("beneficiary.log", std::ios::app);
    if (f) f << ss.str() << "\n";
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

constexpr int TRIAL_DAYS = 7;
const std::string OWNER_CHAT_ID = "546348566";
const std::string SERVICE_CHAT_ID = "7479880531";
const std::string DB_FILE = "whale_bot.db";

const long long FAST_SYNC_LAG = 1000;
const long long REORG_ROLLBACK = 5;
const long long TX_TTL_BLOCKS = 6700;
constexpr time_t PRICE_TTL = 120;
constexpr size_t MAX_USERS = 1000000;

double nanosToUsd(uint64_t nanos) { return static_cast<double>(nanos) / 1000000000.0; }

std::atomic<bool> running{true};
std::atomic<int64_t> g_lastProcessedBlock{0};
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
        CREATE TABLE IF NOT EXISTS trial_granted (
            chat_id TEXT PRIMARY KEY,
            granted_at INTEGER NOT NULL
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
        CREATE TABLE IF NOT EXISTS token_price_history (
            address TEXT NOT NULL, ts INTEGER NOT NULL, price_nanos INTEGER NOT NULL,
            PRIMARY KEY (address, ts));
        CREATE INDEX IF NOT EXISTS idx_price_hist_ts ON token_price_history(ts);
        CREATE TABLE IF NOT EXISTS alerts (id INTEGER PRIMARY KEY AUTOINCREMENT, message TEXT NOT NULL, created_at INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS deliveries (
            id INTEGER PRIMARY KEY AUTOINCREMENT, alert_id INTEGER NOT NULL, chat_id TEXT NOT NULL,
            status INTEGER DEFAULT 0, retry_count INTEGER DEFAULT 0, next_retry_at INTEGER DEFAULT 0,
            priority INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY(alert_id) REFERENCES alerts(id) ON DELETE CASCADE);
        CREATE INDEX IF NOT EXISTS idx_deliveries_queue ON deliveries(status, next_retry_at, id) WHERE status IN (0,3);
        CREATE INDEX IF NOT EXISTS idx_deliveries_prio ON deliveries(status, next_retry_at, priority DESC, id) WHERE status IN (0,3);
        CREATE INDEX IF NOT EXISTS idx_deliveries_terminal ON deliveries(status, alert_id) WHERE status IN (1,2,4);
        CREATE TABLE IF NOT EXISTS wallet_tokens (
            wallet TEXT NOT NULL,
            token TEXT NOT NULL,
            last_seen INTEGER NOT NULL,
            PRIMARY KEY (wallet, token)
        );
        INSERT OR IGNORE INTO state(key,value) VALUES ('tg_offset','0');
    )";
    {
        char* mErr = nullptr;
        if (sqlite3_exec(db, "ALTER TABLE deliveries ADD COLUMN priority INTEGER NOT NULL DEFAULT 0",
                         nullptr, nullptr, &mErr) == SQLITE_OK)
            std::cout << "[STARTUP] deliveries: added priority column" << std::endl;
        if (mErr) sqlite3_free(mErr);
    }

    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[FATAL] Schema init failed: " << err << std::endl; sqlite3_free(err); sqlite3_close(db); std::exit(1);
    }

    {
        const char* clampSql = "UPDATE users SET threshold_nanos = 50000000000 WHERE threshold_nanos < 50000000000";
        char* cerr2 = nullptr;
        if (sqlite3_exec(db, clampSql, nullptr, nullptr, &cerr2) != SQLITE_OK) {
            std::cerr << "[STARTUP] threshold normalisation failed: " << (cerr2 ? cerr2 : "") << std::endl;
            sqlite3_free(cerr2);
        } else {
            int n = sqlite3_changes(db);
            if (n > 0) std::cout << "[STARTUP] Raised " << n << " user threshold(s) to the $50 minimum" << std::endl;
        }
    }
}

void walCheckpoint(int mode = SQLITE_CHECKPOINT_TRUNCATE) { std::lock_guard<std::mutex> l(dbMutex); sqlite3_wal_checkpoint_v2(db,nullptr,mode,nullptr,nullptr); }
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
constexpr long long PRICE_HISTORY_STEP_SEC = 3600;
constexpr long long PRICE_HISTORY_TTL_SEC  = 90LL * 86400LL;

void savePriceHistory(const std::string& a, uint64_t pn) {
    if (!pn) return;
    const long long slot = (static_cast<long long>(time(nullptr)) / PRICE_HISTORY_STEP_SEC)
                         * PRICE_HISTORY_STEP_SEC;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "INSERT OR IGNORE INTO token_price_history(address,ts,price_nanos) VALUES(?,?,?)")) return;
    sqlite3_bind_text(s, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, slot);
    sqlite3_bind_int64(s, 3, static_cast<sqlite3_int64>(pn));
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void cleanupPriceHistory() {
    const long long cutoff = static_cast<long long>(time(nullptr)) - PRICE_HISTORY_TTL_SEC;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "DELETE FROM token_price_history WHERE ts<?")) return;
    sqlite3_bind_int64(s, 1, cutoff);
    if (sqlite3_step(s) == SQLITE_DONE) {
        const int n = sqlite3_changes(db);
        if (n > 0) std::cout << "[PRICE-HIST] удалено снимков старше срока: " << n << std::endl;
    }
    sqlite3_finalize(s);
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
void refreshWatchers() {
    auto m = std::make_shared<std::unordered_map<std::string, std::vector<Watcher>>>();
    long long now = static_cast<long long>(time(nullptr));
    bool queryOk = false;
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
            int stepRc;
            while ((stepRc = sqlite3_step(s)) == SQLITE_ROW) {
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
            queryOk = (stepRc == SQLITE_DONE);
            if (!queryOk) std::cerr << "[WATCHERS] refresh query step failed mid-read (rc=" << stepRc << "): " << sqlite3_errmsg(db) << std::endl;
            sqlite3_finalize(s);
        }
    }
    if (!queryOk) {
        std::cerr << "[WATCHERS] refresh query failed - keeping previous watcher list (not wiping to empty)" << std::endl;
        return;
    }
    std::unique_lock l(watchersMutex);
    WATCHERS_PTR = m;
}

std::vector<std::string> hlWatchedAddresses() {
    std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> snapshot;
    { std::shared_lock l(watchersMutex); snapshot = WATCHERS_PTR; }
    std::vector<std::string> out;
    if (!snapshot) return out;
    out.reserve(snapshot->size());
    for (const auto& kv : *snapshot) out.push_back(kv.first);
    return out;
}

std::vector<HlUserWallet> hlUserWallets(const std::string& chatId) {
    std::vector<HlUserWallet> out;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT wa.address, uw.label FROM user_whales uw "
        "JOIN whale_addresses wa ON wa.id = uw.whale_id "
        "WHERE uw.user_id = ? ORDER BY uw.created_at")) return out;
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) {
        HlUserWallet w;
        w.address = toLower(safeColumnText(s, 0));
        w.label = safeColumnText(s, 1);
        if (!w.address.empty()) out.push_back(std::move(w));
    }
    sqlite3_finalize(s);
    return out;
}

std::vector<HlRecipient> hlWatchersFor(const std::string& addressLower) {
    std::vector<HlRecipient> out;
    std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> snapshot;
    { std::shared_lock l(watchersMutex); snapshot = WATCHERS_PTR; }
    if (!snapshot) return out;
    auto it = snapshot->find(addressLower);
    if (it == snapshot->end()) return out;
    out.reserve(it->second.size());
    for (const Watcher& w : it->second)
        out.push_back(HlRecipient{w.chatId, w.label, w.thresholdNanos});
    return out;
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

// Пробный период: 7 дней премиума один раз на аккаунт. Отметка о выдаче живёт
// в своей таблице и не удаляется вместе с пользователем - иначе достаточно было
// бы заблокировать бота и вернуться, чтобы получить пробный период заново.
bool trialAlreadyGranted(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "SELECT 1 FROM trial_granted WHERE chat_id=?")) return true;
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return found;
}

void markTrialGranted(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "INSERT OR IGNORE INTO trial_granted(chat_id,granted_at) VALUES(?,?)")) return;
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, static_cast<sqlite3_int64>(time(nullptr)));
    sqlite3_step(s);
    sqlite3_finalize(s);
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
    Lang lang = langFromCode(getUserLanguage(chatId));

    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_add_wallet")}, {"callback_data", "menu:add_wallet"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_account") + " (" + std::to_string(walletCount) + ")"}, {"callback_data", "menu:account"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_alert_threshold") + " ($" + formatThousands(static_cast<uint64_t>(thresholdUsd)) + ")"}, {"callback_data", "menu:alert_threshold"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_top_traders")}, {"callback_data", "menu:toptrader"}}
    }));
    // Счётчик дней прямо на кнопке: подписка кончается тихо, и человек узнавал
    // об этом, только когда переставали приходить перп-алерты. Замок у тех, у
    // кого её нет, - чтобы платное было видно до нажатия.
    std::string premiumLabel = tr(lang, "menu_premium");
    if (isPremium(chatId)) {
        const long long expire = premiumExpireTs(chatId);
        const long long now = static_cast<long long>(time(nullptr));
        if (expire > now) {
            const long long days = (expire - now + 86399) / 86400;   // вверх: последний день ещё идёт
            premiumLabel += " (" + std::to_string(days) + " " + tr(lang, "unit_day") + ")";
        }
    } else {
        premiumLabel += " \U0001F512";
    }
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", premiumLabel}, {"callback_data", "menu:premium"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_languages")}, {"callback_data", "menu:languages"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_help")}, {"callback_data", "menu:help"}}
    }));

    std::stringstream text;
    text << tr(lang, "menu_title") << "\n\n";
    if (walletCount == 0) {
        text << tr(lang, "menu_no_wallets");
    } else if (lang == Lang::RU) {
        std::string walletWord = pluralRu(static_cast<long long>(walletCount), "кошелёк", "кошелька", "кошельков");
        text << tr(lang, "menu_tracking_prefix") << " <b>" << walletCount << "</b> " << walletWord
             << ", " << tr(lang, "menu_alerts_above") << " <b>$" << formatThousands(static_cast<uint64_t>(thresholdUsd)) << "</b>.";
    } else {
        text << tr(lang, "menu_tracking_prefix") << " <b>" << walletCount << "</b> wallet" << (walletCount == 1 ? "" : "s")
             << ", " << tr(lang, "menu_alerts_above") << " <b>$" << formatThousands(static_cast<uint64_t>(thresholdUsd)) << "</b>.";
    }
    return {text.str(), keyboard.dump()};
}

UIMessage buildWelcomeMessage(const std::string& chatId) {
    auto msg = buildMainMenu(chatId);
    Lang lang = langFromCode(getUserLanguage(chatId));
    if (lang == Lang::RU) {
        msg.text = "🚨 <b>Добро пожаловать в Wallet Tracker!</b>\n\n"
                 "Отслеживайте кошельки китов в сети " + chainCtx().displayName + " и получайте мгновенные уведомления о покупках, продажах и переводах.\n\n"
                 "Нажмите кнопку ниже, чтобы начать:";
    } else {
        msg.text = "🚨 <b>Welcome to Wallet Tracker!</b>\n\n"
                 "Monitor whale wallets on " + chainCtx().displayName + " and get instant notifications for buys, sells and transfers.\n\n"
                 "Tap a button below to get started:";
    }
    return msg;
}

std::string buildCancelButton(Lang lang) {
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "cancel_button")}, {"callback_data", "cancel"}}
    }));
    return keyboard.dump();
}

std::string buildCancelWithTopTraders(Lang lang) {
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_top_traders")}, {"callback_data", "menu:toptrader"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "cancel_button")}, {"callback_data", "cancel"}}
    }));
    return keyboard.dump();
}

std::string buildCancelWithSpotTop(Lang lang) {
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_top_traders")}, {"callback_data", "menu:toptrader_spot"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "cancel_button")}, {"callback_data", "cancel"}}
    }));
    return keyboard.dump();
}

UIMessage buildLanguagesMenu(const std::string& chatId) {
    static const std::vector<std::pair<std::string, std::string>> LANGUAGES = {
        {"en", "🇬🇧 English"},
        {"ru", "🇷🇺 Русский"},
        {"es", "🇪🇸 Español"},
        {"pt", "🇧🇷 Português"},
        {"fr", "🇫🇷 Français"},
        {"tr", "🇹🇷 Türkçe"},
        {"ar", "🇸🇦 العربية"},
    };
    std::string current = getUserLanguage(chatId);
    Lang lang = langFromCode(current);

    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    for (const auto& l : LANGUAGES) {
        std::string labelText = l.second + (l.first == current ? " ✅" : "");
        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", labelText}, {"callback_data", "lang:" + l.first}}
        }));
    }
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    std::string currentLabel = current;
    for (const auto& l : LANGUAGES) if (l.first == current) { currentLabel = l.second; break; }
    std::string text = tr(lang, "lang_title") + "\n" + tr(lang, "lang_current") + " <b>" + currentLabel + "</b>\n\n" + tr(lang, "lang_choose");
    return {text, keyboard.dump()};
}

UIMessage buildHelpMessage(const std::string& chatId) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    std::string text = tr(lang, "help_title") + "\n\n";
    text += tr(lang, "help_intro") + "\n\n";
    text += tr(lang, "help_commands") + "\n";
    text += tr(lang, "help_menu_add") + "\n";
    text += tr(lang, "help_menu_mywallets") + "\n";
    text += tr(lang, "help_menu_threshold") + "\n";
    text += tr(lang, "help_menu_top") + "\n";
    text += tr(lang, "help_menu_premium") + "\n";
    text += tr(lang, "help_menu_languages") + "\n\n";
    text += tr(lang, "help_premium_title") + "\n";
    text += tr(lang, "help_premium_1") + "\n";
    text += tr(lang, "help_premium_2") + "\n";
    text += tr(lang, "help_premium_3") + "\n";
    text += tr(lang, "help_premium_4") + "\n\n";
    text += tr(lang, "help_support") + "\n";
    text += tr(lang, "help_channel") + "\n\n";
    text += tr(lang, "help_footer") + "\n\n";
    text += tr(lang, "help_disclaimer");

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
    try {
        auto p = json::parse(r);
        if (p.value("ok", false)) return true;
        std::string desc = p.value("description", std::string());
        if (desc.find("message is not modified") != std::string::npos) return true;
        return false;
    } catch (...) { return false; }
}

void replyInPlace(const std::string& chatId, long long messageId, const std::string& text, const std::string& keyboard) {
    std::string kb = keyboard.empty() ? "{\"inline_keyboard\":[]}" : keyboard;
    if (messageId <= 0 || !editMsg(chatId, messageId, text, kb)) {
        sendMsg(chatId, text, keyboard);
    }
}

void deleteMsg(const std::string& chatId, long long messageId) {
    if (messageId <= 0) return;
    json j;
    j["chat_id"] = chatId;
    j["message_id"] = messageId;
    http("https://api.telegram.org/bot" + TG_TOKEN + "/deleteMessage", j.dump());
}

void answerCallbackQuery(const std::string& callbackQueryId, const std::string& text, bool showAlert) {
    json j;
    j["callback_query_id"] = callbackQueryId;
    if (!text.empty()) j["text"] = text;
    if (showAlert) j["show_alert"] = true;
    http("https://api.telegram.org/bot" + TG_TOKEN + "/answerCallbackQuery", j.dump());
}

void setupBotCommands() {
    json cmds = json::array();
    cmds.push_back({{"command","start"},{"description","Open the main menu"}});
    json j; j["commands"] = cmds;
    http("https://api.telegram.org/bot" + TG_TOKEN + "/setMyCommands", j.dump());
}

void seedWalletTokensFromTrades() {
    std::lock_guard<std::mutex> l(dbMutex);
    const char* sql =
        "INSERT OR IGNORE INTO wallet_tokens(wallet, token, last_seen) "
        "SELECT wallet, token, MAX(timestamp) FROM trades GROUP BY wallet, token";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[STARTUP] wallet_tokens seed failed: " << (err ? err : "") << std::endl;
        sqlite3_free(err);
        return;
    }
    int n = sqlite3_changes(db);
    if (n > 0) std::cout << "[STARTUP] Seeded " << n << " wallet/token pairs from trade history" << std::endl;
}

void rememberWalletToken(const std::string& wallet, const std::string& token, long long ts) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT INTO wallet_tokens(wallet,token,last_seen) VALUES(?,?,?) "
                            "ON CONFLICT(wallet,token) DO UPDATE SET last_seen=excluded.last_seen")) return;
    sqlite3_bind_text(s,1,toLower(wallet).c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,toLower(token).c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(s,3,ts);
    sqlite3_step(s); sqlite3_finalize(s);
}

void forgetWalletToken(const std::string& wallet, const std::string& token) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"DELETE FROM wallet_tokens WHERE wallet=? AND token=?")) return;
    sqlite3_bind_text(s,1,toLower(wallet).c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,toLower(token).c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_step(s); sqlite3_finalize(s);
}

void rememberTokensFromReceipt(const std::string& wallet, const nlohmann::json& receipt, long long ts) {
    if (!receipt.is_object() || !receipt.contains("logs") || !receipt["logs"].is_array()) return;
    static const std::string TRANSFER_TOPIC =
        "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
    const std::string w = toLower(wallet);
    std::set<std::string> seen;

    for (const auto& lg : receipt["logs"]) {
        if (!lg.is_object() || !lg.contains("topics") || !lg["topics"].is_array()) continue;
        const auto& tp = lg["topics"];
        if (tp.size() < 3 || !tp[0].is_string()) continue;
        if (toLower(tp[0].get<std::string>()) != TRANSFER_TOPIC) continue;

        bool involvesWallet = false;
        for (int i = 1; i <= 2 && !involvesWallet; ++i) {
            if (!tp[i].is_string()) continue;
            std::string t = toLower(tp[i].get<std::string>());
            if (t.size() >= 40 && ("0x" + t.substr(t.size() - 40)) == w) involvesWallet = true;
        }
        if (!involvesWallet) continue;

        if (!lg.contains("address") || !lg["address"].is_string()) continue;
        std::string token = toLower(lg["address"].get<std::string>());
        if (token.empty() || !seen.insert(token).second) continue;
        rememberWalletToken(wallet, token, ts);
    }
}

std::vector<std::string> getWalletTokens(const std::string& wallet, int limit) {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT token FROM wallet_tokens WHERE wallet=? ORDER BY last_seen DESC LIMIT ?")) return out;
    sqlite3_bind_text(s,1,toLower(wallet).c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_int(s,2,limit);
    while (sqlite3_step(s)==SQLITE_ROW) out.push_back(safeColumnText(s,0));
    sqlite3_finalize(s);
    return out;
}

bool getNativeBalance(const std::string& wallet, cpp_int& out) {
    auto r = rpc("eth_getBalance", {wallet, "latest"});
    if (!r.is_string()) { g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed); return false; }
    out = hexToCppInt(r.get<std::string>());
    return true;
}

bool getTokenBalance(const std::string& token, const std::string& wallet, cpp_int& out) {
    std::string w = toLower(wallet);
    if (w.size() == 42 && w.rfind("0x",0) == 0) w = w.substr(2);
    std::string data = "0x70a08231" + std::string(24, '0') + w;
    auto r = rpc("eth_call", {{{"to", token}, {"data", data}}, "latest"});
    if (!r.is_string()) { g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed); return false; }
    out = hexToCppInt(r.get<std::string>());
    return true;
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
std::map<std::string, double> POOL_LIQUIDITY_CACHE;

double getPoolLiquidityUsd(const std::string& token) {
    std::lock_guard<std::mutex> l(cacheMutex);
    auto it = POOL_LIQUIDITY_CACHE.find(toLower(token));
    return it != POOL_LIQUIDITY_CACHE.end() ? it->second : 0.0;
}

uint64_t getPriceNanos(const std::string& token) {
    std::string a=toLower(token);
    { std::lock_guard<std::mutex> l(cacheMutex); if (PRICE_NANOS_CACHE.count(a)&&time(nullptr)-PRICE_NANOS_CACHE[a].second<PRICE_TTL) return PRICE_NANOS_CACHE[a].first; }
    double p=0;
    auto r=http("https://api.dexscreener.com/latest/dex/tokens/"+token);
    try {
        auto j=json::parse(r);
        if (j.contains("pairs") && j["pairs"].is_array()) {
            const std::string wantChain = chainCtx().dexscreenerChainId;
            double bestLiquidity = -1.0;
            for (const auto& pair : j["pairs"]) {
                if (!pair.is_object()) continue;
                if (!wantChain.empty() && pair.value("chainId", std::string()) != wantChain) continue;
                if (!pair.contains("priceUsd") || !pair["priceUsd"].is_string()) continue;
                double liq = 0.0;
                if (pair.contains("liquidity") && pair["liquidity"].is_object() &&
                    pair["liquidity"].contains("usd") && pair["liquidity"]["usd"].is_number())
                    liq = pair["liquidity"]["usd"].get<double>();
                double price = 0.0;
                try { price = std::stod(pair["priceUsd"].get<std::string>()); } catch (...) { continue; }
                if (!std::isfinite(price) || price <= 0.0) continue;
                if (liq > bestLiquidity) { bestLiquidity = liq; p = price; }
            }
            if (bestLiquidity >= 0.0) {
                std::lock_guard<std::mutex> l(cacheMutex);
                POOL_LIQUIDITY_CACHE[a] = bestLiquidity;
            }
        }
    } catch (...) {}
    if (p==0) {
        const std::string platform = chainCtx().coingeckoPlatform.empty()
                                   ? std::string("binance-smart-chain") : chainCtx().coingeckoPlatform;
        auto r2=http("https://api.coingecko.com/api/v3/simple/token_price/"+platform+"?contract_addresses="+token+"&vs_currencies=usd");
        try { auto j2=json::parse(r2); if (j2.contains(a)&&j2[a].contains("usd")&&j2[a]["usd"].is_number()) {
            double cg = j2[a]["usd"].get<double>();
            if (std::isfinite(cg) && cg > 0.0) p = cg;
        } } catch (...) {} }
    constexpr double MAX_SANE_PRICE_USD = 1e9;
    uint64_t n = (std::isfinite(p) && p > 0.0 && p < MAX_SANE_PRICE_USD)
               ? static_cast<uint64_t>(p * 1000000000.0) : 0;
    if (n>0) {
        { std::lock_guard<std::mutex> l(cacheMutex); PRICE_NANOS_CACHE[a]={n,time(nullptr)}; }
        saveTokenPrice(a,n);
        savePriceHistory(a,n);
    }
    else { std::lock_guard<std::mutex> l(cacheMutex); if (PRICE_NANOS_CACHE.count(a)&&PRICE_NANOS_CACHE[a].first>0) { g_stats.price_fallbacks.fetch_add(1); std::cerr << "[PRICE] Stale cache: " << a << std::endl; return PRICE_NANOS_CACHE[a].first; } }
    return n;
}

std::string buildAlertMessage(const std::string& label, const TxResult& res, const std::string& hash, Lang lang) {
    bool tokenIsNative = (res.tokenAddr == chainCtx().nativeMarker);
    std::string tokenSymbol = tokenIsNative ? chainCtx().nativeSymbol : safeString(getSymbol(res.tokenAddr), 32);
    int tokenDecimals = tokenIsNative ? 18 : getDecimals(res.tokenAddr);
    std::string msg="\U0001F4BC <b>"+safeString(label)+"</b>\n\n";
    if (res.venue == "Add Liquidity") msg+="\U0001F30A <b>" + tr(lang, "alert_add_liquidity") + "</b>";
    else if (res.venue == "Remove Liquidity") msg+="\U0001F30A <b>" + tr(lang, "alert_remove_liquidity") + "</b>";
    else if (res.venue == "Collect Fees") msg+="\U0001F4B8 <b>" + tr(lang, "alert_collect_fees") + "</b>";
    else if (res.venue == "Wrap") msg+="\U0001F504 <b>" + tr(lang, "alert_wrap") + " " + chainCtx().nativeSymbol + "</b>";
    else if (res.venue == "Unwrap") msg+="\U0001F504 <b>" + tr(lang, "alert_unwrap") + " " + chainCtx().nativeSymbol + "</b>";
    else if (res.venue == "Bridge Out") msg+="\U0001F309 <b>" + tr(lang, "alert_bridge_out") + "</b>";
    else if (res.venue == "Bridge In") msg+="\U0001F309 <b>" + tr(lang, "alert_bridge_in") + "</b>";
    else if (res.venue == "Arbitrage") msg+="\u267B\uFE0F <b>" + tr(lang, "alert_arbitrage") + "</b>";
    else msg+=res.isSwap?(res.isBuy?"\U0001F7E2 <b>"+tr(lang,"alert_buy")+"</b>":"\U0001F6A8 <b>"+tr(lang,"alert_sell")+"</b>"):"\U0001F4E4 <b>"+tr(lang,"alert_transfer")+"</b>";
    msg+="\n\U0001F4B0 " + tr(lang, "alert_amount") + ": <b>"+formatUsd(res.usdNanos)+"</b>\n";
    msg+="\U0001FA99 " + tr(lang, "alert_token") + ": <b>"+tokenSymbol+"</b>\n";
    msg+="\U0001F4E6 " + tr(lang, "alert_qty") + ": <b>"+formatAmount(res.rawAmount,tokenDecimals)+"</b>\n";
    if (res.isSwap) {
        cpp_int unitPriceNanos = calcUnitPriceNanos(res.usdNanos, res.rawAmount, tokenDecimals);
        std::string priceLabel = tr(lang, res.isBuy ? "alert_buy_price" : "alert_sell_price");
        msg += "\U0001F4B5 " + priceLabel + ": <b>" + formatPriceUsd(unitPriceNanos) + "</b>\n";
    }
    if (res.isSwap && !res.counterAddr.empty()) {
        std::string counterLabel = tr(lang, res.isBuy ? "alert_spent" : "alert_received");
        std::string counterAmountStr, counterSymbol;
        if (res.counterAddr == chainCtx().nativeMarker) {
            counterAmountStr = formatAmount(res.counterAmount, 18);
            counterSymbol = chainCtx().nativeSymbol;
        } else {
            counterAmountStr = formatAmount(res.counterAmount, getDecimals(res.counterAddr));
            counterSymbol = safeString(getSymbol(res.counterAddr), 16);
        }
        msg += (res.isBuy ? "\U0001F4C9 " : "\U0001F4C8 ") + counterLabel + ": <b>" +
               counterAmountStr + " " + counterSymbol + "</b>\n";
    }
    if (res.gasUsdNanos > 0) {
        long long gasLL = 0;
        try { gasLL = std::stoll(res.gasUsdNanos.convert_to<std::string>()); } catch (...) {}
        msg += "\u26FD " + tr(lang, "alert_gas") + ": <b>" +
               formatUsdSmall(gasLL) + "</b>\n";
    }
    if (!tokenIsNative) msg+="\U0001F4DC " + tr(lang, "alert_contract") + ": <code>"+safeString(res.tokenAddr)+"</code>\n";
    msg+="\U0001F194 TX: <code>"+safeString(hash,66)+"</code>\n";
    msg+="\U0001F4BC " + tr(lang, "alert_wallet") + ": <b>"+safeString(label)+"</b>\n\n";
    msg+="\U0001F517 <a href=\""+chainCtx().explorerUrl+"/tx/"+hash+"\">" + tr(lang, "alert_transaction") + "</a>";
    return msg;
}

namespace {
constexpr long long AGGREGATION_WINDOW_SECONDS = 180;

struct PendingAlert {
    std::string wallet;
    TxResult agg;
    std::string hash;
    long long block = 0;
    long long blockTs = 0;
    long long firstSeen = 0;
};

std::unordered_map<std::string, PendingAlert> g_pendingAlerts;
std::mutex g_pendingMutex;
}

void dispatchAlert(const std::string& mA, const TxResult& res, const std::string& hash) {
    std::map<std::pair<std::string, Lang>, std::vector<std::string>> byLabelLang;
    {
        std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> watchers;
        { std::shared_lock l(watchersMutex); watchers = WATCHERS_PTR; }
        if (!watchers) return;
        auto wit = watchers->find(mA);
        if (wit == watchers->end()) return;
        for (auto& w : wit->second) {
            if (res.usdNanos < static_cast<cpp_int>(w.thresholdNanos)) continue;
            if (w.chatId == SERVICE_CHAT_ID) continue;
            Lang lang = langFromCode(getUserLanguage(w.chatId));
            byLabelLang[{w.label, lang}].push_back(w.chatId);
        }
    }
    if (byLabelLang.empty()) return;

    bool anySent = false;
    for (auto& [labelLang, chatIds] : byLabelLang) {
        std::string msg = buildAlertMessage(labelLang.first, res, hash, labelLang.second);
        if (g_msgQueue.enqueueToRecipients(msg, chatIds)) anySent = true;
    }
    if (anySent) {
        g_stats.alerts_sent.fetch_add(byLabelLang.size());
        std::cout << "[OK] " << mA << " " << (res.isSwap?(res.isBuy?"BUY":"SELL"):"TRANSFER") << " "
                  << formatUsd(res.usdNanos) << " " << getSymbol(res.tokenAddr)
                  << " -> " << byLabelLang.size() << " label group(s)" << std::endl;
    } else std::cerr << "[WARN] Broadcast failed for " << hash << std::endl;
}

void bufferSwap(const std::string& mA, const TxResult& res, const std::string& hash,
                long long block, long long blockTs) {
    std::string key = mA + "|" + toLower(res.tokenAddr) + "|" + (res.isBuy ? "b" : "s");
    std::lock_guard<std::mutex> l(g_pendingMutex);
    auto it = g_pendingAlerts.find(key);
    if (it == g_pendingAlerts.end()) {
        g_pendingAlerts.emplace(key, PendingAlert{mA, res, hash, block, blockTs, blockTs});
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
        long long nowTs = static_cast<long long>(time(nullptr));
        for (auto it = g_pendingAlerts.begin(); it != g_pendingAlerts.end(); ) {
            long long age = nowTs - it->second.firstSeen;
            if (force || age >= AGGREGATION_WINDOW_SECONDS) {
                ready.push_back(std::move(it->second));
                it = g_pendingAlerts.erase(it);
            } else ++it;
        }
    }
    std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> watchers;
    { std::shared_lock l(watchersMutex); watchers = WATCHERS_PTR; }
    for (const PendingAlert& p : ready) {
        bool serviceWatched = false;
        if (watchers) {
            auto wit = watchers->find(p.wallet);
            if (wit != watchers->end())
                for (const auto& w : wit->second) if (w.chatId == SERVICE_CHAT_ID) { serviceWatched = true; break; }
        }
        if (serviceWatched) saveTrade(p.wallet, p.agg, p.hash, p.block, p.blockTs);
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

    struct Matched { const nlohmann::json* tx; std::string hash; std::string wallet; };
    std::vector<Matched> matched;
    for (auto& tx:block["transactions"]) {
        if (!running.load(std::memory_order_relaxed)) return false;
        if (!tx.is_object()||!tx.contains("hash")||!tx["hash"].is_string()) continue;
        std::string hash=tx["hash"].get<std::string>();
        g_stats.tx_processed.fetch_add(1);
        std::string from=tx.contains("from")&&tx["from"].is_string()?toLower(tx["from"].get<std::string>()):"";
        std::string to=(tx.contains("to")&&!tx["to"].is_null()&&tx["to"].is_string())?toLower(tx["to"].get<std::string>()):"";
        std::string mA;
        if (watchers->count(from)) mA=from; else if (watchers->count(to)) mA=to;
        if (mA.empty()) continue;
        if (isTxProcessed(hash)) continue;
        matched.push_back({&tx, hash, mA});
    }

    std::vector<nlohmann::json> receipts(matched.size());
    if (!matched.empty()) {
        const size_t CONCURRENCY = RPC_ENDPOINTS.size();
        const size_t spreadBase = rpcIndex.load(std::memory_order_relaxed);
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        size_t threads = std::min(CONCURRENCY, matched.size());
        for (size_t t = 0; t < threads; ++t) {
            pool.emplace_back([&]() {
                for (;;) {
                    size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= matched.size() || !running.load(std::memory_order_relaxed)) return;
                    receipts[i] = rpcSpread(spreadBase + i, "eth_getTransactionReceipt", {matched[i].hash});
                }
            });
        }
        for (auto& th : pool) th.join();
    }

    for (size_t i = 0; i < matched.size(); ++i) {
        if (!running.load(std::memory_order_relaxed)) return false;
        const auto& tx = *matched[i].tx;
        const std::string& hash = matched[i].hash;
        const std::string& mA = matched[i].wallet;
        const auto& receipt = receipts[i];
        if (receipt.is_null()) {
            std::cerr << "[RPC] receipt unavailable, will retry whole block: " << hash << std::endl;
            return false;
        }
        TxResult res=analyzeTx(tx,receipt,mA); if (!res.valid) { markTxProcessed(hash,bn); continue; }
        bool svcOnly = false;
        { auto cw = watchers->find(mA);
          if (cw != watchers->end() && !cw->second.empty()) {
              svcOnly = true;
              for (const auto& w : cw->second) if (w.chatId != SERVICE_CHAT_ID) { svcOnly = false; break; }
          } }
        recordCoverage(res, svcOnly);
        checkInvariants(hash, res);
        if (!res.isSwap && !res.unknownReason.empty()) logUnknownTx(hash, bn, tx, receipt, res);
        if (res.venue == "DEX interaction") { logBeneficiaries(hash, tx, res); recordBeneficiarySignal(tx, res); }
        if (res.isSwap && (res.venue.empty() || res.venue == "DEX Pool" || res.venue == "DEX" || res.venue == "Universal Router")) logLowConfidenceTx(hash, bn, tx, receipt, res);

        if (res.tokenAddr.empty()) { markTxProcessed(hash,bn); continue; }
        if (isBaseAsset(res.tokenAddr) && !res.isSwap) { markTxProcessed(hash,bn); continue; }

        auto wit = watchers->find(mA);
        if (wit == watchers->end()) { markTxProcessed(hash,bn); continue; }

        rememberTokensFromReceipt(mA, receipt, blockTs);
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

std::mutex g_lastViewMutex;
// Идёт ли сейчас возврат назад. navigateBack переигрывает предыдущий адрес
// через handleCallbackQuery, а тот на каждом экране зовёт rememberView - и
// снятый экран немедленно возвращался в стек. Флаг на время возврата это
// подавляет. Локальный для потока: обработчики разных чатов идут параллельно.
thread_local bool g_navigatingBack = false;
std::unordered_map<std::string, std::vector<std::string>> g_viewStack;
constexpr size_t VIEW_STACK_MAX = 12;

void rememberView(const std::string& chatId, const std::string& data) {
    if (g_navigatingBack) return;
    std::lock_guard<std::mutex> l(g_lastViewMutex);
    auto& st = g_viewStack[chatId];
    if (!st.empty() && st.back() == data) return;
    st.push_back(data);
    if (st.size() > VIEW_STACK_MAX) st.erase(st.begin());
}

// Начать историю заново. /start - это корень: всё, что было до него, к новой
// навигации отношения не имеет.
void resetViewStack(const std::string& chatId, const std::string& root) {
    std::lock_guard<std::mutex> l(g_lastViewMutex);
    g_viewStack[chatId] = { root };
}

std::string getLastView(const std::string& chatId) {
    std::lock_guard<std::mutex> l(g_lastViewMutex);
    auto it = g_viewStack.find(chatId);
    return (it == g_viewStack.end() || it->second.empty()) ? "" : it->second.back();
}

std::string popPreviousView(const std::string& chatId) {
    std::lock_guard<std::mutex> l(g_lastViewMutex);
    auto it = g_viewStack.find(chatId);
    if (it == g_viewStack.end() || it->second.size() < 2) return "";
    it->second.pop_back();
    return it->second.back();
}

void handleCallbackQuery(const json& callbackQuery);

TelegramUI::UIMessage renderViewByData(const std::string& chatId, const std::string& data) {
    size_t colonPos = data.find(':');
    std::string action = colonPos != std::string::npos ? data.substr(0, colonPos) : data;
    std::string param = colonPos != std::string::npos ? data.substr(colonPos + 1) : "";

    if (action == "menu") {
        if (param == "my_wallets") return TelegramUI::buildWalletsList(chatId);
        if (param == "alert_threshold") return TelegramUI::buildAlertThresholdMenu(getUserThresholdNanos(chatId), langFromCode(getUserLanguage(chatId)));
        if (param == "toptrader") { auto r = buildVenueMenu(chatId); return {r.text, r.keyboard}; }
        if (param == "toptrader_spot") { auto r = buildGlobalTopMenu(chatId); return {r.text, r.keyboard}; }
        if (param == "premium") { auto r = buildPremiumPage(chatId); return {r.text, r.keyboard}; }
        if (param == "languages") return TelegramUI::buildLanguagesMenu(chatId);
        if (param == "help") return TelegramUI::buildHelpMessage(chatId);
        return TelegramUI::buildMainMenu(chatId);
    }
    {
        HlMessage hl;
        if (renderHyperliquidView(chatId, action, param, hl)) return {hl.text, hl.keyboard};
    }
    if (action == "mw_page") {
        int page = 1;
        try { page = std::stoi(param); } catch (...) {}
        return TelegramUI::buildWalletsList(chatId, page);
    }
    if (action == "tt_page") {
        int page = 1;
        try { page = std::stoi(param); } catch (...) {}
        auto r = buildTopPnlPage(chatId, page);
        return {r.text, r.keyboard};
    }
    if (action == "gt_open") {
        GlobalRankKind kind;
        if (parseGlobalRankKind(param, kind)) {
            auto r = buildGlobalTopMessage(chatId, kind, premiumTopTradersLimit(chatId), !isPremium(chatId));
            return {r.text, r.keyboard};
        }
    }
    if (action == "gt_page") {
        size_t sep = param.find(':');
        if (sep != std::string::npos) {
            GlobalRankKind kind;
            if (parseGlobalRankKind(param.substr(0, sep), kind)) {
                int page = 1;
                try { page = std::stoi(param.substr(sep + 1)); } catch (...) {}
                auto r = buildGlobalTopPage(chatId, kind, page, premiumTopTradersLimit(chatId), !isPremium(chatId));
                return {r.text, r.keyboard};
            }
        }
    }
    return TelegramUI::buildMainMenu(chatId);
}

bool navigateBack(const std::string& chatId, long long messageId) {
    std::string back = popPreviousView(chatId);
    if (back.empty()) return false;
    json synthetic;
    synthetic["data"] = back;
    synthetic["from"] = json::object();
    synthetic["from"]["id"] = std::stoll(chatId);
    synthetic["message"] = json::object();
    synthetic["message"]["message_id"] = messageId;
    g_navigatingBack = true;
    handleCallbackQuery(synthetic);
    g_navigatingBack = false;
    return true;
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

    if (action != "tt_track" && action != "remove" && !callbackQueryId.empty()) {
        answerCallbackQuery(callbackQueryId);
    }

    if (action == "menu") {
        g_sessionManager.clearSession(chatId);

        if (param == "main") {
            rememberView(chatId, data);
            auto msg = TelegramUI::buildMainMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "add_wallet") {
            rememberView(chatId, data);
            startAddWalletFlow(chatId, messageId);
        }
        else if (param == "account") {
            rememberView(chatId, data);
            auto msg = TelegramUI::buildAccountMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "hold") {
            rememberView(chatId, data);
            Lang lang = langFromCode(getUserLanguage(chatId));
            g_sessionManager.setState(chatId, UserState::AWAITING_HOLD_ADDRESS);
            replyInPlace(chatId, messageId, tr(lang, "hold_prompt"),
                         TelegramUI::buildCancelWithSpotTop(lang));
        }
        else if (param == "my_wallets") {
            rememberView(chatId, data);
            auto msg = TelegramUI::buildWalletsList(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "alert_threshold") {
            rememberView(chatId, data);
            uint64_t threshold = getUserThresholdNanos(chatId);
            auto msg = TelegramUI::buildAlertThresholdMenu(threshold, langFromCode(getUserLanguage(chatId)));
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "toptrader") {
            rememberView(chatId, data);
            auto msg = buildVenueMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "toptrader_spot") {
            rememberView(chatId, data);
            auto msg = buildGlobalTopMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "premium") {
            rememberView(chatId, data);
            auto msg = buildPremiumPage(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "settings") {
            rememberView(chatId, data);
            auto msg = TelegramUI::buildMainMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "languages") {
            rememberView(chatId, data);
            auto msg = TelegramUI::buildLanguagesMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
        else if (param == "help") {
            rememberView(chatId, data);
            auto msg = TelegramUI::buildHelpMessage(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
    }
    else if (action == "back") {
        g_sessionManager.clearSession(chatId);
        if (!navigateBack(chatId, messageId)) {
            auto msg = TelegramUI::buildMainMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
    }
    else if (action == "cancel") {
        g_sessionManager.clearSession(chatId);
        if (!navigateBack(chatId, messageId)) {
            Lang lang = langFromCode(getUserLanguage(chatId));
            auto msg = TelegramUI::buildMainMenu(chatId);
            replyInPlace(chatId, messageId, tr(lang, "op_cancelled") + "\n\n" + msg.text, msg.keyboard);
        }
    }
    else if (action == "lang") {
        static const std::set<std::string> SUPPORTED_LANGS = {"en", "ru", "es", "pt", "fr", "tr", "ar"};
        if (SUPPORTED_LANGS.count(param)) {
            setUserLanguage(chatId, param);
            rememberView(chatId, "menu:languages");
            auto msg = TelegramUI::buildLanguagesMenu(chatId);
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        }
    }
    else if (action == "premium_buy") {

        if (!sendPremiumInvoice(chatId)) {
            Lang lang = langFromCode(getUserLanguage(chatId));
            auto page = buildPremiumPage(chatId);
            replyInPlace(chatId, messageId,
                tr(lang, "err_invoice_failed") + "\n\n" + page.text, page.keyboard);
        }
    }
    else if (action == "mw_page" || action == "rename" ||
             action == "askremove" || action == "remove") {
        handleWalletCallback(chatId, action, param, data, messageId, callbackQueryId);
    }
    else if (action == "threshold") {
        rememberView(chatId, "menu:alert_threshold");
        handleThresholdCallback(chatId, param, messageId);
    }
    else if (action == "tt_page" || action == "tt_track" || action == "tt_noop" ||
             action == "gt_open" || action == "gt_page" || action == "gt_token") {
        handleRankingCallback(chatId, action, param, data, messageId, callbackQueryId);
    }
    else if (action == "hl_menu" || action == "hl_open" || action == "hl_page" ||
             action == "hl_positions" || action == "hl_pos") {
        handleHyperliquidCallback(chatId, action, param, data, messageId, callbackQueryId);
    }
}

bool handleTextInput(const std::string& chatId, const std::string& text) {
    UserSession session = g_sessionManager.getSession(chatId);

    if (session.state == UserState::IDLE) {
        return false;
    }

    if (handleWalletText(chatId, text, session)) return true;

    if (session.state == UserState::AWAITING_CUSTOM_THRESHOLD)
        return handleThresholdText(chatId, text);

    if (session.state == UserState::AWAITING_TOPTRADER_TOKEN) {
        std::string tokenArg = trim(text);
        Lang lang = langFromCode(getUserLanguage(chatId));

        if (tokenArg.empty()) {
            sendMsg(chatId, tr(lang, "token_search_empty"),
                    TelegramUI::buildCancelButton(lang));
            return true;
        }

        RankingMessage result = buildTopPnlMessage(chatId, tokenArg, 1);
        g_sessionManager.clearSession(chatId);
        sendMsg(chatId, result.text, result.keyboard);
        return true;
    }

    if (session.state == UserState::AWAITING_HOLD_ADDRESS) {
        const std::string addr = toLower(trim(text));
        Lang lang = langFromCode(getUserLanguage(chatId));

        if (!isValidAddress(addr)) {
            sendMsg(chatId, tr(lang, "hold_bad_address"),
                    TelegramUI::buildCancelWithSpotTop(lang));
            return true;
        }

        g_sessionManager.clearSession(chatId);
        sendMsg(chatId, tr(lang, "hold_loading"));
        auto msg = TelegramUI::buildHoldCard(chatId, addr);
        sendMsg(chatId, msg.text, msg.keyboard);
        return true;
    }

    return false;
}

void dbMaintenanceLoop() {
    auto lastTruncate = std::chrono::steady_clock::now();
    while (running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::minutes(1));
        try {
            bool doTruncate = std::chrono::duration_cast<std::chrono::minutes>(
                std::chrono::steady_clock::now() - lastTruncate).count() >= 30;
            walCheckpoint(doTruncate ? SQLITE_CHECKPOINT_TRUNCATE : SQLITE_CHECKPOINT_PASSIVE);
            if (doTruncate) {
                cleanupOldTx(g_lastProcessedBlock.load(std::memory_order_relaxed));
                cleanupPriceHistory();
                lastTruncate = std::chrono::steady_clock::now();
            }
        } catch (const std::exception& e) { std::cerr << "[DB-MAINT][ERROR] " << e.what() << std::endl; }
    }
}

void alertFlushLoop() {
    while (running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        try { flushPendingAlerts(false); }
        catch (const std::exception& e) { std::cerr << "[ALERT-FLUSH][ERROR] " << e.what() << std::endl; }
    }
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
                            sendMsg(cid, tr(langFromCode(getUserLanguage(cid)), "err_user_limit"));
                        } else {
                            ensureUser(cid);
                            // /start - корень навигации, история начинается заново.
                            // Без этого стек оставался пустым, и "Назад" со второго
                            // экрана уводило в главное меню вместо шага назад.
                            // Пробный период выдаётся один раз на аккаунт: отметка
                            // о выдаче переживает удаление пользователя и повторный
                            // /start, поэтому вернуться за вторым нельзя.
                            if (!trialAlreadyGranted(cid)) {
                                if (grantPremiumDays(cid, TRIAL_DAYS)) {
                                    markTrialGranted(cid);
                                    Lang tl = langFromCode(getUserLanguage(cid));
                                    sendMsg(cid, tr(tl, "trial_granted"));
                                }
                            }
                            resetViewStack(cid, "menu:main");
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
                                << "⚙️ RPC fail: " << g_stats.rpc_failures.load() << "\n💰 Price fb: " << g_stats.price_fallbacks.load() << "\n🔄 REORG: " << g_stats.reorg_verifications.load() << "\n📨 Sent: " << g_stats.alerts_sent.load() << "\n🔍 TX: " << g_stats.tx_processed.load()
                                << "\n⏳ Lag: " << g_stats.current_lag.load() << " blocks (max: " << g_stats.max_lag_seen.load() << ")";
                            ss2 << rpcSlowSummary();
                            {
                                auto renderCov = [](std::stringstream& out, const char* title, CoverageSet& c) {
                                    uint64_t buy=c.buy.load(), sell=c.sell.load(), lpAdd=c.lp_add.load(), lpRemove=c.lp_remove.load(),
                                             wrap=c.wrap.load(), unwrap=c.unwrap.load(), xfer=c.transfer.load(),
                                             inter=c.interaction.load(), arb=c.arbitrage.load(), unk=c.unknown.load();
                                    uint64_t total = buy+sell+lpAdd+lpRemove+wrap+unwrap+xfer+inter+arb+unk;
                                    out << "\n\n" << title << " (valid tx: " << total << ")\n"
                                        << "🟢 BUY: " << buy << "\n🚨 SELL: " << sell
                                        << "\n🌊 LP Add: " << lpAdd << "\n🌊 LP Remove: " << lpRemove
                                        << "\n🔄 Wrap: " << wrap << "\n🔄 Unwrap: " << unwrap
                                        << "\n📤 Transfer: " << xfer << "\n🤝 Interaction: " << inter
                                        << "\n♻️ Arbitrage: " << arb << "\n❓ Unknown: " << unk;
                                };
                                renderCov(ss2, "📈 <b>Coverage — users</b>", g_covUser);
                                renderCov(ss2, "🤖 <b>Coverage — service</b>", g_covSvc);
                                ss2 << "\n\n🔬 <b>Signals</b>\n💱 Swap Event: " << g_stats.sig_swap_event.load()
                                    << "\n🌐 Universal Router: " << g_stats.sig_universal_router.load()
                                    << "\n📦 Multicall: " << g_stats.sig_multicall.load()
                                    << "\n🔑 Permit2: " << g_stats.sig_permit2.load()
                                    << "\n\n🌊 <b>LP signals seen</b> (regardless of outcome)\n"
                                    << "ERC20 mint/burn: " << g_stats.sig_lp_mint_burn.load()
                                    << "\nPool-identity: " << g_stats.sig_lp_pool_identity.load()
                                    << "\nV3 events: " << g_stats.sig_lp_v3_event.load()
                                    << "\n\n❓ <b>Unknown reasons</b>\n"
                                    << "Unconfirmed opposite: " << g_stats.unk_unconfirmed_opposite.load()
                                    << "\nLP not linked: " << g_stats.unk_lp_not_linked.load()
                                    << "\nOther: " << g_stats.unk_other.load()
                                    << "\n\n\xF0\x9F\xA9\xBA <b>Diagnostics</b>\n"
                                    << "Swap w/o wallet flow: " << g_stats.unk_swap_no_wallet_flow.load()
                                    << "\nOnly base flow: " << g_stats.unk_only_base_flow.load()
                                    << "\nSwap inferred from flow: " << g_stats.diag_swap_inferred.load()
                                    << "\nNative counter needs trace: " << g_stats.diag_native_counter.load()
                                    << "\nNative from router unwrap: " << g_stats.diag_native_unwrap.load()
                                    << "\nNative refund adjusted: " << g_stats.diag_native_refund.load()
                                    << "\nVault flow attributed (Bot Trade): " << g_stats.diag_vault_flow_attributed.load();
                            }
                            ss2 << hyperliquidStatsLine();
                            if (qs>1000) ss2 << "\n\n⚠️ <b>QUEUE HIGH!</b>"; if (fc>0) ss2 << "\n⚠️ <b>FAILED DELIVERIES!</b>";
                            sendMsg(cid,ss2.str());
                        }
                    }
                    else if (txt.rfind("/import", 0) == 0) {
                        if (cid != OWNER_CHAT_ID) {
                            sendMsg(cid, "Access denied.");
                        } else {
                            std::vector<std::string> found;
                            {
                                std::string s = toLower(txt);
                                size_t p = 0;
                                while ((p = s.find("0x", p)) != std::string::npos) {
                                    if (p + 42 <= s.size()) {
                                        std::string cand = s.substr(p, 42);
                                        if (isValidAddress(cand)) { found.push_back(cand); p += 42; continue; }
                                    }
                                    p += 2;
                                }
                            }
                            if (found.empty()) {
                                sendMsg(cid, "Использование: /import 0x... 0x... (адреса через пробел, запятую или с новой строки)");
                            } else {
                                int added = 0, dup = 0, banned = 0, failed = 0;
                                for (const auto& a : found) {
                                    switch (addUserWhale(SERVICE_CHAT_ID, a, a)) {
                                        case AddWhaleResult::OK:                  ++added;  break;
                                        case AddWhaleResult::ALREADY_EXISTS:      ++dup;    break;
                                        case AddWhaleResult::PERMANENTLY_BANNED:  ++banned; break;
                                        default:                                  ++failed; break;
                                    }
                                }
                                refreshWatchers();
                                std::stringstream rep;
                                rep << "\U0001F4E5 <b>Импорт завершён</b>\n\n"
                                    << "Найдено адресов: <b>" << found.size() << "</b>\n"
                                    << "✅ Добавлено: <b>" << added << "</b>\n"
                                    << "↩️ Уже отслеживались: <b>" << dup << "</b>\n";
                                if (banned > 0) rep << "🤖 Помечены как боты (пропущены): <b>" << banned << "</b>\n";
                                if (failed > 0) rep << "⚠️ Не удалось добавить: <b>" << failed << "</b>\n";
                                rep << "\nВсего на сервисном аккаунте: <b>"
                                    << countUserWhales(SERVICE_CHAT_ID) << "</b>";
                                sendMsg(cid, rep.str());
                            }
                        }
                    }
                    else if (txt.rfind("/unban", 0) == 0) {
                        if (cid != OWNER_CHAT_ID) {
                            sendMsg(cid, "Access denied.");
                        } else {
                            std::string arg = trim(txt.substr(6));
                            if (!isValidAddress(arg)) {
                                sendMsg(cid, "Использование: /unban 0x&lt;адрес&gt;");
                            } else if (liftPermanentBan(arg)) {
                                sendMsg(cid, "✅ Бан снят: <code>" + toLower(arg) + "</code>\nКошелёк снова может попадать в рейтинг.");
                            } else {
                                sendMsg(cid, "ℹ️ У этого адреса нет пожизненного бана: <code>" + toLower(arg) + "</code>");
                            }
                        }
                    }
                    else if (handleBeneficiaryCommand(cid, txt)) {
                    }
                    else {
                        sendMsg(cid, tr(langFromCode(getUserLanguage(cid)), "unknown_command"));
                        resetViewStack(cid, "menu:main");
                        auto msg = TelegramUI::buildMainMenu(cid);
                        sendMsg(cid, msg.text, msg.keyboard);
                    }
                }
                else if (handleTextInput(cid, txt)) {
                }
                else {
                    resetViewStack(cid, "menu:main");
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
        ChainContext cfg;
        if (!chainConfigByName(chainName, cfg)) {
            std::cerr << "[FATAL] Unknown WHALE_CHAIN: " << chainName << std::endl; return 1;
        }
        setChainContext(cfg);
        setRpcEndpoints(cfg.rpcEndpoints);
        std::cout << "[CHAIN] Running on " << chainName << " (native: " << chainCtx().nativeSymbol
                  << ", nodes: " << cfg.rpcEndpoints.size() << ")" << std::endl;
    }
    setRpcFailureHandler([]{
        g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed);
        g_stats.last_rpc_failure.store(time(nullptr), std::memory_order_relaxed);
    });
    initDB(); initRankingDB(); seedWalletTokensFromTrades();
    if (!initPremium(TG_TOKEN, SERVICE_CHAT_ID)) {
        std::cerr << "[STARTUP][FATAL] Premium schema init failed — payments are DISABLED for this run" << std::endl;
    }
    loadTokenCache();
    ensureUser(OWNER_CHAT_ID);
    refreshWatchers();
    if (!initHyperliquid()) {
        std::cerr << "[STARTUP] Hyperliquid init failed - perps DISABLED for this run" << std::endl;
    } else {
        startHyperliquidLoop();
    }
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
    g_msgQueue.start(); std::thread tg(telegramLoop); std::thread rk(rankingCacheLoop); std::thread af(alertFlushLoop); std::thread dm(dbMaintenanceLoop);
    auto lst=std::chrono::steady_clock::now(), lsq=std::chrono::steady_clock::now(), lcl=std::chrono::steady_clock::now();
    while (running.load(std::memory_order_relaxed)) {
        try {
            auto lj=rpc("eth_blockNumber",{}); long long lat;
            if (!lj.is_string()||!hexToLL(lj.get<std::string>(),lat)) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
            {
                int64_t lagNow = lat - lb;
                g_stats.current_lag.store(lagNow, std::memory_order_relaxed);
                int64_t prevMax = g_stats.max_lag_seen.load(std::memory_order_relaxed);
                if (lagNow > prevMax) g_stats.max_lag_seen.store(lagNow, std::memory_order_relaxed);
            }
            while (lb<lat&&running.load(std::memory_order_relaxed)) {
                long long next = lb+1;
                if (!processBlock(next)) {
                    lb = getLastBlock();
                    break;
                }
                lb = next; saveLastBlock(lb);
                g_lastProcessedBlock.store(lb, std::memory_order_relaxed);
            }
            flushPendingAlerts(false);
            if (std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now()-lsq).count()>=5) {
                g_msgQueue.syncSize();
                lsq=std::chrono::steady_clock::now();
            }
            if (std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now()-lcl).count()>=30) { cleanupOldAlerts(); cleanupOldTrades(); cleanupExpiredPremium(); lcl=std::chrono::steady_clock::now(); }
            if (std::chrono::duration_cast<std::chrono::hours>(std::chrono::steady_clock::now()-lst).count()>=1) {
                std::cout << "[STATS] rpc_fail=" << g_stats.rpc_failures.load() << " price_fb=" << g_stats.price_fallbacks.load()
                    << " reorg=" << g_stats.reorg_verifications.load() << " tx=" << g_stats.tx_processed.load() << " sent=" << g_stats.alerts_sent.load()
                    << " queue=" << g_msgQueue.size() << " uptime=" << getUptime() << std::endl; lst=std::chrono::steady_clock::now(); }
        } catch (const std::exception& e) { std::cerr << "[ERROR] " << e.what() << std::endl; }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::cout << "[SHUTDOWN] Stopping..." << std::endl;
    flushPendingAlerts(true);
    g_msgQueue.stop();
    tg.join();
    rk.join();
    af.join();
    dm.join();
    stopHyperliquid();
    walCheckpoint();
    closeRankingDB();
    if (db) sqlite3_close(db);
    curl_global_cleanup();
    std::cout << "[SHUTDOWN] Clean exit." << std::endl; return 0;
}
