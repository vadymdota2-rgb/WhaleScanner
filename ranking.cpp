#include "ranking.h"

#include <sqlite3.h>
#include <mutex>
#include <map>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <atomic>
#include <thread>
#include <chrono>
#include "json.hpp"
#include "utils.h"
#include "ru.h"

std::string getUserLanguage(const std::string& chatId);

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;
extern std::atomic<bool> running;

namespace {
constexpr long long WINDOW_SECONDS = 30LL * 86400LL;
constexpr int MIN_COMPLETED_TRADES = 5;
constexpr int MAX_RANKED_WALLETS = 20;
constexpr int MIN_GLOBAL_COMPLETED_TRADES = 1;
constexpr int MAX_GLOBAL_RANKED = 100;

constexpr int MAX_BOT_FILTER_TRADES = 500;
constexpr int PER_PAGE = 5;
constexpr int GLOBAL_PER_PAGE = 5;
constexpr long long REBUILD_INTERVAL_SECONDS = 15 * 60;

const char* const CARD_SEPARATOR = "━━━━━━━━━━━━━━";

const char* const MENU_STRETCH =
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀";

sqlite3* g_rankingReadDb = nullptr;
std::mutex g_rankingReadMutex;

bool safeParseAmount(const std::string& amountStr, const std::string& context, cpp_int& out) {
    if (amountStr.empty()) {
        std::cerr << "[RANKING] Empty token_amount (" << context << ")" << std::endl;
        return false;
    }
    try {
        out = cpp_int(amountStr);
    } catch (const std::exception& e) {
        std::cerr << "[RANKING] Invalid token_amount \"" << amountStr << "\" (" << context
                  << "): " << e.what() << std::endl;
        return false;
    }
    if (out <= 0) {
        std::cerr << "[RANKING] Non-positive token_amount \"" << amountStr << "\" (" << context << ")" << std::endl;
        return false;
    }
    return true;
}
}

bool parseGlobalRankKind(const std::string& s, GlobalRankKind& out) {
    if (s == "pnl") { out = GlobalRankKind::PNL; return true; }
    if (s == "roi") { out = GlobalRankKind::ROI; return true; }
    if (s == "winrate") { out = GlobalRankKind::WIN_RATE; return true; }
    if (s == "active") { out = GlobalRankKind::ACTIVE; return true; }
    return false;
}

std::string globalRankKindToString(GlobalRankKind k) {
    switch (k) {
        case GlobalRankKind::PNL: return "pnl";
        case GlobalRankKind::ROI: return "roi";
        case GlobalRankKind::WIN_RATE: return "winrate";
        case GlobalRankKind::ACTIVE: return "active";
    }
    return "pnl";
}

void migrateTradesUniqueConstraint() {
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "SELECT sql FROM sqlite_master WHERE type='table' AND name='trades'")) return;
    bool tableExists = false;
    std::string existingSql;
    if (sqlite3_step(s) == SQLITE_ROW) { tableExists = true; existingSql = safeColumnText(s, 0); }
    sqlite3_finalize(s);

    if (!tableExists) return; // fresh DB — the CREATE TABLE below will already use the correct constraint
    if (existingSql.find("UNIQUE(wallet, tx_hash)") != std::string::npos) return; // already migrated

    std::cout << "[RANKING] Migrating trades table: tx_hash-only UNIQUE -> UNIQUE(wallet, tx_hash) "
                 "(needed for tracked-to-tracked transactions to record both sides)" << std::endl;
    const char* migrationSql = R"(
        ALTER TABLE trades RENAME TO trades_pre_migration;
        CREATE TABLE trades(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            wallet TEXT NOT NULL,
            token TEXT NOT NULL,
            is_buy INTEGER NOT NULL,
            usd_nanos INTEGER NOT NULL,
            token_amount TEXT NOT NULL,
            tx_hash TEXT,
            block_number INTEGER,
            timestamp INTEGER,
            UNIQUE(wallet, tx_hash)
        );
        INSERT INTO trades(wallet,token,is_buy,usd_nanos,token_amount,tx_hash,block_number,timestamp)
            SELECT wallet,token,is_buy,usd_nanos,token_amount,tx_hash,block_number,timestamp FROM trades_pre_migration;
        DROP TABLE trades_pre_migration;
    )";
    char* err = nullptr;
    if (sqlite3_exec(db, migrationSql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[RANKING][FATAL] trades unique-constraint migration failed: " << (err ? err : "") << std::endl;
        sqlite3_free(err);
    } else {
        std::cout << "[RANKING] trades table migrated successfully" << std::endl;
    }
}

void initRankingDB() {
    std::lock_guard<std::mutex> l(dbMutex);
    migrateTradesUniqueConstraint();
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS trades(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            wallet TEXT NOT NULL,
            token TEXT NOT NULL,
            is_buy INTEGER NOT NULL,
            usd_nanos INTEGER NOT NULL,
            token_amount TEXT NOT NULL,
            tx_hash TEXT,
            block_number INTEGER,
            timestamp INTEGER,
            UNIQUE(wallet, tx_hash)
        );
        CREATE INDEX IF NOT EXISTS idx_trades_token ON trades(token);
        DROP INDEX IF EXISTS idx_trades_wallet;
        CREATE INDEX IF NOT EXISTS idx_trades_wallet_time ON trades(wallet, timestamp);
        CREATE INDEX IF NOT EXISTS idx_trades_time ON trades(timestamp);
        CREATE INDEX IF NOT EXISTS idx_trades_token_time ON trades(token,timestamp);
        CREATE TABLE IF NOT EXISTS ignored_wallets(
            wallet TEXT PRIMARY KEY,
            ignored_until INTEGER NOT NULL,
            permanent INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS ranking_cache(
            cache_key TEXT PRIMARY KEY,
            payload TEXT NOT NULL,
            updated_at INTEGER NOT NULL
        );
    )";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[RANKING][FATAL] schema init failed: " << (err ? err : "") << std::endl;
        sqlite3_free(err);
    }

    {
        const char* alters[] = {
            "ALTER TABLE ignored_wallets ADD COLUMN permanent INTEGER NOT NULL DEFAULT 0",
        };
        for (const char* migSql : alters) {
            char* mErr = nullptr;
            if (sqlite3_exec(db, migSql, nullptr, nullptr, &mErr) != SQLITE_OK) {
                std::string e = mErr ? mErr : "";
                if (e.find("duplicate column") == std::string::npos)
                    std::cerr << "[RANKING][FATAL] migration failed: " << e << std::endl;
            }
            if (mErr) sqlite3_free(mErr);
        }
    }

    const char* fn = sqlite3_db_filename(db, "main");
    if (fn && *fn) {
        if (sqlite3_open_v2(fn, &g_rankingReadDb,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                            nullptr) == SQLITE_OK) {
            sqlite3_busy_timeout(g_rankingReadDb, 5000);
            std::cout << "[RANKING] Read-only connection opened" << std::endl;
        } else {
            std::cerr << "[RANKING] ⚠️ Read-only connection failed ("
                      << (g_rankingReadDb ? sqlite3_errmsg(g_rankingReadDb) : "open error")
                      << "), rankings will fall back to the shared connection" << std::endl;
            if (g_rankingReadDb) { sqlite3_close(g_rankingReadDb); g_rankingReadDb = nullptr; }
        }
    }
}

void closeRankingDB() {
    std::lock_guard<std::mutex> l(g_rankingReadMutex);
    if (g_rankingReadDb) {
        sqlite3_close(g_rankingReadDb);
        g_rankingReadDb = nullptr;
    }
}

void cleanupOldTrades() {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "DELETE FROM trades WHERE timestamp < ?")) return;
    sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(time(nullptr)) - WINDOW_SECONDS);
    int rc1 = sqlite3_step(s);
    int deleted = (rc1 == SQLITE_DONE) ? sqlite3_changes(db) : 0;
    if (rc1 != SQLITE_DONE) std::cerr << "[RANKING] trades cleanup DELETE failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(s);
    if (deleted > 0) std::cout << "[RANKING] Purged " << deleted << " trade(s) older than 30 days" << std::endl;

    if (prepareOrLog(db, &s, "DELETE FROM ignored_wallets WHERE permanent = 0 AND ignored_until <= ?")) {
        sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(time(nullptr)));
        int rc2 = sqlite3_step(s);
        int unblocked = (rc2 == SQLITE_DONE) ? sqlite3_changes(db) : 0;
        if (rc2 != SQLITE_DONE) std::cerr << "[RANKING] ignored_wallets cleanup DELETE failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s);
        if (unblocked > 0) std::cout << "[RANKING] Unblocked " << unblocked << " wallet(s)" << std::endl;
    }
}

namespace {

int64_t cppIntToClampedI64(const cpp_int& v) {
    static const cpp_int maxV(INT64_MAX);
    static const cpp_int minV(INT64_MIN);
    if (v > maxV) return INT64_MAX;
    if (v < minV) return INT64_MIN;
    return v.convert_to<int64_t>();
}

std::string formatUsdSigned(int64_t usdNanos) {
    bool neg = usdNanos < 0;
    int64_t dollars = usdNanos / 1000000000LL;
    if (neg) dollars = -dollars;
    std::string s = std::to_string(dollars);
    std::string out;
    int cnt = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (cnt != 0 && cnt % 3 == 0) out.push_back(',');
        out.push_back(*it);
        cnt++;
    }
    std::reverse(out.begin(), out.end());
    return (neg ? "-$" : "+$") + out;
}

std::string formatPercentSigned(double pct) {
    long long rounded = static_cast<long long>(pct >= 0 ? pct + 0.5 : pct - 0.5);
    std::string sign = rounded >= 0 ? "+" : "-";
    return sign + std::to_string(rounded < 0 ? -rounded : rounded) + "%";
}

std::string rankLabel(int rank) {
    switch (rank) {
        case 1: return "🥇 #1";
        case 2: return "🥈 #2";
        case 3: return "🥉 #3";
        default: return "#" + std::to_string(rank);
    }
}

std::string formatPercentPlain(double pct) {
    long long rounded = static_cast<long long>(pct >= 0 ? pct + 0.5 : pct - 0.5);
    return (rounded < 0 ? "-" : "") + std::to_string(rounded < 0 ? -rounded : rounded) + "%";
}

struct PnlRow {
    std::string wallet;
    int64_t pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int completedTrades = 0;
};

std::vector<PnlRow> computeTopPnl(const std::string& token, bool& ok) {
    ok = false;
    std::vector<PnlRow> results;
    long long since = static_cast<long long>(time(nullptr)) - WINDOW_SECONDS;

    std::string curWallet;
    cpp_int heldQty = 0;
    cpp_int heldCost = 0;
    cpp_int realizedPnl = 0;
    cpp_int totalCostDeployed = 0;
    int completedTrades = 0;
    int winningTrades = 0;

    auto flush = [&]() {
        if (!curWallet.empty() && completedTrades >= MIN_COMPLETED_TRADES &&
            completedTrades <= MAX_BOT_FILTER_TRADES) {
            PnlRow row;
            row.wallet = curWallet;
            row.pnlNanos = cppIntToClampedI64(realizedPnl);
            double costD = totalCostDeployed > 0 ? totalCostDeployed.convert_to<double>() : 0.0;
            double pnlD = realizedPnl.convert_to<double>();
            row.roiPercent = costD > 0.0 ? (100.0 * pnlD / costD) : 0.0;
            row.winRatePercent = completedTrades > 0
                ? static_cast<int>(100.0 * winningTrades / completedTrades + 0.5)
                : 0;
            row.completedTrades = completedTrades;
            results.push_back(row);
        }
    };

    sqlite3* rdb = g_rankingReadDb ? g_rankingReadDb : db;
    std::unique_lock<std::mutex> readLock, writeLock;
    if (g_rankingReadDb) readLock = std::unique_lock<std::mutex>(g_rankingReadMutex);
    else                 writeLock = std::unique_lock<std::mutex>(dbMutex);

    sqlite3_stmt* s;
    if (!prepareOrLog(rdb, &s,
        "SELECT t.wallet, t.is_buy, t.usd_nanos, t.token_amount FROM trades t "
        "WHERE t.token=? AND t.timestamp>=? "
        "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw WHERE iw.wallet=t.wallet AND iw.permanent=1) "
        "ORDER BY t.wallet ASC, t.timestamp ASC, t.id ASC")) return results;
    sqlite3_bind_text(s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, since);

    int stepRc;
    while ((stepRc = sqlite3_step(s)) == SQLITE_ROW) {
        std::string wallet = safeColumnText(s, 0);
        bool isBuy = sqlite3_column_int(s, 1) != 0;
        int64_t usdNanos = sqlite3_column_int64(s, 2);
        std::string amountStr = safeColumnText(s, 3);
        cpp_int amount;
        if (!safeParseAmount(amountStr, "wallet=" + wallet + " token=" + token, amount)) {
            sqlite3_finalize(s);
            std::cerr << "[RANKING] computeTopPnl(" << token << ") aborted: corrupted token_amount" << std::endl;
            return results;
        }

        if (wallet != curWallet) {
            flush();
            curWallet = wallet;
            heldQty = 0; heldCost = 0; realizedPnl = 0; totalCostDeployed = 0;
            completedTrades = 0; winningTrades = 0;
        }

        if (isBuy) {
            heldQty += amount;
            heldCost += cpp_int(usdNanos);
        } else if (heldQty > 0 && amount > 0) {
            cpp_int matchedQty = amount < heldQty ? amount : heldQty;
            cpp_int costOfMatched = (heldCost * matchedQty) / heldQty;
            cpp_int proceedsMatched = (cpp_int(usdNanos) * matchedQty) / amount;

            realizedPnl += (proceedsMatched - costOfMatched);
            totalCostDeployed += costOfMatched;
            completedTrades++;
            if (proceedsMatched > costOfMatched) winningTrades++;

            heldQty -= matchedQty;
            heldCost -= costOfMatched;
        }

    }
    sqlite3_finalize(s);
    if (stepRc != SQLITE_DONE) {
        std::cerr << "[RANKING] computeTopPnl(" << token << ") interrupted mid-read: "
                  << sqlite3_errmsg(rdb) << std::endl;
        return results;
    }
    ok = true;
    flush();

    std::sort(results.begin(), results.end(), [](const PnlRow& a, const PnlRow& b) {
        if (a.pnlNanos != b.pnlNanos) return a.pnlNanos > b.pnlNanos;
        if (a.roiPercent != b.roiPercent) return a.roiPercent > b.roiPercent;
        return a.completedTrades > b.completedTrades;
    });
    if (results.size() > static_cast<size_t>(MAX_RANKED_WALLETS)) results.resize(MAX_RANKED_WALLETS);
    return results;
}

std::mutex g_cacheMutex;
std::map<std::string, std::string> g_lastTokenByChat;
std::atomic<bool> g_forceRebuild{false};

std::string rowsToJson(const std::vector<PnlRow>& rows) {
    json a = json::array();
    for (const PnlRow& r : rows) {
        a.push_back({{"w", r.wallet}, {"p", r.pnlNanos}, {"r", r.roiPercent},
                     {"wr", r.winRatePercent}, {"t", r.completedTrades}});
    }
    return a.dump();
}

bool rowsFromJson(const std::string& payload, std::vector<PnlRow>& out) {
    try {
        json a = json::parse(payload);
        if (!a.is_array()) return false;
        std::vector<PnlRow> tmp;
        tmp.reserve(a.size());
        for (auto& e : a) {
            PnlRow r;
            r.wallet = e.value("w", "");
            r.pnlNanos = e.value("p", static_cast<int64_t>(0));
            r.roiPercent = e.value("r", 0.0);
            r.winRatePercent = e.value("wr", 0);
            r.completedTrades = e.value("t", 0);
            tmp.push_back(r);
        }
        out = std::move(tmp);
        return true;
    } catch (...) { return false; }
}

bool loadCachedPayload(const std::string& key, std::string& out) {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "SELECT payload FROM ranking_cache WHERE cache_key=?")) return false;
    sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    int stepRc = sqlite3_step(s);
    if (stepRc == SQLITE_ROW) { out = safeColumnText(s, 0); found = true; }
    else if (stepRc != SQLITE_DONE) {
        std::cerr << "[RANKING] loadCachedPayload(" << key << ") read error (not a genuine miss): "
                  << sqlite3_errmsg(db) << std::endl;
    }
    sqlite3_finalize(s);
    return found;
}

bool cacheReady() {
    std::string tmp;
    return loadCachedPayload("global_pnl", tmp);
}

RankingMessage buildGeneratingMessage(Lang lang) {
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));
    return {tr(lang, "rk_generating"), keyboard.dump()};
}

RankingMessage renderPage(const std::string& token, const std::vector<PnlRow>& rows, int page, Lang lang) {
    int totalPages = std::max(1, static_cast<int>((rows.size() + PER_PAGE - 1) / PER_PAGE));
    page = std::max(1, std::min(page, totalPages));
    int startIdx = (page - 1) * PER_PAGE;
    int endIdx = std::min(static_cast<int>(rows.size()), startIdx + PER_PAGE);

    std::stringstream text;

text << "🏆 <b>" << tr(lang, "rk_top_pnl_30d") << "</b>\n\n";
text << "📄 <b>" << tr(lang, "rk_smart_contract") << "</b>\n";
text << "<code>" << safeString(token, 42) << "</code>\n\n";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    if (rows.empty()) {
        text << "📊 " << tr(lang, "rk_no_wallets_min_trades1") << " " << MIN_COMPLETED_TRADES
             << " " << tr(lang, "rk_no_wallets_min_trades2");
    } else {
        for (int i = startIdx; i < endIdx; i++) {
            const PnlRow& r = rows[i];
            int rank = i + 1;
            text << rankLabel(rank) << "\n";
            text << "<code>" << safeString(r.wallet, 42) << "</code>\n\n";
            text << "💵 <b>PnL:</b> " << formatUsdSigned(r.pnlNanos) << "\n";
            text << "📈 <b>ROI:</b> " << formatPercentSigned(r.roiPercent) << "\n";
            text << "🎯 <b>" << tr(lang, "ws_winrate") << ":</b> " << r.winRatePercent << "%\n";
            text << "🔄 <b>" << tr(lang, "rk_trades") << ":</b> " << r.completedTrades << "\n";
            if (i + 1 < endIdx) text << "\n" << CARD_SEPARATOR << "\n\n";

            json row;
            row.push_back({{"text", tr(lang, "rk_track") + " #" + std::to_string(rank)}, {"callback_data", "tt_track:" + r.wallet}});
            row.push_back({{"text", "🔍 " + chainCtx().explorerName}, {"url", chainCtx().explorerUrl + "/address/" + r.wallet}});
            keyboard["inline_keyboard"].push_back(row);
        }
    }

    json navRow = json::array();
    if (page > 1) navRow.push_back({{"text", tr(lang, "rk_prev")}, {"callback_data", "tt_page:" + std::to_string(page - 1)}});
    navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "tt_noop"}});
    if (page < totalPages) navRow.push_back({{"text", tr(lang, "rk_next")}, {"callback_data", "tt_page:" + std::to_string(page + 1)}});
    keyboard["inline_keyboard"].push_back(navRow);
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:toptrader"}}
    }));

    return {text.str(), keyboard.dump()};
}

struct GlobalRankings {
    std::vector<PnlRow> byPnl;
    std::vector<PnlRow> byRoi;
    std::vector<PnlRow> byWinRate;
    std::vector<PnlRow> byActive;
};

std::vector<PnlRow> computeGlobalTop30D(bool& ok) {
    ok = false;
    std::vector<PnlRow> results;
    long long since = static_cast<long long>(time(nullptr)) - WINDOW_SECONDS;

    std::string curWallet, curToken;
    cpp_int heldQty = 0, heldCost = 0;
    cpp_int outerPnl = 0, outerBuyVol = 0;
    int outerCompleted = 0, outerWinning = 0;

    auto flush = [&]() {
        if (!curWallet.empty() && outerCompleted >= MIN_GLOBAL_COMPLETED_TRADES &&
            outerCompleted <= MAX_BOT_FILTER_TRADES) {
            PnlRow row;
            row.wallet = curWallet;
            row.pnlNanos = cppIntToClampedI64(outerPnl);
            double volD = outerBuyVol > 0 ? outerBuyVol.convert_to<double>() : 0.0;
            double pnlD = outerPnl.convert_to<double>();

            row.roiPercent = volD > 0.0 ? (100.0 * pnlD / volD) : 0.0;
            row.winRatePercent = outerCompleted > 0
                ? static_cast<int>(100.0 * outerWinning / outerCompleted + 0.5)
                : 0;
            row.completedTrades = outerCompleted;
            results.push_back(row);
        }
    };

    sqlite3* rdb = g_rankingReadDb ? g_rankingReadDb : db;
    std::unique_lock<std::mutex> readLock, writeLock;
    if (g_rankingReadDb) readLock = std::unique_lock<std::mutex>(g_rankingReadMutex);
    else                 writeLock = std::unique_lock<std::mutex>(dbMutex);

    sqlite3_stmt* s;
    if (!prepareOrLog(rdb, &s,
        "SELECT t.wallet, t.token, t.is_buy, t.usd_nanos, t.token_amount FROM trades t "
        "WHERE t.timestamp>=? "
        "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw WHERE iw.wallet=t.wallet AND iw.permanent=1) "
        "ORDER BY t.wallet ASC, t.token ASC, t.timestamp ASC, t.id ASC")) return results;
    sqlite3_bind_int64(s, 1, since);

    int stepRc;
    while ((stepRc = sqlite3_step(s)) == SQLITE_ROW) {
        std::string wallet = safeColumnText(s, 0);
        std::string token = safeColumnText(s, 1);
        bool isBuy = sqlite3_column_int(s, 2) != 0;
        int64_t usdNanos = sqlite3_column_int64(s, 3);
        std::string amountStr = safeColumnText(s, 4);
        cpp_int amount;
        if (!safeParseAmount(amountStr, "wallet=" + wallet + " token=" + token, amount)) {
            sqlite3_finalize(s);
            std::cerr << "[RANKING] computeGlobalTop30D() aborted: corrupted token_amount" << std::endl;
            return results;
        }

        if (wallet != curWallet) {
            flush();
            curWallet = wallet;
            curToken.clear();
            outerPnl = 0; outerBuyVol = 0; outerCompleted = 0; outerWinning = 0;
        }
        if (token != curToken) {
            curToken = token;
            heldQty = 0; heldCost = 0;
        }

        if (isBuy) {
            heldQty += amount;
            heldCost += cpp_int(usdNanos);
            outerBuyVol += cpp_int(usdNanos);
        } else if (heldQty > 0 && amount > 0) {
            cpp_int matchedQty = amount < heldQty ? amount : heldQty;
            cpp_int costOfMatched = (heldCost * matchedQty) / heldQty;
            cpp_int proceedsMatched = (cpp_int(usdNanos) * matchedQty) / amount;

            outerPnl += (proceedsMatched - costOfMatched);
            outerCompleted++;
            if (proceedsMatched > costOfMatched) outerWinning++;

            heldQty -= matchedQty;
            heldCost -= costOfMatched;
        }

    }
    sqlite3_finalize(s);
    if (stepRc != SQLITE_DONE) {
        std::cerr << "[RANKING] computeGlobalTop30D() interrupted mid-read: "
                  << sqlite3_errmsg(rdb) << std::endl;
        return results;
    }
    ok = true;
    flush();
    return results;
}

GlobalRankings buildGlobalRankings(const std::vector<PnlRow>& base) {
    GlobalRankings out;

    out.byPnl = base;
    std::sort(out.byPnl.begin(), out.byPnl.end(),
        [](const PnlRow& a, const PnlRow& b) { return a.pnlNanos > b.pnlNanos; });
    if (out.byPnl.size() > static_cast<size_t>(MAX_GLOBAL_RANKED)) out.byPnl.resize(MAX_GLOBAL_RANKED);

    out.byRoi = base;
    std::sort(out.byRoi.begin(), out.byRoi.end(),
        [](const PnlRow& a, const PnlRow& b) { return a.roiPercent > b.roiPercent; });
    if (out.byRoi.size() > static_cast<size_t>(MAX_GLOBAL_RANKED)) out.byRoi.resize(MAX_GLOBAL_RANKED);

    out.byWinRate = base;
    std::sort(out.byWinRate.begin(), out.byWinRate.end(), [](const PnlRow& a, const PnlRow& b) {
        if (a.winRatePercent != b.winRatePercent) return a.winRatePercent > b.winRatePercent;
        return a.completedTrades > b.completedTrades;
    });
    if (out.byWinRate.size() > static_cast<size_t>(MAX_GLOBAL_RANKED)) out.byWinRate.resize(MAX_GLOBAL_RANKED);

    out.byActive = base;
    std::sort(out.byActive.begin(), out.byActive.end(), [](const PnlRow& a, const PnlRow& b) {
        if (a.completedTrades != b.completedTrades) return a.completedTrades > b.completedTrades;
        return a.pnlNanos > b.pnlNanos;
    });
    if (out.byActive.size() > static_cast<size_t>(MAX_GLOBAL_RANKED)) out.byActive.resize(MAX_GLOBAL_RANKED);

    return out;
}

std::string globalTitle(GlobalRankKind kind, Lang lang) {
    switch (kind) {
        case GlobalRankKind::PNL: return "💵 " + tr(lang, "rk_top_pnl_30d");
        case GlobalRankKind::ROI: return "📈 " + tr(lang, "rk_top_roi_30d");
        case GlobalRankKind::WIN_RATE: return "🎯 " + tr(lang, "rk_top_winrate_30d");
        case GlobalRankKind::ACTIVE: return "🔄 " + tr(lang, "rk_most_active_30d");
    }
    return "🏆 " + tr(lang, "rk_top_traders_30d");
}

RankingMessage renderGlobalPage(GlobalRankKind kind, const std::vector<PnlRow>& rows, int page,
                                int maxRank, bool showUpgrade, Lang lang) {
    if (maxRank < 1) maxRank = 1;
    int visible = std::min(static_cast<int>(rows.size()), maxRank);
    int totalPages = std::max(1, (visible + GLOBAL_PER_PAGE - 1) / GLOBAL_PER_PAGE);
    page = std::max(1, std::min(page, totalPages));
    int startIdx = (page - 1) * GLOBAL_PER_PAGE;
    int endIdx = std::min(visible, startIdx + GLOBAL_PER_PAGE);

    std::stringstream text;
    text << "🏆 <b>" << globalTitle(kind, lang) << "</b>\n\n";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    if (rows.empty()) {
        text << "📊 " << tr(lang, "rk_no_completed_trades");
    } else {
        for (int i = startIdx; i < endIdx; i++) {
            const PnlRow& r = rows[i];
            int rank = i + 1;
            text << rankLabel(rank) << "\n";
            text << "<code>" << safeString(r.wallet, 42) << "</code>\n\n";
            text << "💵 <b>PnL:</b> " << formatUsdSigned(r.pnlNanos) << "\n";
            text << "📈 <b>ROI:</b> " << formatPercentPlain(r.roiPercent) << "\n";
            text << "🎯 <b>" << tr(lang, "ws_winrate") << ":</b> " << r.winRatePercent << "%\n";
            text << "🔄 <b>" << tr(lang, "rk_trades") << ":</b> " << r.completedTrades << "\n";
            if (i + 1 < endIdx) text << "\n" << CARD_SEPARATOR << "\n\n";

            json row;
            row.push_back({{"text", tr(lang, "rk_track") + " #" + std::to_string(rank)}, {"callback_data", "tt_track:" + r.wallet}});
            keyboard["inline_keyboard"].push_back(row);
        }
    }

    if (showUpgrade && !rows.empty()) {
        text << "\n" << CARD_SEPARATOR << "\n";
        text << tr(lang, "rk_unlock_top100");
        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
        }));
    }

    std::string kindParam = globalRankKindToString(kind);
    json navRow = json::array();
    if (page > 1) navRow.push_back({{"text", "⬅️"}, {"callback_data", "gt_page:" + kindParam + ":" + std::to_string(page - 1)}});
    navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "tt_noop"}});
    if (page < totalPages) navRow.push_back({{"text", "➡️"}, {"callback_data", "gt_page:" + kindParam + ":" + std::to_string(page + 1)}});
    keyboard["inline_keyboard"].push_back(navRow);
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:toptrader"}}
    }));

    return {text.str(), keyboard.dump()};
}

RankingMessage buildTokenRankingFromCache(const std::string& token, int page, Lang lang) {
    std::string payload;
    if (!loadCachedPayload("token_" + token, payload)) {
        if (!cacheReady()) return buildGeneratingMessage(lang);
        return renderPage(token, std::vector<PnlRow>{}, page, lang);
    }
    std::vector<PnlRow> rows;
    if (!rowsFromJson(payload, rows)) return buildGeneratingMessage(lang);
    return renderPage(token, rows, page, lang);
}

RankingMessage buildGlobalFromCache(GlobalRankKind kind, int page, int maxRank, bool showUpgrade, Lang lang) {
    std::string payload;
    if (!loadCachedPayload("global_" + globalRankKindToString(kind), payload)) {
        return buildGeneratingMessage(lang);
    }
    std::vector<PnlRow> rows;
    if (!rowsFromJson(payload, rows)) return buildGeneratingMessage(lang);
    return renderGlobalPage(kind, rows, page, maxRank, showUpgrade, lang);
}

std::vector<std::string> listActiveTokens(bool& ok) {
    ok = false;
    std::vector<std::string> tokens;
    long long since = static_cast<long long>(time(nullptr)) - WINDOW_SECONDS;

    sqlite3* rdb = g_rankingReadDb ? g_rankingReadDb : db;
    std::unique_lock<std::mutex> readLock, writeLock;
    if (g_rankingReadDb) readLock = std::unique_lock<std::mutex>(g_rankingReadMutex);
    else                 writeLock = std::unique_lock<std::mutex>(dbMutex);

    sqlite3_stmt* s;
    if (!prepareOrLog(rdb, &s, "SELECT DISTINCT token FROM trades WHERE timestamp>=?")) return tokens;
    sqlite3_bind_int64(s, 1, since);
    int stepRc;
    while ((stepRc = sqlite3_step(s)) == SQLITE_ROW) tokens.push_back(safeColumnText(s, 0));
    sqlite3_finalize(s);
    if (stepRc != SQLITE_DONE) {
        std::cerr << "[RANKING] listActiveTokens() interrupted mid-read: " << sqlite3_errmsg(rdb) << std::endl;
        return tokens;
    }
    ok = true;
    return tokens;
}

void rebuildAllRankings() {
    auto t0 = std::chrono::steady_clock::now();

    bool ok = false;
    std::vector<PnlRow> base = computeGlobalTop30D(ok);
    if (!ok) {
        std::cerr << "[RANKING] cache rebuild aborted: global ranking read was interrupted, keeping previous cache" << std::endl;
        return;
    }
    GlobalRankings g = buildGlobalRankings(base);

    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("global_pnl", rowsToJson(g.byPnl));
    entries.emplace_back("global_roi", rowsToJson(g.byRoi));
    entries.emplace_back("global_winrate", rowsToJson(g.byWinRate));
    entries.emplace_back("global_active", rowsToJson(g.byActive));

    std::vector<std::string> tokens = listActiveTokens(ok);
    if (!ok) {
        std::cerr << "[RANKING] cache rebuild aborted: active-token list read was interrupted, keeping previous cache" << std::endl;
        return;
    }
    for (const std::string& tok : tokens) {
        if (!running.load(std::memory_order_relaxed)) return;
        bool tokOk = false;
        auto rows = computeTopPnl(tok, tokOk);
        if (!tokOk) {
            std::cerr << "[RANKING] cache rebuild aborted: token '" << tok << "' ranking read was interrupted, keeping previous cache" << std::endl;
            return;
        }
        entries.emplace_back("token_" + tok, rowsToJson(rows));
    }

    long long now = static_cast<long long>(time(nullptr));
    {
        std::lock_guard<std::mutex> l(dbMutex);
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
            std::cerr << "[RANKING] cache rebuild: BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
            return;
        }
        sqlite3_stmt* s;
        if (!prepareOrLog(db, &s, "DELETE FROM ranking_cache")) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return;
        }
        int delRc = sqlite3_step(s);
        sqlite3_finalize(s);
        if (delRc != SQLITE_DONE) {
            std::cerr << "[RANKING] cache rebuild: DELETE failed: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return;
        }
        if (!prepareOrLog(db, &s,
            "INSERT INTO ranking_cache(cache_key,payload,updated_at) VALUES(?,?,?)")) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return;
        }
        bool insOk = true;
        for (const auto& e : entries) {
            sqlite3_reset(s);
            sqlite3_bind_text(s, 1, e.first.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, e.second.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, now);
            if (sqlite3_step(s) != SQLITE_DONE) { insOk = false; break; }
        }
        sqlite3_finalize(s);
        if (!insOk) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "[RANKING] cache rebuild failed, rolled back" << std::endl;
            return;
        }
        char* commitErr = nullptr;
        if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, &commitErr) != SQLITE_OK) {
            std::cerr << "[RANKING] cache COMMIT failed: " << (commitErr ? commitErr : sqlite3_errmsg(db)) << std::endl;
            if (commitErr) sqlite3_free(commitErr);
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return;
        }
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "[RANKING] Cache rebuilt: " << entries.size() << " entrie(s), " << ms << "ms" << std::endl;
}

}

namespace {

int countTrades30DLocked(const std::string& wallet, long long since) {
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT COUNT(*) FROM trades WHERE wallet=? AND timestamp>=?")) return 0;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, since);
    int n = 0;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

int countCompletedTrades30DLocked(const std::string& wallet, long long since, int stopAfter) {
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT token, is_buy, token_amount FROM trades "
        "WHERE wallet=? AND timestamp>=? ORDER BY token ASC, timestamp ASC, id ASC")) return 0;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, since);

    std::string curToken;
    cpp_int heldQty = 0;
    int completed = 0;
    int stepRc;
    bool stoppedEarly = false;
    while ((stepRc = sqlite3_step(s)) == SQLITE_ROW) {
        std::string token = safeColumnText(s, 0);
        bool isBuy = sqlite3_column_int(s, 1) != 0;
        std::string amountStr = safeColumnText(s, 2);
        cpp_int amount;
        if (!safeParseAmount(amountStr, "wallet=" + wallet + " token=" + token, amount)) amount = 0;

        if (token != curToken) {
            curToken = token;
            heldQty = 0;
        }
        if (isBuy) {
            heldQty += amount;
        } else if (heldQty > 0 && amount > 0) {
            completed++;
            heldQty -= (amount < heldQty ? amount : heldQty);
            if (completed > stopAfter) { stoppedEarly = true; break; }
        }
    }
    sqlite3_finalize(s);
    if (!stoppedEarly && stepRc != SQLITE_DONE) {
        std::cerr << "[RANKING] countCompletedTrades30DLocked(" << wallet << ") interrupted mid-read: "
                  << sqlite3_errmsg(db) << " (returning undercount " << completed << ")" << std::endl;
    }
    return completed;
}

}

void saveTrade(const std::string& wallet, const TxResult& tx,
               const std::string& hash, long long block, long long blockTimestamp) {

    if (!tx.valid || !tx.isSwap) return;
    if (tx.usdNanos <= 0) return;
    if (tx.tokenAddr.empty()) return;
    if (tx.rawAmount <= 0) return;

    long long now = static_cast<long long>(time(nullptr));
    long long ts = blockTimestamp > 0 ? blockTimestamp : now;

    std::string token = toLower(tx.tokenAddr);
    std::string amountStr = tx.rawAmount.convert_to<std::string>();
    int64_t usdNanos64 = cppIntToClampedI64(tx.usdNanos);

    bool blockedNow = false;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;

        bool ignored = false;
        bool permanent = false;
        long long ignoredUntil = 0;
        if (!prepareOrLog(db, &s, "SELECT ignored_until, permanent FROM ignored_wallets WHERE wallet=?")) return;
        sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            ignored = true;
            ignoredUntil = sqlite3_column_int64(s, 0);
            permanent = sqlite3_column_int(s, 1) != 0;
        }
        sqlite3_finalize(s);

        if (permanent) return;

        if (ignored) {
            if (ignoredUntil > now) return;
            if (prepareOrLog(db, &s, "DELETE FROM ignored_wallets WHERE wallet=?")) {
                sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(s);
                sqlite3_finalize(s);
                std::cout << "[RANKING] Block expired, wallet re-enabled: " << wallet << std::endl;
            }
        }

        if (!prepareOrLog(db, &s,
            "INSERT OR IGNORE INTO trades(wallet,token,is_buy,usd_nanos,token_amount,tx_hash,block_number,timestamp) "
            "VALUES(?,?,?,?,?,?,?,?)")) return;
        sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 3, tx.isBuy ? 1 : 0);
        sqlite3_bind_int64(s, 4, usdNanos64);
        sqlite3_bind_text(s, 5, amountStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 6, hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 7, block);
        sqlite3_bind_int64(s, 8, static_cast<sqlite3_int64>(ts));
        int insRc = sqlite3_step(s);
        bool inserted = false;
        if (insRc == SQLITE_DONE) {
            inserted = sqlite3_changes(db) == 1;
        } else {
            std::cerr << "[RANKING] trade insert failed: " << sqlite3_errmsg(db) << std::endl;
        }
        sqlite3_finalize(s);

        if (inserted && !tx.isBuy) {
            long long since = now - WINDOW_SECONDS;
            if (countTrades30DLocked(wallet, since) > MAX_BOT_FILTER_TRADES &&
                countCompletedTrades30DLocked(wallet, since, MAX_BOT_FILTER_TRADES) > MAX_BOT_FILTER_TRADES) {
                bool blockSaved = false;
                if (prepareOrLog(db, &s,
                    "INSERT OR REPLACE INTO ignored_wallets(wallet, ignored_until, permanent) VALUES(?, ?, 1)")) {
                    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(s, 2, now);
                    int blockRc = sqlite3_step(s);
                    if (blockRc == SQLITE_DONE) {
                        blockSaved = sqlite3_changes(db) == 1;
                    } else {
                        std::cerr << "[RANKING] permanent block failed: " << sqlite3_errmsg(db) << std::endl;
                    }
                    sqlite3_finalize(s);
                }
                if (blockSaved) {
                    blockedNow = true;
                    std::cout << "[RANKING] Bot detected: " << wallet
                              << " — permanently excluded from ranking (>500 trades/30d)" << std::endl;
                } else {
                    std::cerr << "[RANKING] Bot detected: " << wallet
                              << " but permanent block was NOT saved - will re-evaluate on next trade" << std::endl;
                }
            }
        }
    }

    if (blockedNow) g_forceRebuild.store(true, std::memory_order_relaxed);
}

std::string resolveTokenArg(const std::string& argIn) {
    std::string arg = trim(argIn);
    if (arg.empty()) return "";
    if (isValidAddress(arg)) return toLower(arg);

    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "SELECT address FROM token_cache WHERE symbol = ? COLLATE NOCASE LIMIT 1")) return "";
    sqlite3_bind_text(s, 1, arg.c_str(), -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(s) == SQLITE_ROW) result = safeColumnText(s, 0);
    sqlite3_finalize(s);
    return result;
}

RankingMessage buildTopPnlMessage(const std::string& chatId, const std::string& tokenArg, int page) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    std::string token = resolveTokenArg(tokenArg);
    if (token.empty()) {
        return {tr(lang, "rk_unknown_token1") + " " + safeString(tokenArg, 32) +
                "\n\n" + tr(lang, "rk_unknown_token2"), ""};
    }

    {
        std::lock_guard<std::mutex> l(g_cacheMutex);
        g_lastTokenByChat[chatId] = token;
    }

    return buildTokenRankingFromCache(token, page, lang);
}

RankingMessage buildTopPnlPage(const std::string& chatId, int page) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    std::string token;
    {
        std::lock_guard<std::mutex> l(g_cacheMutex);
        auto it = g_lastTokenByChat.find(chatId);
        if (it == g_lastTokenByChat.end()) {
            return {tr(lang, "rk_expired"), ""};
        }
        token = it->second;
    }
    return buildTokenRankingFromCache(token, page, lang);
}

bool getTraderStats(const std::string& walletArg, TraderStats& out) {
    const std::string wallet = toLower(walletArg);
    out = TraderStats{};
    std::string payload;
    if (loadCachedPayload("global_pnl", payload)) {
        std::vector<PnlRow> rows;
        if (rowsFromJson(payload, rows)) {
            for (size_t i = 0; i < rows.size(); i++) {
                if (toLower(rows[i].wallet) == wallet) {
                    out.rank = static_cast<int>(i) + 1;
                    out.pnlNanos = rows[i].pnlNanos;
                    out.roiPercent = rows[i].roiPercent;
                    out.winRatePercent = rows[i].winRatePercent;
                    out.trades = rows[i].completedTrades;
                    break;
                }
            }
        }
    }
    sqlite3* rdb = g_rankingReadDb ? g_rankingReadDb : db;
    std::unique_lock<std::mutex> readLock, writeLock;
    if (g_rankingReadDb) readLock = std::unique_lock<std::mutex>(g_rankingReadMutex);
    else                 writeLock = std::unique_lock<std::mutex>(dbMutex);
    sqlite3_stmt* s;
    if (prepareOrLog(rdb, &s, "SELECT MAX(timestamp), COUNT(*) FROM trades WHERE wallet=?")) {
        sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            out.lastTs = sqlite3_column_int64(s, 0);
            if (out.trades == 0) out.trades = sqlite3_column_int(s, 1);
        }
        sqlite3_finalize(s);
    }
    return out.rank > 0 || out.lastTs > 0;
}

RankingMessage buildGlobalTopMenu(const std::string& chatId) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "rk_btn_top_pnl")}, {"callback_data", "gt_open:pnl"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "rk_btn_top_roi")}, {"callback_data", "gt_open:roi"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "rk_btn_top_winrate")}, {"callback_data", "gt_open:winrate"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "rk_btn_most_active")}, {"callback_data", "gt_open:active"}}
    }));

    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "rk_btn_top_pnl_by_token")}, {"callback_data", "gt_token"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));

    return {std::string("🏆 <b>" + tr(lang, "rk_top_traders_30d") + "</b>\n")
            + "<code>" + MENU_STRETCH + "</code>"
            + "\n" + tr(lang, "rk_choose_ranking"), keyboard.dump()};
}

RankingMessage buildGlobalTopMessage(const std::string& chatId, GlobalRankKind kind,
                                     int maxRank, bool showUpgrade) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    return buildGlobalFromCache(kind, 1, maxRank, showUpgrade, lang);
}

RankingMessage buildGlobalTopPage(const std::string& chatId, GlobalRankKind kind, int page,
                                  int maxRank, bool showUpgrade) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    return buildGlobalFromCache(kind, page, maxRank, showUpgrade, lang);
}

RankingMessage buildDailyChannelDigest() {
    std::string payload;
    if (!loadCachedPayload("global_pnl", payload)) return {"", ""};
    std::vector<PnlRow> rows;
    if (!rowsFromJson(payload, rows)) return {"", ""};
    if (rows.empty()) return {"", ""};
    if (rows.size() > 10) rows.resize(10);

    std::stringstream text;
    text << "🏆 <b>Daily Top 10 Traders (30D PnL)</b>\n\n";
    for (size_t i = 0; i < rows.size(); i++) {
        const PnlRow& r = rows[i];
        text << rankLabel(static_cast<int>(i) + 1) << "\n";
        text << "<code>" << safeString(r.wallet, 42) << "</code>\n";
        text << "💵 " << formatUsdSigned(r.pnlNanos)
             << " | 📈 " << formatPercentPlain(r.roiPercent)
             << " | 🎯 " << r.winRatePercent << "%"
             << " | 🔄 " << r.completedTrades << "\n\n";
    }
    return {text.str(), ""};
}

void rankingCacheLoop() {
    while (running.load(std::memory_order_relaxed)) {
        try {
            rebuildAllRankings();
        } catch (const std::exception& e) {
            std::cerr << "[RANKING] rebuild threw: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[RANKING] rebuild threw unknown exception" << std::endl;
        }
        for (long long slept = 0;
             running.load(std::memory_order_relaxed) && slept < REBUILD_INTERVAL_SECONDS;
             slept++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (g_forceRebuild.exchange(false)) break;
        }
    }
}
