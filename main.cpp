#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <cmath>
#include <cctype>
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
#include "token_prices.h"
#include "telegram.h"
#include "rpc_client.h"
#include "chains.h"
#include "wallets.h"
#include "ru.h"
#include "premium.h"
#include "message_queue.h"
#include "tx_analyzer.h"
#include "beneficiary_stats.h"
#include "btc_chain.h"
#include "hyperliquid.h"
#include "hyperliquid_internal.h"
#include "ws_heads.h"

using json = nlohmann::json;
using boost::multiprecision::cpp_int;

struct Stats {
    std::atomic<uint64_t> rpc_failures{0};
    std::atomic<uint64_t> rpc_giveups{0};
    std::atomic<uint64_t> price_fallbacks{0};
    std::atomic<uint64_t> price_thin_pool{0};
    std::atomic<uint64_t> price_from_pool{0};
    std::atomic<uint64_t> price_from_dex{0};
    std::atomic<uint64_t> price_from_cg{0};
    std::atomic<uint64_t> price_cache_hit{0};
    std::atomic<uint64_t> price_divergence{0};
    std::atomic<uint64_t> price_spike_reject{0};
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

constexpr long long WALLET_TOKEN_TTL_SEC = 60LL * 86400LL;
const std::string OWNER_CHAT_ID = "546348566";
const std::string SERVICE_CHAT_ID = "7479880531";
const std::string DB_FILE = "whale_bot.db";

const long long FAST_SYNC_LAG = 1000;
const long long REORG_ROLLBACK = 5;
const long long TX_TTL_BLOCKS = 6700;

std::atomic<bool> running{true};
std::atomic<int64_t> g_lastProcessedBlock{0};
void signalHandler(int) { running.store(false, std::memory_order_relaxed); }

std::mutex dbMutex, cacheMutex;
sqlite3* db = nullptr;

struct Watcher {
    std::string chatId;
    std::string label;
    uint64_t thresholdNanos;
};
std::shared_mutex watchersMutex;
std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> WATCHERS_PTR =
    std::make_shared<const std::unordered_map<std::string, std::vector<Watcher>>>();
std::shared_ptr<const std::unordered_set<std::string>> BSC_ACTIVE_PTR =
    std::make_shared<const std::unordered_set<std::string>>();
std::shared_ptr<const std::unordered_set<std::string>> HL_ACTIVE_PTR =
    std::make_shared<const std::unordered_set<std::string>>();

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
            hi_nanos INTEGER NOT NULL DEFAULT 0, lo_nanos INTEGER NOT NULL DEFAULT 0,
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
        CREATE INDEX IF NOT EXISTS idx_deliveries_terminal ON deliveries(status, alert_id) WHERE status IN (1,2,4);        CREATE TABLE IF NOT EXISTS pair_cache (
            token TEXT PRIMARY KEY,
            val TEXT NOT NULL
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
    /* Куда слать алерты и что человек уже видел — выбирается в мини-аппе.
       alert_tg=0 — «только в приложении»: в чат алерты не идут, лежат в
       истории. alerts_seen_at — когда человек последний раз открыл историю;
       всё новее считается непрочитанным. API добавляет те же колонки сам,
       если бот ещё не обновлён, — поэтому ошибка «уже есть» здесь норма. */
    for (const char* sql : {
            "ALTER TABLE users ADD COLUMN alert_tg INTEGER NOT NULL DEFAULT 1",
            "ALTER TABLE users ADD COLUMN alerts_seen_at INTEGER NOT NULL DEFAULT 0"}) {
        char* mErr = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &mErr) == SQLITE_OK)
            std::cout << "[STARTUP] " << sql << std::endl;
        if (mErr) sqlite3_free(mErr);
    }
    /* Максимум и минимум цены внутри часа. Раньше от часа оставалась одна
       точка — первая, — хотя бот опрашивал цену по многу раз за час и просто
       выбрасывал остальное. По одной точке не видно, что цена задевала стоп
       и вернулась: сигнал считался «никуда не пошёл», а на деле его выбило.
       У старых строк здесь нули, и тогда в ход идёт та самая единственная
       цена — как было. */
    for (const char* mig : {"ALTER TABLE token_price_history ADD COLUMN hi_nanos INTEGER NOT NULL DEFAULT 0",
                            "ALTER TABLE token_price_history ADD COLUMN lo_nanos INTEGER NOT NULL DEFAULT 0"}) {
        char* mErr = nullptr;
        if (sqlite3_exec(db, mig, nullptr, nullptr, &mErr) == SQLITE_OK)
            std::cout << "[STARTUP] token_price_history: added hi/lo column" << std::endl;
        if (mErr) sqlite3_free(mErr);
    }
    {
        char* mErr = nullptr;
        if (sqlite3_exec(db, "ALTER TABLE user_whales ADD COLUMN is_primary INTEGER NOT NULL DEFAULT 0",
                         nullptr, nullptr, &mErr) == SQLITE_OK)
            std::cout << "[STARTUP] user_whales: added is_primary column" << std::endl;
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

    // Cortex (сигналы и обучаемая модель) убран из проекта. Его таблицы
    // больше никто не пишет и не чистит — удаляем их, чтобы журнал сигналов
    // и свечи не лежали в базе без срока. На чистой базе это пустая операция.
    {
        const char* dropSql =
            "DROP TABLE IF EXISTS ai_signals; DROP TABLE IF EXISTS ai_signal_log;"
            "DROP TABLE IF EXISTS ai_events; DROP TABLE IF EXISTS ai_weights;"
            "DROP TABLE IF EXISTS ai_access; DROP TABLE IF EXISTS ai_models;"
            "DROP TABLE IF EXISTS ai_model_try; DROP TABLE IF EXISTS ai_coin_seen;"
            "DROP TABLE IF EXISTS hl_candles;";
        char* derr = nullptr;
        if (sqlite3_exec(db, dropSql, nullptr, nullptr, &derr) != SQLITE_OK) {
            std::cerr << "[STARTUP] Cortex tables cleanup failed: " << (derr ? derr : "") << std::endl;
            sqlite3_free(derr);
        }
    }

    // Дайджест мини-аппа: выпуски пишет API, лайки и комментарии — люди.
    // Таблицы заводятся и здесь, у хозяина базы, — те же CREATE, что в API:
    // кто бы из двоих ни поднялся первым, схема одна.
    {
        const char* digestSql = R"(
            CREATE TABLE IF NOT EXISTS digests (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                day TEXT NOT NULL UNIQUE,
                made_at INTEGER NOT NULL,
                body TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS digest_likes (
                digest_id INTEGER NOT NULL,
                chat_id TEXT NOT NULL,
                at INTEGER NOT NULL,
                PRIMARY KEY (digest_id, chat_id)
            );
            CREATE TABLE IF NOT EXISTS digest_comments (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                digest_id INTEGER NOT NULL,
                chat_id TEXT NOT NULL,
                name TEXT NOT NULL,
                text TEXT NOT NULL,
                at INTEGER NOT NULL,
                anon INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS digest_tr (
                comment_id INTEGER NOT NULL,
                lang TEXT NOT NULL,
                text TEXT NOT NULL,
                src TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (comment_id, lang)
            );
            CREATE TABLE IF NOT EXISTS digest_mute (
                chat_id TEXT PRIMARY KEY,
                at INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_digest_comments_d ON digest_comments(digest_id, at);
            CREATE INDEX IF NOT EXISTS idx_digest_comments_c ON digest_comments(chat_id);
            CREATE INDEX IF NOT EXISTS idx_digest_likes_c ON digest_likes(chat_id);
        )";
        char* gerr = nullptr;
        if (sqlite3_exec(db, digestSql, nullptr, nullptr, &gerr) != SQLITE_OK) {
            std::cerr << "[STARTUP] digest schema failed: " << (gerr ? gerr : "") << std::endl;
            sqlite3_free(gerr);
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

void ensureUser(const std::string& chatId, const std::string& tgLangCode) {
    std::string lang = "en";
    if (!tgLangCode.empty()) lang = langCodeOf(langFromCode(tgLangCode));

    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR IGNORE INTO users(chat_id,language,threshold_nanos,created_at) VALUES(?,?,?,?)")) return;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,lang.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(s,3,static_cast<sqlite3_int64>(DEFAULT_THRESHOLD_NANOS));
    sqlite3_bind_int64(s,4,time(nullptr));
    sqlite3_step(s); sqlite3_finalize(s);
}

size_t countUsers() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT COUNT(*) FROM users")) return 0;
    size_t n=0; if (sqlite3_step(s)==SQLITE_ROW) n=sqlite3_column_int64(s,0); sqlite3_finalize(s); return n;
}
/**
 * Короткий слепок всего, от чего зависит таблица наблюдателей.
 *
 * Мини-апп пишет в ту же базу своим процессом: добавил кошелёк, сменил
 * порог, удалил себя по /api/forget. Бот об этом не узнаёт — таблица
 * наблюдателей перестраивается только по явному вызову внутри самого бота.
 * До этой проверки правки из приложения вступали в силу лишь тогда, когда
 * кто-то другой случайно дёргал перестройку, а удалённый пользователь всё
 * это время продолжал получать алерты.
 *
 * Две суммирующие выборки по маленьким таблицам раз в минуту дешевле, чем
 * безусловная перестройка, и ловят любое изменение: число строк, порог,
 * премиум, набор кошельков.
 */
std::string watchersFingerprint() {
    std::string out;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (prepareOrLog(db, &s,
        "SELECT COUNT(*), COALESCE(SUM(threshold_nanos),0), COALESCE(SUM(is_premium),0), "
        "COALESCE(SUM(premium_expire),0) FROM users")) {
        if (sqlite3_step(s) == SQLITE_ROW)
            for (int i = 0; i < 4; i++)
                out += std::to_string(sqlite3_column_int64(s, i)) + ":";
        sqlite3_finalize(s);
    }
    if (prepareOrLog(db, &s,
        "SELECT COUNT(*), COALESCE(SUM(whale_id),0), COALESCE(MAX(created_at),0) FROM user_whales")) {
        if (sqlite3_step(s) == SQLITE_ROW)
            for (int i = 0; i < 3; i++)
                out += std::to_string(sqlite3_column_int64(s, i)) + ":";
        sqlite3_finalize(s);
    }
    return out;
}

void refreshWatchers() {
    auto m = std::make_shared<std::unordered_map<std::string, std::vector<Watcher>>>();
    auto bscActive = std::make_shared<std::unordered_set<std::string>>();
    auto hlActive = std::make_shared<std::unordered_set<std::string>>();
    long long now = static_cast<long long>(time(nullptr));
    constexpr long long MARKET_WATCH_GRACE_SEC = 30LL * 86400LL;
    const long long graceAfter = now - MARKET_WATCH_GRACE_SEC;
    bool queryOk = false;
    size_t bscOff = 0, hlOff = 0;

    std::unordered_set<std::string> hasHlFill;
    {
        std::lock_guard<std::mutex> hlLock(hl::g_hlDbMutex);
        if (hl::g_hlDb) {
            sqlite3_stmt* hs = nullptr;
            if (prepareOrLog(hl::g_hlDb, &hs, "SELECT DISTINCT lower(wallet) FROM hl_fills")) {
                while (sqlite3_step(hs) == SQLITE_ROW) {
                    std::string w = safeColumnText(hs, 0);
                    if (!w.empty()) hasHlFill.insert(std::move(w));
                }
                sqlite3_finalize(hs);
            }
        }
    }

    {
        std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;

        if (prepareOrLog(db,&s,
            "SELECT wa.address, uw.user_id, uw.label, u.threshold_nanos, "
            "       CASE WHEN u.is_premium=1 AND u.premium_expire>? THEN 1 ELSE 0 END, "
            "       uw.created_at, "
            "       EXISTS(SELECT 1 FROM trades t WHERE t.wallet = lower(wa.address) LIMIT 1) "
            "FROM user_whales uw "
            "JOIN whale_addresses wa ON wa.id = uw.whale_id "
            "JOIN users u ON u.chat_id = uw.user_id "
            "ORDER BY uw.user_id ASC, uw.is_primary DESC, uw.created_at ASC, uw.rowid ASC")) {
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
                long long createdAt = sqlite3_column_int64(s,5);
                bool hasSpot = sqlite3_column_int(s,6) != 0;
                const bool inGrace = (createdAt <= 0) || (createdAt >= graceAfter);
                const bool hasHl = hasHlFill.count(addr) > 0;

                if (uid != prevUser) { prevUser = uid; loadedForUser = 0; }
                if (!prem && uid != SERVICE_CHAT_ID && loadedForUser >= 1) continue;
                (*m)[addr].push_back(Watcher{uid,label,nanos});
                loadedForUser++;
                // Адрес биткоина: алерты по нему шлёт сканер BTC, а BSC и
                // Hyperliquid искать его у себя незачем — Hyperliquid
                // подписался бы на поток несуществующего счёта.
                if (addr.rfind("0x", 0) != 0) continue;

                if (hasSpot || inGrace) {
                    bscActive->insert(addr);
                } else {
                    ++bscOff;
                }
                if (hasHl || inGrace) {
                    hlActive->insert(addr);
                } else {
                    ++hlOff;
                }
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
    std::cout << "[WATCHERS] total=" << m->size()
              << " bsc_active=" << bscActive->size()
              << " hl_active=" << hlActive->size()
              << " bsc_cold_links=" << bscOff
              << " hl_cold_links=" << hlOff << std::endl;
    {
        std::unordered_set<std::string> btc;
        for (const auto& [addr, ws] : *m) if (addr.rfind("0x", 0) != 0) btc.insert(addr);
        btcSetFollowed(std::move(btc));
    }
    std::unique_lock l(watchersMutex);
    WATCHERS_PTR = m;
    BSC_ACTIVE_PTR = bscActive;
    HL_ACTIVE_PTR = hlActive;
}

std::vector<std::string> hlWatchedAddresses() {
    std::shared_ptr<const std::unordered_set<std::string>> hlActive;
    { std::shared_lock l(watchersMutex); hlActive = HL_ACTIVE_PTR; }
    std::vector<std::string> out;
    if (!hlActive) return out;
    out.reserve(hlActive->size());
    for (const auto& a : *hlActive) out.push_back(a);
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

size_t hlAlertRecipientCount() {
    std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> snapshot;
    { std::shared_lock l(watchersMutex); snapshot = WATCHERS_PTR; }
    if (!snapshot) return 0;

    std::set<std::string> uniq;
    for (const auto& kv : *snapshot)
        for (const Watcher& w : kv.second)
            if (w.chatId != SERVICE_CHAT_ID) uniq.insert(w.chatId);

    size_t n = 0;
    for (const std::string& c : uniq) if (isPremium(c)) n++;
    return n;
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

/* Бесплатную неделю премиума бот больше не выдаёт: её выдаёт API при первом
   открытии мини-аппа (grant_trial в whale_api.py). Таблица trial_granted
   осталась общей — по ней и API, и прежние выдачи ботом видят, что неделя
   уже была, и второй раз её не дают. Премиум, выданный мимо бота, тот
   подхватывает сам: отпечаток watchersFingerprint() включает is_premium и
   premium_expire, и список наблюдения перестраивается в течение минуты. */

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

void answerCallbackQuery(const std::string& callbackQueryId) {
    json j;
    j["callback_query_id"] = callbackQueryId;
    http("https://api.telegram.org/bot" + TG_TOKEN + "/answerCallbackQuery", j.dump());
}

const std::string MINIAPP_URL = []{
    const char* v = std::getenv("WHALE_MINIAPP_URL");
    return std::string(v ? v : "");
}();

std::string openAppKeyboard(Lang lang) {
    if (MINIAPP_URL.empty()) return "";
    json kb;
    kb["inline_keyboard"] = json::array({json::array({
        {{"text", tr(lang, "menu_open_app")}, {"web_app", {{"url", MINIAPP_URL}}}}
    })});
    return kb.dump();
}

// Ответ на любое сообщение и на кнопки старых меню: всё теперь в приложении.
void sendOpenApp(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    sendMsg(chatId, tr(lang, "start_open_app"), openAppKeyboard(lang));
}

/* Меню команд в чате больше не нужно: всё открывается из приложения.
   Список команд стираем — у людей, открывших бота
   раньше, он иначе так и висел бы со старыми командами — политика, условия
   и удаление данных теперь в приложении. */
void setupBotCommands() {
    http("https://api.telegram.org/bot" + TG_TOKEN + "/deleteMyCommands", "{}");
    if (MINIAPP_URL.empty())
        std::cerr << "[TG] WHALE_MINIAPP_URL не задан — кнопки «Открыть приложение» не будет" << std::endl;
}

std::string buildAlertMessage(const std::string& label, const std::string& wallet,
                              const TxResult& res, const std::string& hash, Lang lang) {
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

        if (!res.isBuy) {
            SellPnl pnl;
            if (sellOutcome(wallet, res.tokenAddr,
                            static_cast<long long>(res.usdNanos),
                            res.rawAmount.convert_to<std::string>(), hash, pnl)) {
                if (pnl.avgEntryNanos > 0)
                    msg += "\U0001F4CA " + tr(lang, "alert_avg_entry") + ": <b>"
                         + formatPriceUsd(cpp_int(pnl.avgEntryNanos)) + "</b>\n";
                msg += (pnl.pnlNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ")
                     + tr(lang, "alert_trade_pnl") + ": <b>"
                     + formatUsdNanosSigned(pnl.pnlNanos, true) + "</b> ("
                     + formatPercent(pnl.pnlPercent, true) + ")\n";
            }
        }

        if (res.isBuy) {
            PriorBuy prior;
                if (lastBuyOutcome(wallet, res.tokenAddr, hash,
                                   static_cast<long long>(unitPriceNanos), prior)) {
                if (prior.avgEntryNanos > 0 && prior.buyCount > 1)
                    msg += "\U0001F4CA " + tr(lang, "alert_avg_entry") + ": <b>"
                         + formatPriceUsd(cpp_int(prior.avgEntryNanos)) + "</b>\n";
                if (prior.changePercent != 0.0 &&
                    prior.changePercent < 1000000.0 && prior.changePercent > -1000000.0) {
                    msg += (prior.changePercent >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ")
                         + tr(lang, "alert_prior_buy") + ": <b>"
                         + formatPriceUsd(cpp_int(prior.thenPriceNanos)) + "</b> "
                         + formatHoldTime(prior.ageSeconds, lang) + " "
                         + tr(lang, "alert_prior_ago") + " \u2192 <b>"
                         + formatPercent(prior.changePercent, true) + "</b>\n";
                }
            }
        }
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

bool isSaneAlertNotional(const TxResult& res) {
    if (res.usdNanos <= 0) return true;
    static const cpp_int kMaxUsdNanos = cpp_int("50000000000000000");
    return res.usdNanos <= kMaxUsdNanos;
}

void dispatchAlert(const std::string& mA, const TxResult& res, const std::string& hash) {
    if (!isSaneAlertNotional(res)) return;
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
        std::string msg = buildAlertMessage(labelLang.first, mA, res, hash, labelLang.second);
        if (g_msgQueue.enqueueToRecipients(msg, chatIds)) anySent = true;
    }
    if (anySent) {
        g_stats.alerts_sent.fetch_add(byLabelLang.size());
        std::cout << "[OK] " << mA << " " << (res.isSwap?(res.isBuy?"BUY":"SELL"):"TRANSFER") << " "
                  << formatUsd(res.usdNanos) << " " << getSymbol(res.tokenAddr)
                  << " -> " << byLabelLang.size() << " label group(s)" << std::endl;
    } else std::cerr << "[WARN] Broadcast failed for " << hash << std::endl;
}

/* Алерт по кошельку биткоина. Кому слать — те же наблюдатели и те же
   правила, что у BSC: порог человека, язык, сервисный аккаунт молчит,
   бесплатному — только основной кошелёк (это уже сделал refreshWatchers).
   Покупка или продажа определена сканером по второй стороне транзакции. */
static std::string btcQty(long long sats) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.8f", static_cast<double>(sats) / 1e8);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string buildBtcAlertMessage(const std::string& label, const BtcAlert& a, Lang lang) {
    std::string msg = "\U0001F4BC <b>" + safeString(label) + "</b>\n\n";
    switch (a.kind) {
        case BtcAlert::BUY:  msg += "\U0001F7E2 <b>" + tr(lang, "alert_buy") + "</b>"; break;
        case BtcAlert::SELL: msg += "\U0001F6A8 <b>" + tr(lang, "alert_sell") + "</b>"; break;
        case BtcAlert::IN:   msg += "\U0001F4E5 <b>" + tr(lang, "alert_transfer") + "</b>"; break;
        default:             msg += "\U0001F4E4 <b>" + tr(lang, "alert_transfer") + "</b>"; break;
    }
    if (a.txs > 1) msg += " \u00D7" + std::to_string(a.txs);
    msg += "\n\U0001F4B0 " + tr(lang, "alert_amount") + ": <b>" + formatUsd(cpp_int(a.usdNanos)) + "</b>\n";
    msg += "\U0001FA99 " + tr(lang, "alert_token") + ": <b>BTC</b>\n";
    msg += "\U0001F4E6 " + tr(lang, "alert_qty") + ": <b>" + btcQty(a.sats) + "</b>\n";
    if ((a.kind == BtcAlert::BUY || a.kind == BtcAlert::SELL) && a.priceNanos > 0)
        msg += "\U0001F4B5 " + tr(lang, a.kind == BtcAlert::BUY ? "alert_buy_price" : "alert_sell_price") +
               ": <b>" + formatPriceUsd(cpp_int(a.priceNanos)) + "</b>\n";
    if (a.avgEntryNanos > 0 && (a.kind == BtcAlert::SELL || a.priorBuys > 1))
        msg += "\U0001F4CA " + tr(lang, "alert_avg_entry") + ": <b>" + formatPriceUsd(cpp_int(a.avgEntryNanos)) + "</b>\n";
    if (a.hasPnl)
        msg += std::string(a.pnlNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ") + tr(lang, "alert_trade_pnl") + ": <b>" +
               formatUsdNanosSigned(a.pnlNanos, true) + "</b> (" + formatPercent(a.pnlPct, true) + ")\n";
    if (!a.ex.empty())
        msg += "\U0001F3E6 " + tr(lang, a.kind == BtcAlert::BUY ? "alert_from_exchange" : "alert_to_exchange") +
               ": <b>" + safeString(a.ex, 32) + "</b>\n";
    msg += "\U0001F194 TX: <code>" + safeString(a.txid, 66) + "</code>\n";
    msg += "\U0001F4BC " + tr(lang, "alert_wallet") + ": <b>" + safeString(label) + "</b>\n\n";
    msg += "\U0001F517 <a href=\"https://mempool.space/tx/" + safeString(a.txid, 66) + "\">" +
           tr(lang, "alert_transaction") + "</a>";
    return msg;
}

void dispatchBtcAlert(const BtcAlert& a) {
    std::map<std::pair<std::string, Lang>, std::vector<std::string>> byLabelLang;
    {
        std::shared_ptr<const std::unordered_map<std::string, std::vector<Watcher>>> watchers;
        { std::shared_lock l(watchersMutex); watchers = WATCHERS_PTR; }
        if (!watchers) return;
        auto wit = watchers->find(a.key);
        if (wit == watchers->end()) return;
        for (const auto& w : wit->second) {
            if (a.usdNanos < static_cast<long long>(w.thresholdNanos)) continue;
            if (w.chatId == SERVICE_CHAT_ID) continue;
            byLabelLang[{w.label, langFromCode(getUserLanguage(w.chatId))}].push_back(w.chatId);
        }
    }
    if (byLabelLang.empty()) return;
    bool anySent = false;
    for (auto& [labelLang, chatIds] : byLabelLang)
        if (g_msgQueue.enqueueToRecipients(buildBtcAlertMessage(labelLang.first, a, labelLang.second), chatIds))
            anySent = true;
    if (anySent) {
        g_stats.alerts_sent.fetch_add(byLabelLang.size());
        static const char* kinds[] = {"BUY", "SELL", "IN", "OUT"};
        std::cout << "[OK][BTC] " << a.wallet << " " << kinds[a.kind & 3] << " " << btcQty(a.sats) << " BTC -> "
                  << byLabelLang.size() << " label group(s)" << std::endl;
    }
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
        saveTrade(p.wallet, p.agg, p.hash, p.block, p.blockTs);
        (void)serviceWatched;
        saveWalletHistory(p.wallet, p.agg, p.hash, p.blockTs);
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
    std::shared_ptr<const std::unordered_set<std::string>> bscActive;
    { std::shared_lock l(watchersMutex); watchers = WATCHERS_PTR; bscActive = BSC_ACTIVE_PTR; }

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
        if (bscActive && watchers) {
            if (bscActive->count(from) && watchers->count(from)) mA=from;
            else if (bscActive->count(to) && watchers->count(to)) mA=to;
        }
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

        if (!isSaneAlertNotional(res)) {
            std::cerr << "[ALERT] skip insane notional " << hash
                      << " usdNanos=" << res.usdNanos.convert_to<std::string>()
                      << " token=" << res.tokenAddr << std::endl;
            markTxProcessed(hash,bn);
            continue;
        }
        if (res.isSwap) bufferSwap(mA, res, hash, bn, blockTs);
        else { saveWalletHistory(mA, res, hash, blockTs); dispatchAlert(mA, res, hash); }
        markTxProcessed(hash,bn);
    }
    saveLastBlockHash(block.is_object()?block.value("hash",""):""); return true;
}

void cleanupOldAlerts() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    const long long now = static_cast<long long>(time(nullptr));

    // 6 — «только в приложении»: такая же завершённая доставка, срок у неё тот же.
    if (prepareOrLog(db,&s,"DELETE FROM deliveries WHERE status IN (1,2,4,6) AND id IN (SELECT d.id FROM deliveries d JOIN alerts a ON a.id=d.alert_id WHERE a.created_at<?)")) {
        sqlite3_bind_int64(s,1,now-2*86400); sqlite3_step(s); int dd=sqlite3_changes(db); sqlite3_finalize(s);
        if (dd>0) std::cout << "[CLEANUP] Removed " << dd << " terminal deliveries" << std::endl; }

    if (prepareOrLog(db,&s,"DELETE FROM alerts WHERE id IN (SELECT a.id FROM alerts a WHERE a.created_at<? AND NOT EXISTS (SELECT 1 FROM deliveries d WHERE d.alert_id=a.id AND d.status IN (0,3)))")) {
        sqlite3_bind_int64(s,1,now-3*86400); sqlite3_step(s); int da=sqlite3_changes(db); sqlite3_finalize(s);
        if (da>0) std::cout << "[CLEANUP] Removed " << da << " old alerts" << std::endl; }

    if (prepareOrLog(db,&s,"DELETE FROM deliveries WHERE status IN (1,2,4,6) AND NOT EXISTS (SELECT 1 FROM alerts a WHERE a.id=deliveries.alert_id)")) {
        sqlite3_step(s); int orp=sqlite3_changes(db); sqlite3_finalize(s);
        if (orp>0) std::cout << "[CLEANUP] Removed " << orp << " orphaned deliveries" << std::endl; }
}

void dbMaintenanceLoop() {
    auto lastTruncate = std::chrono::steady_clock::now();
    while (running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::minutes(1));
        try {
            cleanupTokenPricesPeriodic();

            // Изменения, пришедшие мимо бота — из мини-аппа. Первый проход
            // только запоминает слепок и ничего не перестраивает.
            {
                static std::string lastFp = watchersFingerprint();
                const std::string fp = watchersFingerprint();
                if (fp != lastFp) {
                    lastFp = fp;
                    refreshWatchers();
                    std::cout << "[WATCHERS] база изменилась мимо бота — список наблюдения обновлён"
                              << std::endl;
                }
            }

            bool doTruncate = std::chrono::duration_cast<std::chrono::minutes>(
                std::chrono::steady_clock::now() - lastTruncate).count() >= 30;
            walCheckpoint(doTruncate ? SQLITE_CHECKPOINT_TRUNCATE : SQLITE_CHECKPOINT_PASSIVE);
            if (doTruncate) {
                cleanupOldTx(g_lastProcessedBlock.load(std::memory_order_relaxed));
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

/* Служебные команды владельца: состояние сканера, импорт кошельков в
   сервисный аккаунт, снятие бана. Остальным они не видны — на любой текст
   человек получает кнопку приложения. */
bool handleOwnerCommand(const std::string& cid, const std::string& txt) {
    if (cid != OWNER_CHAT_ID || txt.empty() || txt[0] != '/') return false;
    if (txt=="/health") {
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
    else if (txt=="/statsbtc") {
        // Сеть Bitcoin — отдельной командой: в /stats и так
        // десятки строк про BSC и Hyperliquid.
        sendMsg(cid, btcStatsLine());
    }
    else if (txt=="/stats") {
        size_t qs=g_msgQueue.size(); size_t uc=countUsers(); int64_t fc=0;
        { std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s; if (prepareOrLog(db,&s,"SELECT COUNT(*) FROM deliveries WHERE status=4")) { if (sqlite3_step(s)==SQLITE_ROW) fc=sqlite3_column_int64(s,0); sqlite3_finalize(s); } }
        std::string langStats;
        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (prepareOrLog(db, &s,
                "SELECT COALESCE(NULLIF(TRIM(u.language), ''), 'en') AS lang, "
                "COUNT(*), "
                "SUM(CASE WHEN EXISTS(SELECT 1 FROM user_whales uw WHERE uw.user_id=u.chat_id) "
                "THEN 1 ELSE 0 END) "
                "FROM users u GROUP BY lang ORDER BY COUNT(*) DESC, lang ASC")) {
                struct LangRow { std::string lang; long long users, active; };
                std::vector<LangRow> rows;
                long long total = 0, totalActive = 0;
                while (sqlite3_step(s) == SQLITE_ROW) {
                    LangRow r;
                    r.lang = safeColumnText(s, 0);
                    r.users = sqlite3_column_int64(s, 1);
                    r.active = sqlite3_column_int64(s, 2);
                    total += r.users;
                    totalActive += r.active;
                    rows.push_back(std::move(r));
                }
                sqlite3_finalize(s);
                if (!rows.empty()) {
                    std::ostringstream ls;
                    ls << "🌐 Languages (с кошельком):";
                    for (const auto& r : rows) {
                        ls << "\n· " << r.lang << ": <b>" << r.users << "</b>";
                        if (total > 0) ls << " (" << (r.users * 100 / total) << "%)";
                        ls << " → <b>" << r.active << "</b>";
                        if (r.users > 0) ls << " (" << (r.active * 100 / r.users) << "%)";
                    }
                    if (total > 0)
                        ls << "\n· всего с кошельком: <b>" << totalActive
                           << "</b> из " << total
                           << " (" << (totalActive * 100 / total) << "%)";
                    langStats = ls.str();
                }
            }
        }

        std::stringstream ss2; ss2 << "📊 <b>Stats</b>\n\n👥 Users: <b>" << uc << "</b>\n📬 Queue: <b>" << qs << "</b>\n❌ Failed: <b>" << fc << "</b>"
              << "\n🧵 Потоки: <b>" << g_msgQueue.busy() << "/"
              << g_msgQueue.threads() << "</b> заняты · отправлено <b>"
              << g_msgQueue.sent() << "</b>"
              << "\n⏱ Uptime: <b>" << getUptime() << "</b>";
        if (!langStats.empty()) ss2 << "\n" << langStats;
        ss2 << "\n\n"
            << "⚙️ RPC: " << g_stats.rpc_failures.load() << " попыток · "
            << g_stats.rpc_giveups.load() << " отказов"
            << "\n💰 Цена: кэш " << g_stats.price_cache_hit.load()
            << " · пул " << g_stats.price_from_pool.load()
            << " · DexScreener " << g_stats.price_from_dex.load()
            << " · CoinGecko " << g_stats.price_from_cg.load()
            << "\n💰 Защита: тонкий пул " << g_stats.price_thin_pool.load()
            << " · устаревший кэш " << g_stats.price_fallbacks.load()
            << " · расхождение " << g_stats.price_divergence.load()
            << " · скачок " << g_stats.price_spike_reject.load()
            << "\n🔄 REORG: " << g_stats.reorg_verifications.load()
            << "\n📨 Sent: " << g_stats.alerts_sent.load()
            << "\n🔍 TX: " << g_stats.tx_processed.load()
            << "\n⏳ Lag: " << g_stats.current_lag.load()
            << " blocks (max: " << g_stats.max_lag_seen.load() << ")";
        if (wsHeadsOk()) {
            ss2 << "\n🔌 WS: ✅ " << wsHeadsActiveLabel() << " · блок " << wsHeadsLatest();
        } else {
            ss2 << "\n🔌 WS: ❌ HTTP fallback"
                << (wsHeadsLatest() > 0
                        ? (std::string(" · last ") + std::to_string(wsHeadsLatest()))
                        : "");
        }
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
    else if (txt.rfind("/import", 0) == 0) {
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
        // Адреса биткоина — в свою базу того же сервисного
        // аккаунта (btc.db). Регистр base58 значим, поэтому
        // разбираем исходный текст, а не строчную копию.
        std::vector<std::string> btcFound;
        {
            std::string tok;
            auto flush = [&] {
                if (!tok.empty() && isBtcAddress(tok)) btcFound.push_back(normBtcAddress(tok));
                tok.clear();
            };
            for (char ch : txt) {
                if (std::isalnum(static_cast<unsigned char>(ch))) tok += ch;
                else flush();
            }
            flush();
        }
        if (found.empty() && btcFound.empty()) {
            sendMsg(cid, "Использование: /import 0x... bc1... (адреса BSC, Hyperliquid и Bitcoin через пробел, запятую или с новой строки)");
        } else if (found.empty()) {
            BtcImportResult br = btcImport(btcFound);
            std::stringstream rep;
            rep << "\U0001F4E5 <b>Импорт завершён</b>\n\n"
                << "₿ Адресов Bitcoin: <b>" << btcFound.size() << "</b>\n"
                << "✅ Добавлено: <b>" << br.added << "</b>\n"
                << "↩️ Уже в базе: <b>" << br.dup << "</b>\n"
                << "\nКошельков Bitcoin на сервисном аккаунте: <b>" << btcWatchCount() << "</b>";
            sendMsg(cid, rep.str());
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
            if (!btcFound.empty()) {
                BtcImportResult br = btcImport(btcFound);
                rep << "\n\n₿ Bitcoin: найдено <b>" << btcFound.size() << "</b>, добавлено <b>"
                    << br.added << "</b>, уже в базе <b>" << br.dup << "</b>"
                    << "\nКошельков Bitcoin на сервисном аккаунте: <b>" << btcWatchCount() << "</b>";
            }
            sendMsg(cid, rep.str());
        }
    }
    else if (txt.rfind("/unban", 0) == 0) {
        std::string arg = trim(txt.substr(6));
        if (!isValidAddress(arg)) {
            sendMsg(cid, "Использование: /unban 0x&lt;адрес&gt;");
        } else if (liftPermanentBan(arg)) {
            sendMsg(cid, "✅ Бан снят: <code>" + toLower(arg) + "</code>\nКошелёк снова может попадать в рейтинг.");
        } else {
            sendMsg(cid, "ℹ️ У этого адреса нет пожизненного бана: <code>" + toLower(arg) + "</code>");
        }
    }
    else if (handleBeneficiaryCommand(cid, txt)) {
    }
    else return false;
    return true;
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
                offset=cuid+1; if (++ub%5==0) saveTgOffset(offset);

                // Кнопки старых меню в истории чата: снять «часики» и
                // показать кнопку приложения.
                if (u.contains("callback_query")&&u["callback_query"].is_object()) {
                    const json& cq = u["callback_query"];
                    if (cq.contains("id") && cq["id"].is_string())
                        answerCallbackQuery(cq["id"].get<std::string>());
                    if (cq.contains("message") && cq["message"].is_object() &&
                        cq["message"].contains("chat") && cq["message"]["chat"].is_object() &&
                        cq["message"]["chat"].contains("id")) {
                        const std::string ccid = std::to_string(cq["message"]["chat"]["id"].get<long>());
                        if (g_rateLimiter.allow(ccid)) sendOpenApp(ccid);
                    }
                    continue;
                }

                if (u.contains("pre_checkout_query")&&u["pre_checkout_query"].is_object()) {
                    handlePreCheckoutQuery(u["pre_checkout_query"]);
                    continue;
                }

                if (!u.contains("message")||!u["message"].is_object()) continue;
                const json& m = u["message"];
                if (!m.contains("chat")||!m["chat"].is_object()||!m["chat"].contains("id")) continue;
                const std::string cid=std::to_string(m["chat"]["id"].get<long>());

                if (m.contains("successful_payment")) {
                    handleSuccessfulPayment(cid, m["successful_payment"]);
                    continue;
                }
                if (!g_rateLimiter.allow(cid)) continue;

                const std::string txt = m.contains("text") && m["text"].is_string() ? m["text"].get<std::string>() : "";
                if (handleOwnerCommand(cid, txt)) continue;

                std::string tgLang;
                if (m.contains("from") && m["from"].is_object() &&
                    m["from"].contains("language_code") && m["from"]["language_code"].is_string())
                    tgLang = m["from"]["language_code"].get<std::string>();
                ensureUser(cid, tgLang);
                sendOpenApp(cid);
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
        {
            if (chainName == "bsc" || chainName == "bnb")
                startWsBsc();
            else
                std::cout << "[WS] skip (non-BSC chain)" << std::endl;
        }
    }
    setRpcFailureHandler([]{
        g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed);
        g_stats.last_rpc_failure.store(time(nullptr), std::memory_order_relaxed);
    });
    setPriceStatHandler([](int kind){
        switch (kind) {
            case 0: g_stats.price_from_pool.fetch_add(1, std::memory_order_relaxed); break;
            case 1: g_stats.price_thin_pool.fetch_add(1, std::memory_order_relaxed); break;
            case 2: g_stats.price_fallbacks.fetch_add(1, std::memory_order_relaxed); break;
            case 3: g_stats.rpc_failures.fetch_add(1, std::memory_order_relaxed); break; // meta rpc
            case 4: g_stats.price_divergence.fetch_add(1, std::memory_order_relaxed); break;
            case 5: g_stats.price_spike_reject.fetch_add(1, std::memory_order_relaxed); break;
            case 6: g_stats.price_from_dex.fetch_add(1, std::memory_order_relaxed); break;
            case 7: g_stats.price_from_cg.fetch_add(1, std::memory_order_relaxed); break;
            case 8: g_stats.price_cache_hit.fetch_add(1, std::memory_order_relaxed); break;
            default: break;
        }
    });
    setRpcGiveUpHandler([]{
        g_stats.rpc_giveups.fetch_add(1, std::memory_order_relaxed);
    });
    initDB(); initRankingDB();
    if (!initPremium(TG_TOKEN, SERVICE_CHAT_ID)) {
        std::cerr << "[STARTUP][FATAL] Premium schema init failed — payments are DISABLED for this run" << std::endl;
    }
    loadTokenCache();
    loadPairCache();
    ensureNativePrice();
    ensureUser(OWNER_CHAT_ID);
    refreshWatchers();
    checkTranslations();

    if (!initHyperliquid()) {
        std::cerr << "[STARTUP] Hyperliquid init failed - perps DISABLED for this run" << std::endl;
    } else {
        startHyperliquidLoop();
    }
    btcSetAlertSink(dispatchBtcAlert);
    startBtcLoop();
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
    auto ltp=std::chrono::steady_clock::now();
    auto lrt=std::chrono::steady_clock::now()-std::chrono::minutes(10);
    while (running.load(std::memory_order_relaxed)) {
        try {
            long long lat = 0;
            if (wsHeadsOk()) {
                lat = static_cast<long long>(wsHeadsLatest());
            }
            if (lat <= 0) {
                auto lj=rpc("eth_blockNumber",{});
                if (!lj.is_string()||!hexToLL(lj.get<std::string>(),lat)) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }
            }
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
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-ltp).count()>=20) {
                pollUsdtPayments();
                ltp=std::chrono::steady_clock::now();
            }
            if (std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now()-lrt).count()>=5) {
                ensureNativePrice();
                lrt=std::chrono::steady_clock::now();
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
    stopBtc();
    stopWsBsc();
    walCheckpoint();
    closeRankingDB();
    if (db) sqlite3_close(db);
    curl_global_cleanup();
    std::cout << "[SHUTDOWN] Clean exit." << std::endl; return 0;
}
