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
#include "autobase.h"
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
#include "lifecycle.h"
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

// Адрес Bot API. По умолчанию — сам Telegram; WHALE_TG_API нужен для
// своего сервера Bot API или для проверки бота на подставном.
const std::string TG_API_BASE = []{
    const char* v = std::getenv("WHALE_TG_API");
    return std::string(v && *v ? v : "https://api.telegram.org");
}();

std::string tgApi(const std::string& method) {
    return TG_API_BASE + "/bot" + TG_TOKEN + "/" + method;
}

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
        CREATE INDEX IF NOT EXISTS idx_deliveries_terminal ON deliveries(status, alert_id) WHERE status IN (1,2,4);
        CREATE TABLE IF NOT EXISTS pair_cache (
            token TEXT PRIMARY KEY,
            val TEXT NOT NULL
        );
        INSERT OR IGNORE INTO state(key,value) VALUES ('tg_offset','0');
    )";
    // Сначала таблицы, потом добавление колонок: на новой базе ALTER до
    // CREATE не находил таблицу, и user_whales жила без is_primary до
    // второго запуска — список наблюдения в первый запуск не собирался.
    // Поэтому в общем CREATE нет ничего, что опирается на колонки из ALTER.
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[FATAL] Schema init failed: " << err << std::endl; sqlite3_free(err); sqlite3_close(db); std::exit(1);
    }

    {
        char* mErr = nullptr;
        if (sqlite3_exec(db, "ALTER TABLE deliveries ADD COLUMN priority INTEGER NOT NULL DEFAULT 0",
                         nullptr, nullptr, &mErr) == SQLITE_OK)
            std::cout << "[STARTUP] deliveries: added priority column" << std::endl;
        if (mErr) sqlite3_free(mErr);
        // Индекс по priority — только после того, как колонка точно есть:
        // в общем CREATE он ронял бы запуск на базе, созданной до неё.
        sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_deliveries_prio ON deliveries"
                         "(status, next_retry_at, priority DESC, id) WHERE status IN (0,3)",
                     nullptr, nullptr, nullptr);
    }
    /* Куда слать алерты и что человек уже видел — выбирается в мини-аппе.
       alert_tg=0 — «только в приложении»: в чат алерты не идут, лежат в
       истории. alerts_seen_at — когда человек последний раз открыл историю;
       всё новее считается непрочитанным. API добавляет те же колонки сам,
       если бот ещё не обновлён, — поэтому ошибка «уже есть» здесь норма. */
    for (const char* sql : {
            "ALTER TABLE users ADD COLUMN alert_tg INTEGER NOT NULL DEFAULT 1",
            "ALTER TABLE users ADD COLUMN alerts_seen_at INTEGER NOT NULL DEFAULT 0",
            // Кнопка под алертом (бесплатному — «открыть цену входа и PnL»).
            "ALTER TABLE alerts ADD COLUMN markup TEXT NOT NULL DEFAULT ''",
            // Тот же алерт полями (JSON) — из него приложение рисует карточку.
            "ALTER TABLE alerts ADD COLUMN data TEXT NOT NULL DEFAULT ''"}) {
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
            CREATE TABLE IF NOT EXISTS digest_subs (
                chat_id TEXT PRIMARY KEY,
                at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS token_subs (
                chat_id TEXT PRIMARY KEY,
                at INTEGER NOT NULL
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

    // Воронка продаж: события пишут API (открыл, увидел замок, нажал оплату)
    // и премиум бота (оплатил). Та же схема, что FUNNEL_SCHEMA в API.
    {
        char* ferr = nullptr;
        if (sqlite3_exec(db,
                "CREATE TABLE IF NOT EXISTS funnel_events ("
                " chat_id TEXT NOT NULL, ev TEXT NOT NULL, src TEXT NOT NULL DEFAULT '',"
                " day TEXT NOT NULL, at INTEGER NOT NULL,"
                " PRIMARY KEY (chat_id, ev, src, day));"
                "CREATE INDEX IF NOT EXISTS idx_funnel_day ON funnel_events(day, ev);",
                nullptr, nullptr, &ferr) != SQLITE_OK) {
            std::cerr << "[STARTUP] funnel schema failed: " << (ferr ? ferr : "") << std::endl;
            sqlite3_free(ferr);
        }
    }

    // Заявки на бонус за регистрацию на бирже: кладёт API, решение владельца
    // пишет бот. Та же схема, что EXCH_SCHEMA в API.
    {
        char* eerr = nullptr;
        if (sqlite3_exec(db,
                "CREATE TABLE IF NOT EXISTS exch_claims ("
                " id INTEGER PRIMARY KEY AUTOINCREMENT, chat_id TEXT NOT NULL, ex TEXT NOT NULL,"
                " uid TEXT NOT NULL, at INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'wait',"
                " decided_at INTEGER NOT NULL DEFAULT 0, granted_at INTEGER NOT NULL DEFAULT 0,"
                " days INTEGER NOT NULL DEFAULT 0, notified INTEGER NOT NULL DEFAULT 0);"
                "CREATE INDEX IF NOT EXISTS idx_exch_chat ON exch_claims(chat_id, ex);"
                "CREATE UNIQUE INDEX IF NOT EXISTS idx_exch_uid ON exch_claims(ex, uid) WHERE status!='no';",
                nullptr, nullptr, &eerr) != SQLITE_OK) {
            std::cerr << "[STARTUP] exch schema failed: " << (eerr ? eerr : "") << std::endl;
            sqlite3_free(eerr);
        }
    }

    // Каналы партнёров: заводит владелец (/partner), подписки и пришедших по
    // ссылке блогера пишет API. Та же схема, что PARTNER_SCHEMA в API.
    {
        char* perr = nullptr;
        if (sqlite3_exec(db,
                "CREATE TABLE IF NOT EXISTS partner_channels ("
                " handle TEXT PRIMARY KEY, title TEXT NOT NULL DEFAULT '', days INTEGER NOT NULL DEFAULT 2,"
                " active INTEGER NOT NULL DEFAULT 1, slug TEXT NOT NULL DEFAULT '', added_at INTEGER NOT NULL DEFAULT 0);"
                "CREATE TABLE IF NOT EXISTS partner_claims ("
                " chat_id TEXT NOT NULL, handle TEXT NOT NULL, joined_at INTEGER NOT NULL, check_at INTEGER NOT NULL,"
                " status TEXT NOT NULL DEFAULT 'wait', done_at INTEGER NOT NULL DEFAULT 0, days INTEGER NOT NULL DEFAULT 0,"
                " PRIMARY KEY (chat_id, handle));"
                "CREATE INDEX IF NOT EXISTS idx_partner_claims_due ON partner_claims(status, check_at);"
                "CREATE TABLE IF NOT EXISTS partner_refs (chat_id TEXT PRIMARY KEY, slug TEXT NOT NULL, at INTEGER NOT NULL);"
                "CREATE INDEX IF NOT EXISTS idx_partner_refs_slug ON partner_refs(slug);",
                nullptr, nullptr, &perr) != SQLITE_OK) {
            std::cerr << "[STARTUP] partner schema failed: " << (perr ? perr : "") << std::endl;
            sqlite3_free(perr);
        }
    }
}

/* Воронка за неделю для /stats: сколько разных людей дошли до каждого шага
   и где чаще всего упираются в замок. */
std::string funnelStatsLine() {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    const time_t since = time(nullptr) - 7 * 86400;
    std::map<std::string, long long> n;
    if (prepareOrLog(db, &s, "SELECT ev, COUNT(DISTINCT chat_id) FROM funnel_events WHERE at >= ? GROUP BY ev")) {
        sqlite3_bind_int64(s, 1, since);
        while (sqlite3_step(s) == SQLITE_ROW) n[safeColumnText(s, 0)] = sqlite3_column_int64(s, 1);
        sqlite3_finalize(s);
    }
    std::ostringstream out;
    out << "\n\n\U0001F4B0 <b>Воронка за 7 дней</b> (людей)"
        << "\nоткрыли " << n["open"] << " → кошелёк " << n["wallet"]
        << " → замок " << n["paywall"] << " → оплата " << n["checkout"]
        << " → купили <b>" << n["paid"] << "</b>";
    // Доля купивших от открывших — главное число воронки.
    if (n["open"] > 0)
        out << " (" << std::fixed << std::setprecision(1) << 100.0 * n["paid"] / n["open"] << "% открывших)";
    out << "\nпробных: " << n["trial"] << " · по приглашению: " << n["ref"] << " · бонусы за соцсети: " << n["bonus"];
    if (prepareOrLog(db, &s, "SELECT COUNT(*) FROM token_subs")) {
        if (sqlite3_step(s) == SQLITE_ROW) out << " · ждут токен: " << sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    // Защита от накруток (API): сколько проб не выдано (3+ новых аккаунта с
    // одного IP за сутки) и сколько наград за друзей отклонено (общий IP).
    if (prepareOrLog(db, &s, "SELECT SUM(src='trial_ip'), SUM(src='ref_ip') FROM funnel_events "
                             "WHERE ev='abuse' AND at >= ?")) {
        sqlite3_bind_int64(s, 1, since);
        if (sqlite3_step(s) == SQLITE_ROW)
            out << "\n🛡 Накрутки: проб не выдано " << sqlite3_column_int64(s, 0)
                << " · наград за друзей отклонено " << sqlite3_column_int64(s, 1);
        sqlite3_finalize(s);
    }
    // Автопополнение базы китов (BSC и Hyperliquid; биткоин — отдельно, /statsbtc).
    if (prepareOrLog(db, &s, "SELECT SUM(label='auto-bsc' AND created_at>=?), SUM(label='auto-hl' AND created_at>=?), "
                             "SUM(label='auto-bsc'), SUM(label='auto-hl') FROM user_whales WHERE user_id=?")) {
        const long long day = time(nullptr) - 86400;
        sqlite3_bind_int64(s, 1, day);
        sqlite3_bind_int64(s, 2, day);
        sqlite3_bind_text(s, 3, SERVICE_CHAT_ID.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW)
            out << "\n🐋 Автобаза за сутки: BSC +" << sqlite3_column_int64(s, 0) << " · Hyperliquid +"
                << sqlite3_column_int64(s, 1) << " (всего авто: " << sqlite3_column_int64(s, 2) << " / "
                << sqlite3_column_int64(s, 3) << ")";
        sqlite3_finalize(s);
    }
    if (prepareOrLog(db, &s, "SELECT SUM(status='wait'), SUM(status='ok') FROM exch_claims")) {
        if (sqlite3_step(s) == SQLITE_ROW)
            out << "\nOKX: ждут проверки " << sqlite3_column_int64(s, 0) << " · одобрено " << sqlite3_column_int64(s, 1)
                << (sqlite3_column_int64(s, 0) ? " (/okx)" : "");
        sqlite3_finalize(s);
    }
    if (prepareOrLog(db, &s,
            "SELECT src, COUNT(DISTINCT chat_id) c FROM funnel_events WHERE at >= ? AND ev='paywall' "
            "GROUP BY src ORDER BY c DESC LIMIT 8")) {
        sqlite3_bind_int64(s, 1, since);
        std::string tops;
        while (sqlite3_step(s) == SQLITE_ROW)
            tops += (tops.empty() ? "" : " · ") + safeColumnText(s, 0) + " " + std::to_string(sqlite3_column_int64(s, 1));
        sqlite3_finalize(s);
        if (!tops.empty()) out << "\nзамки: " << tops;
    }
    return out.str();
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
                if (!prem && uid != SERVICE_CHAT_ID && loadedForUser >= FREE_ALERT_WALLETS) continue;
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
        static int cc=0;
        if (++cc%1000==0) {
            for (auto it=users.begin();it!=users.end();) {
                if (std::chrono::duration_cast<std::chrono::hours>(now-it->second.last).count()>CLEANUP_H) it=users.erase(it);
                else ++it;
            }
        }
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
    auto r = http(tgApi("sendMessage"), j.dump());
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

void answerCallbackQuery(const std::string& callbackQueryId, const std::string& text = "") {
    json j;
    j["callback_query_id"] = callbackQueryId;
    if (!text.empty()) j["text"] = text;
    http(tgApi("answerCallbackQuery"), j.dump());
}

const std::string MINIAPP_URL = []{
    const char* v = std::getenv("WHALE_MINIAPP_URL");
    return std::string(v ? v : "");
}();

std::string openAppKeyboard(Lang lang, const std::string& go, const char* btn) {
    if (MINIAPP_URL.empty()) return "";
    std::string url = MINIAPP_URL;
    if (!go.empty()) {
        // Параметр — до «#»: после него Telegram дописывает подпись запуска.
        const size_t hash = url.find('#');
        const std::string tail = hash == std::string::npos ? "" : url.substr(hash);
        if (hash != std::string::npos) url.resize(hash);
        url += (url.find('?') == std::string::npos ? "?go=" : "&go=") + go + tail;
    }
    json kb;
    kb["inline_keyboard"] = json::array({json::array({
        {{"text", tr(lang, btn)}, {"web_app", {{"url", url}}}}
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
    http(tgApi("deleteMyCommands"), "{}");
    if (MINIAPP_URL.empty())
        std::cerr << "[TG] WHALE_MINIAPP_URL не задан — кнопки «Открыть приложение» не будет" << std::endl;
}

// Арабский пишется справа налево: метка в начале строки, чтобы эмодзи и
// числа не перескакивали на другой край (как в алертах Hyperliquid).
static const char* rtlMark(Lang lang) { return lang == Lang::AR ? "‏" : ""; }

static double nanosToUsd(const cpp_int& n) { return n.convert_to<double>() / 1e9; }

/* Алерт BSC — в том же виде, что у Hyperliquid: кто, что и на сколько одной
   строкой сверху, ниже только то, по чему решают: цена, количество, что
   отдал или получил, средний вход и результат. Длинный хэш и второй раз
   кошелёк из текста ушли: транзакция — ссылкой, рядом график монеты.

   `full` — алерт подписчика. Бесплатному уходит тот же алерт без средней
   цены входа, PnL сделки и прошлой покупки — и одной строкой сказано, что
   это есть в премиуме. Строка появляется, только если было что скрыть.

   `card` — те же данные полями, без языка: приложение рисует из них
   карточку на языке человека (alerts.data). */
std::string buildAlertMessage(const std::string& label, const std::string& wallet,
                              const TxResult& res, const std::string& hash, Lang lang, bool full = true,
                              json* card = nullptr) {
    const char* const dm = rtlMark(lang);
    bool locked = false;
    const bool tokenIsNative = (res.tokenAddr == chainCtx().nativeMarker);
    const std::string tokenSymbol = tokenIsNative ? chainCtx().nativeSymbol : safeString(getSymbol(res.tokenAddr), 32);
    const int tokenDecimals = tokenIsNative ? 18 : getDecimals(res.tokenAddr);

    // Что произошло: значок, слово и код для приложения.
    std::string icon, word, act;
    if (res.venue == "Add Liquidity")         { icon = "\U0001F30A"; word = tr(lang, "alert_add_liquidity"); act = "add_liq"; }
    else if (res.venue == "Remove Liquidity") { icon = "\U0001F30A"; word = tr(lang, "alert_remove_liquidity"); act = "rm_liq"; }
    else if (res.venue == "Collect Fees")     { icon = "\U0001F4B8"; word = tr(lang, "alert_collect_fees"); act = "fees"; }
    else if (res.venue == "Wrap")             { icon = "\U0001F504"; word = tr(lang, "alert_wrap"); act = "wrap"; }
    else if (res.venue == "Unwrap")           { icon = "\U0001F504"; word = tr(lang, "alert_unwrap"); act = "unwrap"; }
    else if (res.venue == "Bridge Out")       { icon = "\U0001F309"; word = tr(lang, "alert_bridge_out"); act = "bridge_out"; }
    else if (res.venue == "Bridge In")        { icon = "\U0001F309"; word = tr(lang, "alert_bridge_in"); act = "bridge_in"; }
    else if (res.venue == "Arbitrage")        { icon = "♻️"; word = tr(lang, "alert_arbitrage"); act = "arb"; }
    else if (res.isSwap && res.isBuy)         { icon = "\U0001F7E2"; word = tr(lang, "alert_buy"); act = "buy"; }
    else if (res.isSwap)                      { icon = "\U0001F534"; word = tr(lang, "alert_sell"); act = "sell"; }
    else                                      { icon = "\U0001F4E4"; word = tr(lang, "alert_transfer"); act = "transfer"; }

    json c;
    if (card) {
        c["v"] = 1; c["k"] = "bsc"; c["a"] = act; c["w"] = wallet; c["n"] = label;
        c["sym"] = tokenSymbol; c["usd"] = nanosToUsd(res.usdNanos); c["tx"] = hash;
        if (tokenDecimals >= 0 && tokenDecimals <= 36)
            c["qty"] = res.rawAmount.convert_to<double>() / std::pow(10.0, tokenDecimals);
        if (!tokenIsNative) c["ca"] = res.tokenAddr;
    }

    std::string msg = std::string(dm) + "\U0001F4BC <b>" + safeString(label) + "</b>\n\n";
    msg += std::string(dm) + icon + " <b>" + word + " " + tokenSymbol + "</b> · <b>" + formatUsdNanosSigned(static_cast<long long>(res.usdNanos), false) + "</b>\n";

    if (res.isSwap) {
        const cpp_int unitPriceNanos = calcUnitPriceNanos(res.usdNanos, res.rawAmount, tokenDecimals);
        msg += std::string(dm) + "\U0001F4B5 " + tr(lang, res.isBuy ? "alert_buy_price" : "alert_sell_price") +
               ": <b>" + formatPriceUsd(unitPriceNanos) + "</b>\n";
        if (card && unitPriceNanos > 0) c["px"] = nanosToUsd(unitPriceNanos);
    }
    msg += std::string(dm) + "\U0001F4E6 " + tr(lang, "alert_qty") + ": <b>" +
           formatAmount(res.rawAmount, tokenDecimals) + " " + tokenSymbol + "</b>\n";

    // Что отдал за покупку или получил за продажу.
    if (res.isSwap && !res.counterAddr.empty()) {
        std::string counterAmountStr, counterSymbol;
        int counterDec = 18;
        if (res.counterAddr == chainCtx().nativeMarker) {
            counterSymbol = chainCtx().nativeSymbol;
        } else {
            counterDec = getDecimals(res.counterAddr);
            counterSymbol = safeString(getSymbol(res.counterAddr), 16);
        }
        counterAmountStr = formatAmount(res.counterAmount, counterDec);
        msg += std::string(dm) + "\U0001F4B1 " + tr(lang, res.isBuy ? "alert_spent" : "alert_received") +
               ": <b>" + counterAmountStr + " " + counterSymbol + "</b>\n";
        if (card && counterDec >= 0 && counterDec <= 36) {
            c["cq"] = res.counterAmount.convert_to<double>() / std::pow(10.0, counterDec);
            c["cs"] = counterSymbol;
        }
    }

    if (res.isSwap && !res.isBuy) {
        SellPnl pnl;
        if (sellOutcome(wallet, res.tokenAddr, static_cast<long long>(res.usdNanos),
                        res.rawAmount.convert_to<std::string>(), hash, pnl)) {
            if (!full) {
                locked = true;
            } else {
                if (pnl.avgEntryNanos > 0) {
                    msg += std::string(dm) + "\U0001F4CA " + tr(lang, "alert_avg_entry") + ": <b>" +
                           formatPriceUsd(cpp_int(pnl.avgEntryNanos)) + "</b>\n";
                    if (card) c["avg"] = pnl.avgEntryNanos / 1e9;
                }
                msg += std::string(dm) + (pnl.pnlNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ") +
                       tr(lang, "alert_trade_pnl") + ": <b>" + formatUsdNanosSigned(pnl.pnlNanos, true) +
                       "</b> (" + formatPercent(pnl.pnlPercent, true) + ")\n";
                if (card) { c["pnl"] = pnl.pnlNanos / 1e9; c["pnlPct"] = pnl.pnlPercent; }
            }
        }
    }

    if (res.isSwap && res.isBuy) {
        const cpp_int unitPriceNanos = calcUnitPriceNanos(res.usdNanos, res.rawAmount, tokenDecimals);
        PriorBuy prior;
        const bool priorOk = lastBuyOutcome(wallet, res.tokenAddr, hash,
                                            static_cast<long long>(unitPriceNanos), prior);
        const bool showAvg = priorOk && prior.avgEntryNanos > 0 && prior.buyCount > 1;
        const bool showPrior = priorOk && prior.changePercent != 0.0 &&
                               prior.changePercent < 1000000.0 && prior.changePercent > -1000000.0;
        if ((showAvg || showPrior) && !full) {
            locked = true;
        } else {
            if (showAvg) {
                msg += std::string(dm) + "\U0001F4CA " + tr(lang, "alert_avg_entry") + ": <b>" +
                       formatPriceUsd(cpp_int(prior.avgEntryNanos)) + "</b>\n";
                if (card) c["avg"] = prior.avgEntryNanos / 1e9;
            }
            if (showPrior) {
                msg += std::string(dm) + (prior.changePercent >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ") +
                       tr(lang, "alert_prior_buy") + " <b>" + formatPriceUsd(cpp_int(prior.thenPriceNanos)) +
                       "</b> " + formatHoldTime(prior.ageSeconds, lang) + " " + tr(lang, "alert_prior_ago") +
                       " → <b>" + formatPercent(prior.changePercent, true) + "</b>\n";
                if (card) c["prior"] = {{"px", prior.thenPriceNanos / 1e9}, {"ago", prior.ageSeconds},
                                        {"chg", prior.changePercent}};
            }
        }
    }

    if (locked) {
        msg += std::string(dm) + "\U0001F512 " + tr(lang, "alert_locked") + "\n";
        if (card) c["lock"] = true;
    }
    if (!tokenIsNative)
        msg += std::string(dm) + "\U0001F4DC <code>" + safeString(res.tokenAddr) + "</code>\n";
    msg += "\n" + std::string(dm) + "\U0001F517 <a href=\"" + chainCtx().explorerUrl + "/tx/" + hash + "\">" +
           tr(lang, "alert_transaction") + "</a>";
    if (!tokenIsNative)
        msg += " · <a href=\"https://dexscreener.com/bsc/" + safeString(res.tokenAddr) + "\">DexScreener</a>";
    if (card) *card = std::move(c);
    return msg;
}

/* Под бесплатным алертом, где скрыты цена входа и PnL, — кнопка прямо на
   экран Премиума. Скрывать было нечего (первая покупка, нет цены) — кнопки
   нет: обещать то, чего в этом алерте и так не было бы, нечестно. */
std::string freeAlertKeyboard(const std::string& msg, Lang lang) {
    if (msg.find(tr(lang, "alert_locked")) == std::string::npos) return "";
    return openAppKeyboard(lang, "premium-alert", "alert_unlock_btn");
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
        // Подписчикам — полный алерт, бесплатным — без цены входа и PnL.
        const std::set<std::string> prem = premiumSubsetOf(chatIds);
        std::vector<std::string> paid, free;
        for (const auto& c : chatIds) (prem.count(c) ? paid : free).push_back(c);
        // Карточка для приложения — теми же данными, что и текст (alerts.data).
        if (!paid.empty()) {
            json card;
            const std::string m = buildAlertMessage(labelLang.first, mA, res, hash, labelLang.second, true, &card);
            if (g_msgQueue.enqueueToRecipients(m, paid, "", card.dump())) anySent = true;
        }
        if (!free.empty()) {
            json card;
            const std::string m = buildAlertMessage(labelLang.first, mA, res, hash, labelLang.second, false, &card);
            if (g_msgQueue.enqueueToRecipients(m, free, freeAlertKeyboard(m, labelLang.second), card.dump())) anySent = true;
        }
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

/* Алерт по биткоин-кошельку — тот же вид, что у BSC и Hyperliquid: что и на
   сколько одной строкой, ниже цена, количество, биржа на той стороне и,
   подписчику, средний вход и результат. `card` — те же данные полями для
   карточки в приложении. */
std::string buildBtcAlertMessage(const std::string& label, const BtcAlert& a, Lang lang, bool full = true,
                                 json* card = nullptr) {
    const char* const dm = rtlMark(lang);
    std::string icon, word, act;
    switch (a.kind) {
        case BtcAlert::BUY:  icon = "\U0001F7E2"; word = tr(lang, "alert_buy"); act = "buy"; break;
        case BtcAlert::SELL: icon = "\U0001F534"; word = tr(lang, "alert_sell"); act = "sell"; break;
        case BtcAlert::IN:   icon = "\U0001F4E5"; word = tr(lang, "alert_transfer"); act = "in"; break;
        default:             icon = "\U0001F4E4"; word = tr(lang, "alert_transfer"); act = "out"; break;
    }
    std::string msg = std::string(dm) + "\U0001F4BC <b>" + safeString(label) + "</b>\n\n";
    msg += std::string(dm) + icon + " <b>" + word + " BTC</b> \u00B7 <b>" + formatUsdNanosSigned(a.usdNanos, false) + "</b>";
    if (a.txs > 1) msg += " \u00B7 \u00D7" + std::to_string(a.txs);
    msg += "\n";
    const bool trade = a.kind == BtcAlert::BUY || a.kind == BtcAlert::SELL;
    if (trade && a.priceNanos > 0)
        msg += std::string(dm) + "\U0001F4B5 " + tr(lang, a.kind == BtcAlert::BUY ? "alert_buy_price" : "alert_sell_price") +
               ": <b>" + formatPriceUsd(cpp_int(a.priceNanos)) + "</b>\n";
    msg += std::string(dm) + "\U0001F4E6 " + tr(lang, "alert_qty") + ": <b>" + btcQty(a.sats) + " BTC</b>\n";
    if (!a.ex.empty())
        msg += std::string(dm) + "\U0001F3E6 " + tr(lang, a.kind == BtcAlert::BUY ? "alert_from_exchange" : "alert_to_exchange") +
               ": <b>" + safeString(a.ex, 32) + "</b>\n";
    // Цена входа и PnL — подписчику; бесплатному — строка, что они есть.
    const bool hasAvg = a.avgEntryNanos > 0 && (a.kind == BtcAlert::SELL || a.priorBuys > 1);
    if (full && hasAvg)
        msg += std::string(dm) + "\U0001F4CA " + tr(lang, "alert_avg_entry") + ": <b>" +
               formatPriceUsd(cpp_int(a.avgEntryNanos)) + "</b>\n";
    if (full && a.hasPnl)
        msg += std::string(dm) + (a.pnlNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ") + tr(lang, "alert_trade_pnl") +
               ": <b>" + formatUsdNanosSigned(a.pnlNanos, true) + "</b> (" + formatPercent(a.pnlPct, true) + ")\n";
    const bool locked = !full && (hasAvg || a.hasPnl);
    if (locked) msg += std::string(dm) + "\U0001F512 " + tr(lang, "alert_locked") + "\n";
    msg += "\n" + std::string(dm) + "\U0001F517 <a href=\"https://mempool.space/tx/" + safeString(a.txid, 66) + "\">" +
           tr(lang, "alert_transaction") + "</a>";
    if (card) {
        json c;
        c["v"] = 1; c["k"] = "btc"; c["a"] = act; c["w"] = a.wallet; c["n"] = label;
        c["sym"] = "BTC"; c["usd"] = a.usdNanos / 1e9; c["qty"] = a.sats / 1e8; c["tx"] = a.txid;
        if (a.txs > 1) c["txs"] = a.txs;
        if (trade && a.priceNanos > 0) c["px"] = a.priceNanos / 1e9;
        if (!a.ex.empty()) c["ex"] = a.ex;
        if (full && hasAvg) c["avg"] = a.avgEntryNanos / 1e9;
        if (full && a.hasPnl) { c["pnl"] = a.pnlNanos / 1e9; c["pnlPct"] = a.pnlPct; }
        if (locked) c["lock"] = true;
        *card = std::move(c);
    }
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
    for (auto& [labelLang, chatIds] : byLabelLang) {
        const std::set<std::string> prem = premiumSubsetOf(chatIds);
        std::vector<std::string> paid, free;
        for (const auto& c : chatIds) (prem.count(c) ? paid : free).push_back(c);
        if (!paid.empty()) {
            json card;
            const std::string m = buildBtcAlertMessage(labelLang.first, a, labelLang.second, true, &card);
            if (g_msgQueue.enqueueToRecipients(m, paid, "", card.dump())) anySent = true;
        }
        if (!free.empty()) {
            json card;
            const std::string m = buildBtcAlertMessage(labelLang.first, a, labelLang.second, false, &card);
            if (g_msgQueue.enqueueToRecipients(m, free, freeAlertKeyboard(m, labelLang.second), card.dump())) anySent = true;
        }
    }
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

/* Автопополнение базы сервисного аккаунта на BSC — как у биткоина
   (autoWatch в btc_chain.cpp): кто крупно вывел с биржи и не похож на
   сервис, остаётся в базе сам.
   • Вывод с биржи: отправитель — горячий кошелёк (порядковый номер его
     транзакций, nonce, от BSC_HOT_NONCE; видно прямо в блоке, без запросов),
     сама транзакция — перевод BNB или transfer() стейблкоина.
   • Крупный: от autoMinUsd(BSC) — по умолчанию $10 тыс. (WHALE_BSC_AUTO_MIN).
   • Не больше 500 новых в сутки, и поиск можно остановить: /autobase.
   • Не сервис: получатель — обычный кошелёк, не контракт, и у него меньше
     BSC_AUTO_MAX_NONCE исходящих транзакций (биржи и боты — миллионы).
   Разбор блока только складывает кандидатов; проверка (два запроса к RPC)
   идёт отдельным потоком и сканер не задерживает. Кошельки, которые за
   месяц не сделали ни одной сделки, убирает pruneAutoWallets. */
constexpr long long BSC_HOT_NONCE = 300000;
constexpr long long BSC_AUTO_MAX_NONCE = 1000;
constexpr size_t BSC_AUTO_QUEUE_MAX = 500;
constexpr size_t BSC_AUTO_SEEN_MAX = 200000;
std::mutex g_bscAutoMutex;
std::vector<std::string> g_bscAutoQueue;
std::unordered_set<std::string> g_bscAutoSeen;

static long double hexToLD(const std::string& h, size_t from = 0, size_t len = std::string::npos) {
    long double v = 0;
    const size_t end = len == std::string::npos ? h.size() : std::min(h.size(), from + len);
    for (size_t i = from; i < end; i++) {
        const char c = h[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) continue;
        v = v * 16 + d;
    }
    return v;
}

static void bscAutoConsider(const nlohmann::json& tx, const std::string& to,
                            const std::unordered_map<std::string, std::vector<Watcher>>* watchers) {
    static const bool onBsc = chainCtx().coingeckoPlatform == "binance-smart-chain";
    if (!onBsc || to.empty() || !tx.contains("nonce") || !tx["nonce"].is_string()) return;
    // Сначала дешёвые проверки — они отсекают почти все транзакции блока.
    const std::string nonceHex = tx["nonce"].get<std::string>();
    if (nonceHex.size() < 7) return;  // не больше четырёх знаков (< 0x10000) — точно не горячий кошелёк
    if (hexToLD(nonceHex, 2) < BSC_HOT_NONCE) return;
    if (!autoRoom(AutoNet::BSC)) return;  // поиск выключен или лимит дня выбран
    const std::string input = tx.contains("input") && tx["input"].is_string() ? tx["input"].get<std::string>() : "";
    std::string rcpt;
    long double usd = 0;
    if (input == "0x" || input.empty()) {
        const std::string val = tx.contains("value") && tx["value"].is_string() ? tx["value"].get<std::string>() : "0x0";
        const long double bnb = hexToLD(val, 2) / 1e18L;
        if (bnb <= 0) return;
        const uint64_t px = nativePriceCachedNanos();
        if (!px) return;
        usd = bnb * static_cast<long double>(px) / 1e9L;
        rcpt = to;
    } else if (input.size() >= 138 && input.compare(0, 10, "0xa9059cbb") == 0 && chainCtx().stablecoins.count(to)) {
        // transfer(address,uint256): адрес — последние 40 знаков первого
        // слова, сумма — второе слово. У стейблкоинов BSC 18 знаков.
        rcpt = "0x" + toLower(input.substr(34, 40));
        usd = hexToLD(input, 74, 64) / 1e18L;
    } else {
        return;
    }
    if (usd < static_cast<long double>(autoMinUsd(AutoNet::BSC)) || rcpt.size() != 42) return;
    if (watchers && watchers->count(rcpt)) return;
    std::lock_guard<std::mutex> l(g_bscAutoMutex);
    if (g_bscAutoQueue.size() >= BSC_AUTO_QUEUE_MAX) return;
    if (g_bscAutoSeen.size() >= BSC_AUTO_SEEN_MAX) g_bscAutoSeen.clear();
    if (!g_bscAutoSeen.insert(rcpt).second) return;
    g_bscAutoQueue.push_back(rcpt);
}

void bscAutoLoop() {
    long long pending = 0;
    auto lastRefresh = std::chrono::steady_clock::now();
    while (running.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 50 && running.load(std::memory_order_relaxed); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::vector<std::string> batch;
        {
            std::lock_guard<std::mutex> l(g_bscAutoMutex);
            while (!g_bscAutoQueue.empty() && batch.size() < 10) {
                batch.push_back(g_bscAutoQueue.back());
                g_bscAutoQueue.pop_back();
            }
        }
        // Не проверен из-за сбоя (сеть, база) — забыть, чтобы следующий
        // крупный вывод проверил его снова. Отказ по делу (контракт, сервис,
        // бот) — помнить: второй раз спрашивать незачем.
        auto retryLater = [](const std::string& a) {
            std::lock_guard<std::mutex> l(g_bscAutoMutex);
            g_bscAutoSeen.erase(a);
        };
        for (size_t i = 0; i < batch.size(); i++) {
            const std::string& a = batch[i];
            if (!autoRoom(AutoNet::BSC)) {
                // Лимит дня выбран или поиск выключили — непроверенных
                // вернуть в очередь, а не потерять.
                std::lock_guard<std::mutex> l(g_bscAutoMutex);
                for (size_t j = i; j < batch.size(); j++) g_bscAutoQueue.push_back(batch[j]);
                break;
            }
            if (isPermanentlyBanned(a)) continue;
            auto code = rpc("eth_getCode", {a, "latest"});
            if (!code.is_string()) { retryLater(a); continue; }
            if (code.get<std::string>() != "0x") continue;  // контракт
            auto cnt = rpc("eth_getTransactionCount", {a, "latest"});
            long long n = 0;
            if (!cnt.is_string() || !hexToLL(cnt.get<std::string>(), n)) { retryLater(a); continue; }
            if (n >= BSC_AUTO_MAX_NONCE) continue;
            const AddWhaleResult r = addUserWhale(SERVICE_CHAT_ID, a, "auto-bsc");
            if (r == AddWhaleResult::ERROR) retryLater(a);
            if (r == AddWhaleResult::OK) {
                autoCounted(AutoNet::BSC);
                std::cout << "[BSC] в базу сервисного аккаунта: " << a << " (транзакций " << n << ")" << std::endl;
                ++pending;
            }
        }
        // Список отслеживаемых — раз в минуту, если что-то добавилось.
        if (pending > 0 && std::chrono::steady_clock::now() - lastRefresh >= std::chrono::seconds(60)) {
            refreshWatchers();
            pending = 0;
            lastRefresh = std::chrono::steady_clock::now();
        }
    }
}

/* Неактивные автокошельки убираются из базы, но не банятся: кто 30 дней
   (AUTO_IDLE_SEC) не торговал — BSC без сделок в trades, Hyperliquid без
   сделок в hl_fills, Bitcoin без движений в btc_moves — тот уходит из базы
   сервисного аккаунта, а поиск его «забывает», чтобы снова добавить, когда
   кошелёк вернётся к крупной торговле. Смотрим только на тех, кто в базе
   дольше AUTO_IDLE_SEC: свежему ещё не было когда поторговать. У людей,
   следящих за адресом, он остаётся.
   Боты — другое дело: их банит разбор сделок (ignored_wallets на BSC,
   hl_banned на Hyperliquid) навсегда, и ни импорт, ни поиск их не вернут. */
constexpr long long AUTO_IDLE_SEC = 30LL * 86400LL;

void pruneAutoWallets() {
    const long long cut = static_cast<long long>(time(nullptr)) - AUTO_IDLE_SEC;
    struct W { long long id; std::string addr; };
    std::vector<W> bsc, hlc;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s,
                "SELECT uw.whale_id, lower(wa.address) FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id "
                "WHERE uw.user_id=? AND uw.label='auto-bsc' AND uw.created_at>0 AND uw.created_at<? "
                "AND NOT EXISTS (SELECT 1 FROM trades t WHERE t.wallet=lower(wa.address) AND t.timestamp>=?)")) {
            sqlite3_bind_text(s, 1, SERVICE_CHAT_ID.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, cut);
            sqlite3_bind_int64(s, 3, cut);
            while (sqlite3_step(s) == SQLITE_ROW) bsc.push_back({sqlite3_column_int64(s, 0), safeColumnText(s, 1)});
            sqlite3_finalize(s);
        }
        if (prepareOrLog(db, &s,
                "SELECT uw.whale_id, lower(wa.address) FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id "
                "WHERE uw.user_id=? AND uw.label='auto-hl' AND uw.created_at>0 AND uw.created_at<?")) {
            sqlite3_bind_text(s, 1, SERVICE_CHAT_ID.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, cut);
            while (sqlite3_step(s) == SQLITE_ROW) hlc.push_back({sqlite3_column_int64(s, 0), safeColumnText(s, 1)});
            sqlite3_finalize(s);
        }
    }
    // Hyperliquid: сделки в своей базе, время — в миллисекундах.
    std::vector<W> hl;
    {
        std::lock_guard<std::mutex> l(hl::g_hlDbMutex);
        sqlite3_stmt* s = nullptr;
        if (hl::g_hlDb && !hlc.empty() &&
            // wallet=? (адреса пишутся строчными), а не lower(wallet)=?: так
            // идёт индекс (wallet, ts), а не перебор всех сделок за месяц на
            // каждый из тысяч кошельков.
            prepareOrLog(hl::g_hlDb, &s, "SELECT 1 FROM hl_fills WHERE wallet=? AND ts>=? LIMIT 1")) {
            for (const auto& w : hlc) {
                sqlite3_reset(s);
                sqlite3_bind_text(s, 1, w.addr.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(s, 2, cut * 1000);
                if (sqlite3_step(s) != SQLITE_ROW) hl.push_back(w);
            }
            sqlite3_finalize(s);
        }
    }
    int removed = 0;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s, "DELETE FROM user_whales WHERE user_id=? AND label=? AND whale_id=?")) {
            for (const auto* list : {&bsc, &hl}) {
                const char* label = list == &bsc ? "auto-bsc" : "auto-hl";
                for (const auto& w : *list) {
                    sqlite3_reset(s);
                    sqlite3_bind_text(s, 1, SERVICE_CHAT_ID.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(s, 2, label, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(s, 3, w.id);
                    if (sqlite3_step(s) == SQLITE_DONE) removed += sqlite3_changes(db);
                }
            }
            sqlite3_finalize(s);
        }
    }
    // Поиск их «забывает»: вернутся к крупной торговле — добавит снова.
    {
        std::lock_guard<std::mutex> l(g_bscAutoMutex);
        for (const auto& w : bsc) g_bscAutoSeen.erase(w.addr);
    }
    for (const auto& w : hl) hlAutoForget(w.addr);
    const int btc = btcPruneAuto(cut);
    if (removed > 0 || btc > 0) {
        std::cout << "[AUTO] убраны неактивные 30 дней (не баним): BSC " << bsc.size() << ", Hyperliquid " << hl.size()
                  << ", Bitcoin " << btc << std::endl;
        if (removed > 0) refreshWatchers();
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
        // Крупный вывод с биржи — кандидат в базу (без запросов к сети).
        bscAutoConsider(tx, to, watchers.get());
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
/* Уведомление о запуске токена — тем, кто нажал «Сообщить о запуске» в
   приложении (token_subs). Без аргумента — предпросмотр владельцу и число
   получателей, ничего не уходит. «go» — разослать готовый текст на языке
   каждого (tk_ready): подробности — токеномика, контракт, сеть, дата — живут
   в приложении, в разделе «Токен проекта», куда ведёт кнопка. «go <текст>» —
   разослать свой текст как есть (на одном языке). */
void tokenCast(const std::string& owner, const std::string& arg) {
    std::vector<std::string> subs;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s, "SELECT chat_id FROM token_subs ORDER BY at")) {
            while (sqlite3_step(s) == SQLITE_ROW) subs.push_back(safeColumnText(s, 0));
            sqlite3_finalize(s);
        }
    }
    const bool go = arg == "go" || arg.rfind("go ", 0) == 0;
    const std::string custom = arg.size() > 3 && go ? trim(arg.substr(3)) : "";
    const Lang ol = langFromCode(getUserLanguage(owner));
    if (!go) {
        sendMsg(owner, "🪙 <b>Рассылка о токене</b>\nПолучателей: <b>" + std::to_string(subs.size()) +
                       "</b>\n\nТак увидят (ваш язык):\n\n" + tr(ol, "tk_ready") +
                       "\n\nОтправить всем: <code>/tokencast go</code>\nСвоим текстом: <code>/tokencast go текст</code>",
                openAppKeyboard(ol, "token", "tk_open_btn"));
        return;
    }
    if (subs.empty()) {
        sendMsg(owner, "🪙 Подписавшихся на токен пока нет — отправлять некому.");
        return;
    }
    sendMsg(owner, "🪙 Рассылка о токене началась: " + std::to_string(subs.size()) + " получателей.");
    std::thread([subs, custom, owner]() {
        size_t ok = 0;
        for (const auto& chat : subs) {
            const Lang lang = langFromCode(getUserLanguage(chat));
            if (sendMsg(chat, custom.empty() ? tr(lang, "tk_ready") : custom,
                        openAppKeyboard(lang, "token", "tk_open_btn")).ok) ++ok;
            // Telegram пропускает около 30 сообщений в секунду.
            std::this_thread::sleep_for(std::chrono::milliseconds(45));
        }
        sendMsg(owner, "🪙 Рассылка о токене закончена: доставлено " + std::to_string(ok) + " из " +
                       std::to_string(subs.size()) + ".");
    }).detach();
}

/* Бонус за регистрацию на бирже по нашей ссылке. Заявку (UID человека)
   кладёт в exch_claims API приложения и шлёт владельцу сообщение с кнопками
   «ex:ok:<id>» / «ex:no:<id>». Здесь — только решение владельца: status
   меняется один раз (wait → ok / no). Дни и сообщение человеку — за фоновым
   проходом API (exch_settle): там начисление идёт одной транзакцией с
   отметкой о нём. */
struct ExchClaim { long long id = 0; std::string chat, ex, uid, status; };

static bool loadExchClaim(long long id, ExchClaim& c) {
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "SELECT id, chat_id, ex, uid, status FROM exch_claims WHERE id=?")) return false;
    sqlite3_bind_int64(s, 1, id);
    bool ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok) {
        c.id = sqlite3_column_int64(s, 0);
        c.chat = safeColumnText(s, 1);
        c.ex = safeColumnText(s, 2);
        c.uid = safeColumnText(s, 3);
        c.status = safeColumnText(s, 4);
    }
    sqlite3_finalize(s);
    return ok;
}

static std::string exchName(const std::string& ex) { return ex == "okx" ? "OKX" : ex; }

static std::string exchClaimText(const ExchClaim& c) {
    std::string t = "\U0001F3E6 <b>" + escapeHtml(exchName(c.ex)) + ": заявка #" + std::to_string(c.id) + "</b>\n"
                    "UID: <code>" + escapeHtml(c.uid) + "</code>\n";
    if (!c.chat.empty())
        t += "Пользователь: <a href=\"tg://user?id=" + escapeHtml(c.chat) + "\">" + escapeHtml(c.chat) + "</a>\n";
    if (c.status == "ok") t += "\n✅ <b>Одобрено</b> — дни Премиума придут человеку в течение минуты.";
    else if (c.status == "no") t += "\n❌ <b>Отклонено</b> — человеку придёт сообщение.";
    else t += "\nПроверьте в кабинете партнёра: UID среди приглашённых, депозит и покупка от 200 €.";
    return t;
}

static std::string exchClaimKeyboard(long long id) {
    json kb;
    kb["inline_keyboard"] = json::array({json::array({
        {{"text", "✅ Одобрить"}, {"callback_data", "ex:ok:" + std::to_string(id)}},
        {{"text", "❌ Отклонить"}, {"callback_data", "ex:no:" + std::to_string(id)}}})});
    return kb.dump();
}

/* Кнопка под заявкой. true — нажатие разобрано (чужое или нет). */
bool handleExchCallback(const json& cq) {
    const std::string data = cq.contains("data") && cq["data"].is_string() ? cq["data"].get<std::string>() : "";
    if (data.rfind("ex:", 0) != 0) return false;
    const std::string qid = cq.contains("id") && cq["id"].is_string() ? cq["id"].get<std::string>() : "";
    const std::string from = cq.contains("from") && cq["from"].is_object() && cq["from"].contains("id") &&
                             cq["from"]["id"].is_number_integer()
                             ? std::to_string(cq["from"]["id"].get<long long>()) : "";
    // Решает только владелец: кнопку из пересланного сообщения нажать может
    // кто угодно.
    if (from != OWNER_CHAT_ID) {
        if (!qid.empty()) answerCallbackQuery(qid);
        return true;
    }
    const bool approve = data.rfind("ex:ok:", 0) == 0;
    if (!approve && data.rfind("ex:no:", 0) != 0) {
        if (!qid.empty()) answerCallbackQuery(qid);
        return true;
    }
    long long id = 0;
    try { id = std::stoll(data.substr(6)); } catch (...) { id = 0; }
    ExchClaim c;
    int changed = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (id > 0 && prepareOrLog(db, &s,
                "UPDATE exch_claims SET status=?, decided_at=? WHERE id=? AND status='wait' "
                // Один UID — одна награда: второй раз тот же UID не одобрить.
                "AND (?1 != 'ok' OR NOT EXISTS (SELECT 1 FROM exch_claims o WHERE o.ex=exch_claims.ex "
                "AND o.uid=exch_claims.uid AND o.status='ok' AND o.id!=exch_claims.id))")) {
            sqlite3_bind_text(s, 1, approve ? "ok" : "no", -1, SQLITE_STATIC);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)time(nullptr));
            sqlite3_bind_int64(s, 3, id);
            if (sqlite3_step(s) == SQLITE_DONE) changed = sqlite3_changes(db);
            sqlite3_finalize(s);
        }
        found = id > 0 && loadExchClaim(id, c);
    }
    if (!found) {
        if (!qid.empty()) answerCallbackQuery(qid, "Заявка не найдена");
        return true;
    }
    // Не изменилась, а всё ещё ждёт — значит, этот UID уже одобрен в другой
    // заявке: эту владелец может только отклонить.
    const bool dupUid = !changed && c.status == "wait";
    if (!qid.empty())
        answerCallbackQuery(qid, changed ? (approve ? "Одобрено" : "Отклонено")
                                 : dupUid ? "Этот UID уже получил награду — одобрить нельзя"
                                 : (c.status == "ok" ? "Уже одобрено" : "Уже отклонено"));
    if (dupUid) return true;
    // Сообщение с заявкой — с итогом и без кнопок, чтобы не нажать дважды.
    if (cq.contains("message") && cq["message"].is_object() && cq["message"].contains("message_id") &&
        cq["message"].contains("chat") && cq["message"]["chat"].is_object() &&
        cq["message"]["chat"].contains("id")) {
        json j;
        j["chat_id"] = cq["message"]["chat"]["id"];
        j["message_id"] = cq["message"]["message_id"];
        j["text"] = exchClaimText(c);
        j["parse_mode"] = "HTML";
        j["disable_web_page_preview"] = true;
        http(tgApi("editMessageText"), j.dump());
    }
    std::cout << "[EXCH] #" << id << " " << c.ex << " uid " << c.uid << " chat " << c.chat
              << " -> " << c.status << (changed ? "" : " (already)") << std::endl;
    return true;
}

/* /okx — заявки, ждущие решения: каждая отдельным сообщением с кнопками
   (на случай, если первое сообщение не дошло или затерялось). */
void exchPending(const std::string& owner) {
    std::vector<ExchClaim> rows;
    long long nOk = 0, nNo = 0;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s, "SELECT id, chat_id, ex, uid, status FROM exch_claims "
                                 "WHERE status='wait' ORDER BY id LIMIT 20")) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                ExchClaim c;
                c.id = sqlite3_column_int64(s, 0);
                c.chat = safeColumnText(s, 1);
                c.ex = safeColumnText(s, 2);
                c.uid = safeColumnText(s, 3);
                c.status = safeColumnText(s, 4);
                rows.push_back(c);
            }
            sqlite3_finalize(s);
        }
        if (prepareOrLog(db, &s, "SELECT SUM(status='ok'), SUM(status='no') FROM exch_claims")) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                nOk = sqlite3_column_int64(s, 0);
                nNo = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
    }
    sendMsg(owner, "\U0001F3E6 <b>Заявки с бирж</b>\nЖдут решения: <b>" + std::to_string(rows.size()) +
                   (rows.size() >= 20 ? "+" : "") + "</b> · одобрено: " + std::to_string(nOk) +
                   " · отклонено: " + std::to_string(nNo));
    for (const auto& c : rows) {
        sendMsg(owner, exchClaimText(c), exchClaimKeyboard(c.id));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
}

/* Каналы партнёров — взаимная реклама с блогерами. Владелец заводит канал
   (/partner add @канал 2): в «Бонусах» приложения появляется «подпишись —
   +2 дня» (дни через 3 дня, если человек ещё подписан; проверяет и начисляет
   API, partner_settle). Блогер получает ссылку t.me/<бот>?startapp=p_<канал>:
   кто пришёл по ней — в partner_refs. /partner — каналы и цифры по каждому. */
static json tgCall(const std::string& method, const json& body) {
    try {
        auto p = json::parse(http(tgApi(method), body.dump()));
        if (p.value("ok", false) && p.contains("result")) return p["result"];
    } catch (...) {}
    return nullptr;
}

static std::string botUsername() {
    static std::string name;
    if (name.empty()) {
        json me = tgCall("getMe", json::object());
        if (me.is_object() && me.contains("username") && me["username"].is_string())
            name = me["username"].get<std::string>();
    }
    return name;
}

// Бот — админ канала? Без этого подписку не проверить. -1 — канал недоступен.
static int botIsAdmin(const std::string& handle) {
    const std::string botId = TG_TOKEN.substr(0, TG_TOKEN.find(':'));
    json m;
    try { m = tgCall("getChatMember", {{"chat_id", handle}, {"user_id", std::stoll(botId)}}); } catch (...) { return -1; }
    if (!m.is_object()) return -1;
    const std::string st = m.value("status", "");
    return st == "administrator" || st == "creator" ? 1 : 0;
}

static std::string partnerLink(const std::string& slug) {
    const std::string bot = botUsername();
    return bot.empty() ? "(имя бота не получено)" : "https://t.me/" + bot + "?startapp=p_" + slug;
}

static void partnerList(const std::string& owner) {
    struct Ch { std::string handle, title, slug; long long days = 0, active = 0, subs = 0, ok = 0, wait = 0, left = 0, refs = 0, paid = 0; };
    std::vector<Ch> rows;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s,
                "SELECT c.handle, c.title, c.slug, c.days, c.active, "
                "(SELECT COUNT(*) FROM partner_claims p WHERE p.handle=c.handle), "
                "(SELECT COUNT(*) FROM partner_claims p WHERE p.handle=c.handle AND p.status IN ('ok','cap')), "
                "(SELECT COUNT(*) FROM partner_claims p WHERE p.handle=c.handle AND p.status='wait'), "
                "(SELECT COUNT(*) FROM partner_claims p WHERE p.handle=c.handle AND p.status='left'), "
                "(SELECT COUNT(*) FROM partner_refs r WHERE r.slug=c.slug), "
                "(SELECT COUNT(*) FROM partner_refs r WHERE r.slug=c.slug AND ("
                "  EXISTS (SELECT 1 FROM premium_payments pp WHERE pp.chat_id=r.chat_id) OR "
                "  EXISTS (SELECT 1 FROM ton_invoices ti WHERE ti.chat_id=r.chat_id AND ti.status='paid'))) "
                "FROM partner_channels c ORDER BY c.active DESC, c.added_at")) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                Ch c;
                c.handle = safeColumnText(s, 0); c.title = safeColumnText(s, 1); c.slug = safeColumnText(s, 2);
                c.days = sqlite3_column_int64(s, 3); c.active = sqlite3_column_int64(s, 4);
                c.subs = sqlite3_column_int64(s, 5); c.ok = sqlite3_column_int64(s, 6);
                c.wait = sqlite3_column_int64(s, 7); c.left = sqlite3_column_int64(s, 8);
                c.refs = sqlite3_column_int64(s, 9); c.paid = sqlite3_column_int64(s, 10);
                rows.push_back(c);
            }
            sqlite3_finalize(s);
        }
    }
    if (rows.empty()) {
        sendMsg(owner, "🤝 <b>Каналы партнёров</b>\nПока нет ни одного.\n\n"
                       "Добавить: <code>/partner add @канал 2</code> — 2 дня Премиума за подписку.\n"
                       "Сначала блогер добавляет бота админом своего канала (права не нужны).");
        return;
    }
    std::string t = "🤝 <b>Каналы партнёров</b>\n";
    for (const auto& c : rows) {
        const int admin = c.active ? botIsAdmin(c.handle) : 1;
        t += "\n" + std::string(c.active ? "🟢 " : "⚪️ ") + "<b>" + escapeHtml(c.title.empty() ? c.handle : c.title) +
             "</b> " + escapeHtml(c.handle) + " · +" + std::to_string(c.days) + " дн." + (c.active ? "" : " · выключен") + "\n";
        if (admin == 0) t += "⚠️ бот не админ канала — подписку не проверить\n";
        if (admin < 0) t += "⚠️ канал недоступен боту\n";
        t += "Наши → к нему: подписались " + std::to_string(c.subs) + ", засчитано " + std::to_string(c.ok) +
             ", ждут проверки " + std::to_string(c.wait) + ", отписались " + std::to_string(c.left) + "\n";
        t += "Его → к нам: пришли " + std::to_string(c.refs) + ", из них платят " + std::to_string(c.paid) + "\n";
        t += "Ссылка для блогера: " + escapeHtml(partnerLink(c.slug)) + "\n";
    }
    t += "\nДобавить или поменять дни: <code>/partner add @канал 2</code>\nВыключить: <code>/partner off @канал</code>";
    sendMsg(owner, t);
}

static void partnerAdd(const std::string& owner, std::string handle, long long days) {
    if (!handle.empty() && handle[0] != '@') handle = "@" + handle;
    handle = toLower(handle);
    bool okName = handle.size() >= 5 && handle.size() <= 33;
    for (size_t i = 1; i < handle.size() && okName; i++)
        okName = std::isalnum(static_cast<unsigned char>(handle[i])) || handle[i] == '_';
    if (!okName || days < 1 || days > 7) {
        sendMsg(owner, "Использование: <code>/partner add @канал 2</code>\nДней — от 1 до 7.");
        return;
    }
    json chat = tgCall("getChat", {{"chat_id", handle}});
    if (!chat.is_object()) {
        sendMsg(owner, "❌ Канал " + escapeHtml(handle) + " не найден или закрыт. Нужен публичный канал с @именем.");
        return;
    }
    const std::string title = chat.value("title", handle);
    const std::string slug = handle.substr(1);
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s,
                "INSERT INTO partner_channels(handle, title, days, active, slug, added_at) VALUES(?,?,?,1,?,?) "
                "ON CONFLICT(handle) DO UPDATE SET title=excluded.title, days=excluded.days, active=1")) {
            sqlite3_bind_text(s, 1, handle.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, days);
            sqlite3_bind_text(s, 4, slug.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 5, static_cast<long long>(time(nullptr)));
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }
    const int admin = botIsAdmin(handle);
    std::string t = "✅ <b>" + escapeHtml(title) + "</b> " + escapeHtml(handle) + " — в «Бонусах»: +" +
                    std::to_string(days) + " дн. за подписку (дни через 3 дня, если человек ещё подписан).\n\n" +
                    "Ссылка для блогера на наше приложение:\n" + escapeHtml(partnerLink(slug)) +
                    "\nКто придёт по ней — видно в /partner.";
    if (admin != 1)
        t += "\n\n⚠️ Бот ещё не админ канала — проверка подписки не сработает. Пусть блогер добавит @" +
             escapeHtml(botUsername()) + " в администраторы канала (никакие права не нужны).";
    sendMsg(owner, t);
}

static void partnerOff(const std::string& owner, std::string handle) {
    if (!handle.empty() && handle[0] != '@') handle = "@" + handle;
    handle = toLower(handle);
    int changed = 0;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s, "UPDATE partner_channels SET active=0 WHERE handle=?")) {
            sqlite3_bind_text(s, 1, handle.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) == SQLITE_DONE) changed = sqlite3_changes(db);
            sqlite3_finalize(s);
        }
    }
    sendMsg(owner, changed ? "⚪️ " + escapeHtml(handle) + " выключен: из «Бонусов» убран. Уже подписавшимся "
                             "проверка через 3 дня всё равно пройдёт, цифры остаются в /partner."
                           : "Канала " + escapeHtml(handle) + " нет в списке. Список: /partner");
}

void partnerCommand(const std::string& owner, const std::string& arg) {
    std::istringstream in(arg);
    std::string sub, handle, days;
    in >> sub >> handle >> days;
    if (sub.empty()) return partnerList(owner);
    if (sub == "add") {
        long long d = 2;
        try { if (!days.empty()) d = std::stoll(days); } catch (...) { d = 0; }
        return partnerAdd(owner, handle, d);
    }
    if (sub == "off") return partnerOff(owner, handle);
    sendMsg(owner, "Команды: <code>/partner</code> · <code>/partner add @канал 2</code> · <code>/partner off @канал</code>");
}

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
        ss2 << funnelStatsLine();
        if (qs>1000) ss2 << "\n\n⚠️ <b>QUEUE HIGH!</b>";
        if (fc>0) ss2 << "\n⚠️ <b>FAILED DELIVERIES!</b>";
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
    else if (txt == "/partner" || txt.rfind("/partner ", 0) == 0) {
        partnerCommand(cid, trim(txt.substr(8)));
    }
    else if (txt == "/autorenew") {
        // Проверить и отменить автопродления звёздами, если какие-то остались.
        std::thread([cid]{ cancelStarSubscriptions(cid, true); }).detach();
    }
    else if (txt == "/autobase" || txt.rfind("/autobase ", 0) == 0) {
        autobaseCommand(cid, trim(txt.substr(9)));
    }
    else if (txt == "/okx") {
        exchPending(cid);
    }
    else if (txt.rfind("/tokencast", 0) == 0) {
        tokenCast(cid, trim(txt.substr(10)));
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
            auto raw=http(tgApi("getUpdates")+"?offset="+std::to_string(offset)+"&timeout=30&allowed_updates=%5B%22message%22%2C%22callback_query%22%2C%22pre_checkout_query%22%5D","",35);
            if (raw.empty()) continue;
            auto upd=json::parse(raw);
            if (!upd.contains("result")||!upd["result"].is_array()) continue;
            int ub=0;
            for (auto& u:upd["result"]) {
                if (!u.contains("update_id")) continue;
                offset=u["update_id"].get<long>()+1;
                if (++ub%5==0) saveTgOffset(offset);

                // Кнопки старых меню в истории чата: снять «часики» и
                // показать кнопку приложения.
                if (u.contains("callback_query")&&u["callback_query"].is_object()) {
                    const json& cq = u["callback_query"];
                    if (handleExchCallback(cq)) continue;
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
    if (!initPremium(SERVICE_CHAT_ID)) {
        std::cerr << "[STARTUP][FATAL] Premium schema init failed — payments are DISABLED for this run" << std::endl;
    }
    initLifecycle();
    initAutobase();
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
    // Автопродлений нет: подписки звёздами, оформленные раньше, отменяем при
    // запуске (отменять нечего — молча). Отдельным потоком: Telegram может
    // отвечать небыстро, а сканер ждать этого не должен.
    std::thread([]{ std::this_thread::sleep_for(std::chrono::seconds(5)); cancelStarSubscriptions(OWNER_CHAT_ID, false); }).detach();
    std::thread(bscAutoLoop).detach();
    g_msgQueue.start(); std::thread tg(telegramLoop); std::thread rk(rankingCacheLoop); std::thread af(alertFlushLoop); std::thread dm(dbMaintenanceLoop);
    auto lst=std::chrono::steady_clock::now(), lsq=std::chrono::steady_clock::now(), lcl=std::chrono::steady_clock::now();
    auto ltp=std::chrono::steady_clock::now();
    // Письма жизненного цикла и чистка истёкших подписок — уже в первом
    // проходе: бот перезапускают часто, и без этого письмо о конце пробной
    // недели могло ждать лишние полчаса. Повторов не будет: каждое письмо
    // отмечается в lifecycle_sent до отправки.
    auto llc=std::chrono::steady_clock::now()-std::chrono::minutes(10);
    auto lwc=std::chrono::steady_clock::now()-std::chrono::minutes(1);
    lcl=std::chrono::steady_clock::now()-std::chrono::minutes(30);
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
            // Приветствие новым — быстро: человек только что открыл приложение.
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-lwc).count()>=60) {
                welcomeTick();
                lwc=std::chrono::steady_clock::now();
            }
            // Письма жизненного цикла: конец пробной недели, продление, возврат.
            if (std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now()-llc).count()>=10) {
                lifecycleTick();
                llc=std::chrono::steady_clock::now();
            }
            if (std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now()-lcl).count()>=30) { cleanupOldAlerts(); cleanupOldTrades(); cleanupExpiredPremium(); pruneAutoWallets(); lcl=std::chrono::steady_clock::now(); }
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
