#include "hyperliquid.h"
#include "ranking.h"
#include "wallet_menu.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "json.hpp"
#include "utils.h"
#include "ru.h"
#include "message_queue.h"
#include "premium.h"
#include "alert_settings.h"

using json = nlohmann::json;

extern std::atomic<bool> running;
extern const std::string SERVICE_CHAT_ID;
std::string http(const std::string& url, const std::string& post, int timeout);
void logCritical(const std::string& msg);
std::string getUserLanguage(const std::string& chatId);
void rememberView(const std::string& chatId, const std::string& data);
std::string shortAddress(const std::string& a);

#include "hyperliquid_internal.h"

using namespace hl;

namespace {

constexpr int HL_PICKER_PER_PAGE = 5;

constexpr int HL_MIN_CLOSED_TRADES = 5;
constexpr int HL_MAX_CLOSED_TRADES_30D = 200;  // как спот: >200/30д = бот
constexpr int HL_PER_PAGE = 5;
constexpr int HL_RANK_WINDOWS_DAYS[] = {30, 90, 180, 365};
constexpr int HL_RANK_WINDOWS_COUNT = 4;
constexpr long long HL_RANK_WINDOW_INTERVAL_SEC[] = {
    15 * 60,            // 30д  — 15 мин
    47 * 60,            // 90д  — 47 мин
    3 * 3600 + 11 * 60, // 180д — 3ч 11м
    6 * 3600 + 13 * 60, // 365д — 6ч 13м
};

int clampHlWindowDays(int days) {
    for (int d : HL_RANK_WINDOWS_DAYS) if (d == days) return d;
    return 30;
}

int hlWindowIndex(int days) {
    days = clampHlWindowDays(days);
    for (int i = 0; i < HL_RANK_WINDOWS_COUNT; i++)
        if (HL_RANK_WINDOWS_DAYS[i] == days) return i;
    return 0;
}

struct PerpRow {
    std::string wallet;
    long long pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int closedTrades = 0;
    double avgLeverage = 0.0;
    bool roiKnown = false;
    bool winRateKnown = false;
    long long volumeNanos = 0;
    long long lastTs = 0;
};

std::mutex g_rankMutex;
long long g_rankBuiltAt[4] = {0, 0, 0, 0};
std::vector<PerpRow> g_rankCache[4];

long long fundingPayment(long long pos, long long rate, long long mark) {
    if (pos == 0 || rate == 0 || mark <= 0) return 0;
    __int128 x = -static_cast<__int128>(pos) * mark;
    x *= rate;
    x /= static_cast<__int128>(NANOS_PER_UNIT);
    x /= static_cast<__int128>(NANOS_PER_UNIT);
    if (x > 9000000000000000000LL || x < -9000000000000000000LL) return 0;
    return static_cast<long long>(x);
}

std::vector<PerpRow> computeRanking(long long windowSec, bool& ok) {
    ok = false;
    std::vector<PerpRow> rows;
    if (windowSec < 86400LL) windowSec = 86400LL;
    const long long sinceMs = (nowSec() - windowSec) * 1000LL;
    const long long untilMs = nowSec() * 1000LL;
    const long long sinceHour = (nowSec() - windowSec) / 3600LL * 3600LL;

    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return rows;

    using HourRate = std::pair<long long, long long>;
    std::unordered_map<std::string, std::unordered_map<long long, HourRate>> rates;
    {
        sqlite3_stmt* r = nullptr;
        if (prepareOrLog(g_hlDb, &r,
                "SELECT coin,hour_ts,rate_nanos,mark_nanos FROM hl_funding_rate WHERE hour_ts>=?")) {
            sqlite3_bind_int64(r, 1, sinceHour);
            int rcRates = SQLITE_DONE;
            while ((rcRates = sqlite3_step(r)) == SQLITE_ROW) {
                const std::string coin = safeColumnText(r, 0);
                rates[coin][sqlite3_column_int64(r, 1)] =
                    {sqlite3_column_int64(r, 2), sqlite3_column_int64(r, 3)};
            }
            sqlite3_finalize(r);
            if (rcRates != SQLITE_DONE) return rows;
        }
    }

    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT f.wallet,f.coin,f.ts,f.tid,f.oid,f.dir_code,f.flat,"
            " f.closed_pnl_nanos,f.margin_nanos,f.leverage,f.notional_nanos,"
            " f.start_pos_nanos,f.sz,f.side,f.px,f.fee_nanos"
            " FROM hl_fills f"
            " WHERE f.ts>=?"
            " AND NOT EXISTS (SELECT 1 FROM hl_banned b WHERE b.wallet=f.wallet)"
            " ORDER BY f.wallet,f.coin,f.ts,f.tid"))
        return rows;
    sqlite3_bind_int64(s, 1, sinceMs);

    struct Acc {
        long long closedPnl = 0;
        long long funding = 0;
        long long fees = 0;
        long long volume = 0;
        long long lastTs = 0;
        int trades = 0;
        int wins = 0;
        int losses = 0;
        long long margin = 0;
        long long levSum = 0;
        int levN = 0;
    };
    struct Series {
        long long pnl = 0;
        long long fee = 0;
        long long margin = 0;
        int lev = 0;
        long long closeOid = 0;
    };
    struct PosWalk {
        long long pos = 0;
        long long lastTs = 0;
        long long lastPx = 0;
        bool known = false;
    };

    std::unordered_map<std::string, Acc> accs;
    std::string curW, curC;
    Series ser;
    PosWalk pw;

    auto closeTrade = [&](Acc& a) {
        a.closedPnl += ser.pnl;
        a.trades++;
        const long long net = ser.pnl - ser.fee;
        if (net > 0) a.wins++;
        else if (net < 0) a.losses++;
        if (ser.lev > 0) { a.levSum += ser.lev; a.levN++; }
        if (ser.margin > 0) a.margin += ser.margin;
    };

    auto applyHours = [&](Acc& a, const std::string& coin, long long pos, long long px,
                          long long fromMs, long long toMs) {
        if (pos == 0 || fromMs >= toMs) return;
        auto rit = rates.find(coin);
        if (rit == rates.end()) return;
        const long long fromH = ((fromMs / 1000LL) / 3600LL + 1) * 3600LL;
        const long long toH = (toMs / 1000LL) / 3600LL * 3600LL;
        for (long long h = fromH; h <= toH; h += 3600LL) {
            auto hit = rit->second.find(h);
            if (hit == rit->second.end()) continue;
            long long mark = hit->second.second > 0 ? hit->second.second : px;
            a.funding += fundingPayment(pos, hit->second.first, mark);
        }
    };

    auto flushPos = [&]() {
        if (curW.empty() || !pw.known) return;
        applyHours(accs[curW], curC, pw.pos, pw.lastPx, pw.lastTs, untilMs);
    };

    int stepRc;
    while ((stepRc = sqlite3_step(s)) == SQLITE_ROW) {
        const std::string w = safeColumnText(s, 0);
        const std::string c = safeColumnText(s, 1);
        const long long ts = sqlite3_column_int64(s, 2);
        const long long tid = sqlite3_column_int64(s, 3);
        const long long oid = sqlite3_column_int64(s, 4);
        const int dir = sqlite3_column_int(s, 5);
        const int flat = sqlite3_column_int(s, 6);
        const long long pnl = sqlite3_column_int64(s, 7);
        const long long margin = sqlite3_column_int64(s, 8);
        const int lev = sqlite3_column_int(s, 9);
        const long long notional = sqlite3_column_int64(s, 10);
        const bool hasStart = sqlite3_column_type(s, 11) != SQLITE_NULL;
        const long long startCol = hasStart ? sqlite3_column_int64(s, 11) : 0;
        long long sz = 0;
        parseDecimalToNanos(safeColumnText(s, 12), sz);
        if (sz < 0) sz = -sz;
        const std::string side = safeColumnText(s, 13);
        const long long signedSz = (side == "B") ? sz : -sz;
        long long px = 0;
        parseDecimalToNanos(safeColumnText(s, 14), px);
        if (px < 0) px = -px;
        const long long fee = sqlite3_column_int64(s, 15);

        if (w != curW || c != curC) {
            flushPos();
            ser = Series{};
            pw = PosWalk{};
            curW = w;
            curC = c;
        }

        Acc& a = accs[w];
        a.volume += notional;
        a.fees += fee;
        if (ts > a.lastTs) a.lastTs = ts;

        long long start = 0;
        bool startKnown = false;
        if (hasStart) { start = startCol; startKnown = true; }
        else if (pw.known) { start = pw.pos; startKnown = true; }
        else if (flat == 1) { start = -signedSz; startKnown = true; }
        else if (dir == DIR_OPEN_LONG || dir == DIR_OPEN_SHORT) { start = 0; startKnown = true; }

        if (startKnown) {
            const long long fromMs = pw.known ? pw.lastTs : sinceMs;
            applyHours(a, c, start, px > 0 ? px : pw.lastPx, fromMs, ts);
            pw.pos = start + signedSz;
            pw.known = true;
            pw.lastTs = ts;
            if (px > 0) pw.lastPx = px;
        }

        ser.pnl += pnl;
        ser.fee += fee;
        if (margin > 0) {
            if (margin > ser.margin) ser.margin = margin;
        } else if (lev > 0 && notional > 0) {
            const long long m = notional / lev;
            if (m > ser.margin) ser.margin = m;
        }
        if (lev > 0) ser.lev = lev;

        if (flat != 1 && dir < 5) continue;

        const long long id = oid > 0 ? oid : tid;
        if (id != 0 && id == ser.closeOid) {
            a.closedPnl += ser.pnl;
        } else {
            closeTrade(a);
            ser.closeOid = id;
        }
        ser.pnl = 0;
        ser.fee = 0;
        ser.margin = 0;
        ser.lev = 0;
    }
    sqlite3_finalize(s);
    if (stepRc != SQLITE_DONE) {
        std::cerr << "[HL] рейтинг: чтение прервано, старый кэш сохранён" << std::endl;
        return {};
    }
    flushPos();

    rows.reserve(accs.size());
    const long long maxForWindow = std::max(1LL,
        (HL_MAX_CLOSED_TRADES_30D * windowSec + (30LL * 86400LL - 1)) / (30LL * 86400LL));
    for (auto& kv : accs) {
        Acc& a = kv.second;
        if (a.trades < HL_MIN_CLOSED_TRADES) continue;
        if (a.trades > maxForWindow) continue;
        PerpRow r;
        r.wallet = kv.first;
        r.pnlNanos = a.closedPnl + a.funding - a.fees;
        r.volumeNanos = a.volume;
        r.lastTs = a.lastTs;
        r.closedTrades = a.trades;
        const int decided = a.wins + a.losses;
        r.winRateKnown = decided > 0;
        r.winRatePercent = decided > 0 ? static_cast<int>((100LL * a.wins) / decided) : 0;
        r.avgLeverage = a.levN > 0 ? static_cast<double>(a.levSum) / a.levN : 0.0;
        if (a.margin > 0) {
            r.roiPercent = 100.0 * static_cast<double>(r.pnlNanos) / static_cast<double>(a.margin);
            r.roiKnown = true;
        }
        rows.push_back(std::move(r));
    }
    ok = true;
    return rows;
}

void rankingSnapshot(std::vector<PerpRow>& localCopy, int windowDays = 30) {
    const int idx = hlWindowIndex(windowDays);
    std::lock_guard<std::mutex> l(g_rankMutex);
    localCopy = g_rankCache[idx];
}

enum class PerpKind { PNL, ROI, WINRATE, ACTIVE };

bool parsePerpKind(const std::string& v, PerpKind& out) {
    if (v == "pnl")     { out = PerpKind::PNL;     return true; }
    if (v == "roi")     { out = PerpKind::ROI;     return true; }
    if (v == "winrate") { out = PerpKind::WINRATE; return true; }
    if (v == "active")  { out = PerpKind::ACTIVE;  return true; }
    return false;
}

std::string perpKindStr(PerpKind k) {
    switch (k) {
        case PerpKind::ROI:     return "roi";
        case PerpKind::WINRATE: return "winrate";
        case PerpKind::ACTIVE:  return "active";
        default:                return "pnl";
    }
}

std::string perpTitle(PerpKind k, Lang lang) {
    switch (k) {
        case PerpKind::ROI:     return tr(lang, "rk_btn_top_roi");
        case PerpKind::WINRATE: return tr(lang, "rk_btn_top_winrate");
        case PerpKind::ACTIVE:  return tr(lang, "rk_btn_most_active");
        default:                return tr(lang, "rk_btn_top_pnl");
    }
}

void sortByKind(std::vector<PerpRow>& rows, PerpKind kind) {
    switch (kind) {
        case PerpKind::ROI:
            std::sort(rows.begin(), rows.end(), [](const PerpRow& a, const PerpRow& b) {
                if (a.roiKnown != b.roiKnown) return a.roiKnown;
                return a.roiPercent > b.roiPercent;
            });
            break;
        case PerpKind::WINRATE:
            std::sort(rows.begin(), rows.end(), [](const PerpRow& a, const PerpRow& b) {
                if (a.winRateKnown != b.winRateKnown) return a.winRateKnown;
                if (a.winRatePercent != b.winRatePercent) return a.winRatePercent > b.winRatePercent;
                return a.closedTrades > b.closedTrades;
            });
            break;
        case PerpKind::ACTIVE:
            std::sort(rows.begin(), rows.end(),
                      [](const PerpRow& a, const PerpRow& b) { return a.closedTrades > b.closedTrades; });
            break;
        default:
            std::sort(rows.begin(), rows.end(),
                      [](const PerpRow& a, const PerpRow& b) { return a.pnlNanos > b.pnlNanos; });
    }
}

std::string rankLabel(int rank) {
    switch (rank) {
        case 1: return "\U0001F947 #1";
        case 2: return "\U0001F948 #2";
        case 3: return "\U0001F949 #3";
        default: return "#" + std::to_string(rank);
    }
}

HlMessage renderPerpPage(const std::string& chatId, PerpKind kind, int page, int windowDays = 30) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    windowDays = clampHlWindowDays(windowDays);
    int maxRank = premiumTopTradersLimit(chatId);
    if (maxRank < 1) maxRank = 1;
    const bool showUpgrade = !isPremium(chatId);

    std::vector<PerpRow> rows;
    rankingSnapshot(rows, windowDays);
    sortByKind(rows, kind);

    const int visible = std::min(static_cast<int>(rows.size()), maxRank);
    const int totalPages = std::max(1, (visible + HL_PER_PAGE - 1) / HL_PER_PAGE);
    page = std::max(1, std::min(page, totalPages));
    const int startIdx = (page - 1) * HL_PER_PAGE;
    const int endIdx = std::min(visible, startIdx + HL_PER_PAGE);

    const char* const dm = dirMark(lang);

    std::stringstream text;
    text << dm << "\U0001F535 <b>Hyperliquid \u2014 " << perpTitle(kind, lang) << "</b> · " << windowDays << tr(lang, "unit_day") << "\n\n";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    if (visible == 0) {
        text << dm << "\U0001F4CA " << tr(lang, "hl_rk_empty");
    } else {
        for (int i = startIdx; i < endIdx; i++) {
            const PerpRow& r = rows[static_cast<size_t>(i)];
            const int rank = i + 1;
            text << dm << rankLabel(rank) << "\n";
            text << dm << "<code>" << safeString(r.wallet, 42) << "</code>\n\n";
            text << dm << "\U0001F4B5 <b>PnL:</b> " << formatUsdNanosSigned(r.pnlNanos, true) << "\n";
            if (r.roiKnown)
                text << dm << "\U0001F4C8 <b>" << tr(lang, "rk_roi_per_trade") << ":</b> " << formatPercent(r.roiPercent, true) << "\n";
            if (r.winRateKnown)
                text << dm << "\U0001F3AF <b>" << tr(lang, "ws_winrate") << ":</b> " << r.winRatePercent << "%\n";
            text << dm << "\U0001F504 <b>" << tr(lang, "rk_trades") << ":</b> " << r.closedTrades << "\n";
            if (r.avgLeverage > 0.0) {
                text << dm << "\u2699\uFE0F <b>" << tr(lang, "hl_rk_leverage") << ":</b> "
                     << static_cast<int>(r.avgLeverage + 0.5) << "\u00D7\n";
            }
            if (const int days = rankPresenceDays("perp", r.wallet); days > 0)
                text << dm << "\U0001F4C5 <b>" << tr(lang, "rk_in_top") << ":</b> "
                     << days << " " << tr(lang, "rk_days") << "\n";
            if (i + 1 < endIdx) text << "\n" << dm << HL_CARD_SEPARATOR << "\n\n";

            keyboard["inline_keyboard"].push_back(json::array({
                {{"text", tr(lang, "rk_track") + " #" + std::to_string(rank)},
                 {"callback_data", "tt_track:" + r.wallet}}
            }));
        }
    }

    if (showUpgrade && visible > 0) {
        text << "\n" << dm << HL_CARD_SEPARATOR << "\n";
        text << dm << tr(lang, "rk_unlock_top100");
        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
        }));
    }

    const std::string kindParam = perpKindStr(kind);
    const std::string winSuffix = ":" + std::to_string(windowDays);
    json navRow = json::array();
    if (page > 1)
        navRow.push_back({{"text", "\u2B05\uFE0F"}, {"callback_data", "hl_page:" + kindParam + ":" + std::to_string(page - 1) + winSuffix}});
    navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "tt_noop"}});
    if (page < totalPages)
        navRow.push_back({{"text", "\u27A1\uFE0F"}, {"callback_data", "hl_page:" + kindParam + ":" + std::to_string(page + 1) + winSuffix}});
    keyboard["inline_keyboard"].push_back(navRow);

    json winRow = json::array();
    for (int d : HL_RANK_WINDOWS_DAYS) {
        const bool on = (d == windowDays);
        const std::string label = on
            ? ("· " + std::to_string(d) + tr(lang, "unit_day") + " ·")
            : (std::to_string(d) + tr(lang, "unit_day"));
        winRow.push_back({{"text", label},
                          {"callback_data", "hl_open:" + kindParam + ":" + std::to_string(d)}});
    }
    keyboard["inline_keyboard"].push_back(winRow);

    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    return {text.str(), keyboard.dump()};
}

}

std::string hyperliquidStatsLine() {
    std::stringstream ss;
    const long long last = g_lastMsgTs.load(std::memory_order_relaxed);
    size_t queued;
    { std::lock_guard<std::mutex> l(g_queueMutex); queued = g_enrichQueue.size() + g_urgentQueue.size(); }

    long long bannedTotal = 0;
    {
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (g_hlDb) {
            sqlite3_stmt* s;
            if (prepareOrLog(g_hlDb, &s, "SELECT COUNT(*) FROM hl_banned")) {
                if (sqlite3_step(s) == SQLITE_ROW) bannedTotal = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
        }
    }

    const unsigned long long seen  = g_tradesSeen.load(std::memory_order_relaxed);
    const unsigned long long hits  = g_hits.load(std::memory_order_relaxed);
    const unsigned long long enr   = g_enriched.load(std::memory_order_relaxed);
    const unsigned long long alerts= g_alertsSent.load(std::memory_order_relaxed);
    const unsigned long long skips = g_budgetSkips.load(std::memory_order_relaxed);
    const unsigned long long recon = g_reconnects.load(std::memory_order_relaxed);

    ss << "\n\n\U0001F535 <b>Hyperliquid</b>\n"
       << (g_connected.load(std::memory_order_relaxed) ? "\u2705 подключён" : "\u274C нет связи")
       << " \u00B7 слежу за " << formatThousands(watchedCount()) << " кошельками"
       << " на " << g_subscribedCoins.load(std::memory_order_relaxed) << " монетах";

    if (last > 0) ss << "\n\u23F1 последняя сделка: " << (nowSec() - last) << "с назад";

    ss << "\n\n\U0001F4CA <b>Поток</b>"
       << "\n\u2022 сделок на бирже: " << formatThousands(seen)
       << "\n\u2022 у наших кошельков: " << formatThousands(hits);

    ss << "\n\n\U0001F4E8 <b>Алерты</b>"
       << "\n\u2022 получателей: " << hlAlertRecipientCount()
       << "\n\u2022 отправлено: " << formatThousands(alerts)
       << "\n\u2022 запросов истории: " << formatThousands(enr);
    if (queued > 0) ss << "\n\u2022 ждут проверки: " << queued << " кошельков";

    long long bscBanned = 0;
    {
        extern sqlite3* db;
        extern std::mutex dbMutex;
        std::lock_guard<std::mutex> l(dbMutex);
        if (db) {
            sqlite3_stmt* s = nullptr;
            if (prepareOrLog(db, &s, "SELECT COUNT(*) FROM ignored_wallets WHERE permanent=1")) {
                if (sqlite3_step(s) == SQLITE_ROW) bscBanned = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
        }
    }
    ss << "\n\n\U0001F916 <b>Фильтры</b>"
       << "\n\u2022 боты BSC (бан): " << formatThousands(static_cast<uint64_t>(bscBanned))
       << "\n\u2022 боты HL (бан): " << formatThousands(static_cast<uint64_t>(bannedTotal));
    ss << "\n\u2022 пропущено по лимиту API: " << formatThousands(skips);
    if (recon > 0) ss << "\n\u2022 обрывов связи: " << recon;

    return ss.str();
}

std::vector<std::pair<std::string, PerpRankInfo>> perpTopThree() {
    std::vector<PerpRow> rows;
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        rows = g_rankCache[0]; // 30d
    }
    std::sort(rows.begin(), rows.end(),
              [](const PerpRow& a, const PerpRow& b) { return a.pnlNanos > b.pnlNanos; });

    std::vector<std::pair<std::string, PerpRankInfo>> out;
    for (size_t i = 0; i < rows.size() && out.size() < 3; i++) {
        PerpRankInfo info;
        info.rank = static_cast<int>(i) + 1;
        info.total = static_cast<int>(rows.size());
        info.pnlNanos = rows[i].pnlNanos;
        info.roiPercent = rows[i].roiPercent;
        info.roiKnown = rows[i].roiKnown;
        info.winRatePercent = rows[i].winRatePercent;
        info.winRateKnown = rows[i].winRateKnown;
        info.closedTrades = rows[i].closedTrades;
        out.emplace_back(rows[i].wallet, info);
    }
    return out;
}

std::unordered_set<std::string> hlTopPnlWallets(int n) {
    std::unordered_set<std::string> out;
    if (n <= 0) return out;
    std::vector<PerpRow> rows;
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        rows = g_rankCache[0];
    }
    if (rows.empty()) return out;
    std::sort(rows.begin(), rows.end(),
              [](const PerpRow& a, const PerpRow& b) { return a.pnlNanos > b.pnlNanos; });
    const int lim = std::min(n, static_cast<int>(rows.size()));
    out.reserve(static_cast<size_t>(lim));
    for (int i = 0; i < lim; i++) {
        std::string w = toLower(rows[static_cast<size_t>(i)].wallet);
        if (!w.empty()) out.insert(std::move(w));
    }
    return out;
}

bool perpRankOf(const std::string& wallet, PerpRankInfo& out) {
    std::vector<PerpRow> rows;
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        rows = g_rankCache[0]; // 30d
    }
    if (rows.empty()) return false;
    std::sort(rows.begin(), rows.end(),
              [](const PerpRow& a, const PerpRow& b) { return a.pnlNanos > b.pnlNanos; });
    const std::string w = toLower(wallet);
    for (size_t i = 0; i < rows.size(); i++) {
        if (toLower(rows[i].wallet) != w) continue;
        out.rank = static_cast<int>(i) + 1;
        out.total = static_cast<int>(rows.size());
        out.pnlNanos = rows[i].pnlNanos;
        out.roiPercent = rows[i].roiPercent;
        out.roiKnown = rows[i].roiKnown;
        out.winRatePercent = rows[i].winRatePercent;
        out.winRateKnown = rows[i].winRateKnown;
        out.closedTrades = rows[i].closedTrades;
        return true;
    }
    return false;
}

namespace hl {
void rebuildRankCache() {
    std::vector<std::string> top;
    const long long now = nowSec();

    int best = -1;
    double bestScore = -1.0;
    for (int i = 0; i < HL_RANK_WINDOWS_COUNT; i++) {
        const long long interval = HL_RANK_WINDOW_INTERVAL_SEC[i];
        const long long built = g_rankBuiltAt[i];
        if (built > 0 && (now - built) < interval) continue;
        const double score = (built <= 0)
            ? (1000.0 - i)
            : static_cast<double>(now - built) / static_cast<double>(interval);
        if (score > bestScore) { bestScore = score; best = i; }
    }
    if (best < 0) return;

    const int days = HL_RANK_WINDOWS_DAYS[best];
    bool ok = false;
    std::vector<PerpRow> fresh = computeRanking(static_cast<long long>(days) * 86400LL, ok);
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        if (ok) {
            g_rankCache[best].swap(fresh);
            g_rankBuiltAt[best] = now;
        }
        if (best == 0) {
            top.reserve(g_rankCache[0].size());
            for (const auto& r : g_rankCache[0]) top.push_back(r.wallet);
        }
    }
    if (!top.empty())
        markRankPresence("perp", top);
}

void invalidateRankCache() {
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        for (int i = 0; i < HL_RANK_WINDOWS_COUNT; i++) {
            g_rankBuiltAt[i] = 0;
            g_rankCache[i].clear();
        }
    }
    std::cout << "[HL] рейтинг сброшен, пересчёт при следующем проходе" << std::endl;
}

}

void invalidateRankCache() {
    hl::invalidateRankCache();
}

HlMessage buildVenueMenu(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "hl_venue_spot")}, {"callback_data", "menu:toptrader_spot"}}
    }));
    const std::string perpLabel = tr(lang, "hl_venue_perp")
                                + (isPremium(chatId) ? "" : "  \U0001F512");
    kb["inline_keyboard"].push_back(json::array({
        {{"text", perpLabel}, {"callback_data", "hl_menu"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));
    return {"\U0001F3C6 <b>" + tr(lang, "hl_venue_title") + "</b>\n\n"
            + tr(lang, "hl_venue_choose"), kb.dump()};
}

HlMessage buildPerpTopMenu(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    json kb;
    kb["inline_keyboard"] = json::array();
    const char* kinds[4] = {"pnl", "roi", "winrate", "active"};
    const char* keys[4]  = {"rk_btn_top_pnl", "rk_btn_top_roi",
                            "rk_btn_top_winrate", "rk_btn_most_active"};
    for (int i = 0; i < 4; i++) {
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, keys[i])}, {"callback_data", std::string("hl_open:") + kinds[i]}}
        }));
    }
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));
    return {"\U0001F535 <b>Hyperliquid \u2014 " + tr(lang, "rk_top_traders_30d") + "</b>\n\n"
            + tr(lang, "hl_rk_choose"), kb.dump()};
}

HlMessage buildPerpLocked(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    std::stringstream t;
    t << "\U0001F535 <b>Hyperliquid \u2014 " << tr(lang, "rk_top_traders_30d") << "</b>\n";
    t << HL_CARD_SEPARATOR << "\n\n";
    t << "\U0001F512 " << tr(lang, "hl_locked_body") << "\n";

    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));
    return {t.str(), kb.dump()};
}

struct OpenPosition {
    std::string wallet;
    std::string label;
    std::string coin;
    std::string dex;
    bool isLong = false;
    long long sizeNanos = 0;
    long long entryPxNanos = 0;
    long long markPxNanos = 0;
    long long liqPxNanos = 0;
    long long marginNanos = 0;
    long long unrealizedNanos = 0;
    long long dexAccountNanos = 0;
    double roePercent = 0.0;
    int leverage = 0;
    bool isolated = false;
};

bool fetchOpenPositions(const std::string& wallet, std::vector<OpenPosition>& out,
                       long long& accountValueNanos,
                       std::vector<std::pair<std::string, long long>>& dexAccounts) {
    accountValueNanos = 0;
    dexAccounts.clear();
    bool any = false;

    auto ingest = [&](const std::string& dex, const json& j) -> bool {
        if (!j.is_object()) return false;

        long long dexAv = 0;
        if (j.contains("marginSummary") && j["marginSummary"].is_object())
            dexAv = jsonDecimalNanos(j["marginSummary"], "accountValue");
        if (dexAv <= 0 && j.contains("crossMarginSummary") && j["crossMarginSummary"].is_object())
            dexAv = jsonDecimalNanos(j["crossMarginSummary"], "accountValue");
        if (dexAv > 0) {
            bool have = false;
            for (const auto& d : dexAccounts)
                if (d.first == dex) { have = true; break; }
            if (!have) {
                accountValueNanos += dexAv;
                dexAccounts.emplace_back(dex, dexAv);
            }
        }

        if (!j.contains("assetPositions") || !j["assetPositions"].is_array())
            return true;

        for (const auto& ap : j["assetPositions"]) {
            if (!ap.is_object() || !ap.contains("position") || !ap["position"].is_object()) continue;
            const json& p = ap["position"];

            OpenPosition op;
            op.wallet = wallet;
            op.dex = dex;
            op.dexAccountNanos = dexAv;
            op.coin = jstr(p, "coin");
            if (op.coin.empty()) continue;
            if (!dex.empty() && op.coin.find(':') == std::string::npos)
                op.coin = dex + ":" + op.coin;

            bool dup = false;
            for (const auto& e : out)
                if (e.coin == op.coin) { dup = true; break; }
            if (dup) continue;

            long long szi = 0;
            parseDecimalToNanos(jstr(p, "szi", "0"), szi);
            if (szi == 0) continue;
            op.isLong = szi > 0;
            op.sizeNanos = szi < 0 ? -szi : szi;

            parseDecimalToNanos(jstr(p, "entryPx", "0"), op.entryPxNanos);

            long long posValue = 0;
            if (parseDecimalToNanos(jstr(p, "positionValue", "0"), posValue) &&
                posValue > 0 && op.sizeNanos > 0) {
                const __int128 num = static_cast<__int128>(posValue) * NANOS_PER_UNIT;
                op.markPxNanos = static_cast<long long>(num / op.sizeNanos);
            }
            parseDecimalToNanos(jstr(p, "liquidationPx", "0"), op.liqPxNanos);
            parseDecimalToNanos(jstr(p, "marginUsed", "0"), op.marginNanos);
            parseDecimalToNanos(jstr(p, "unrealizedPnl", "0"), op.unrealizedNanos);

            long long roe = 0;
            if (parseDecimalToNanos(jstr(p, "returnOnEquity", "0"), roe))
                op.roePercent = 100.0 * static_cast<double>(roe) / static_cast<double>(NANOS_PER_UNIT);

            if (p.contains("leverage") && p["leverage"].is_object()) {
                const json& lev = p["leverage"];
                if (lev.contains("value") && lev["value"].is_number())
                    op.leverage = lev["value"].get<int>();
                op.isolated = jstr(lev, "type") == "isolated";
            }
            out.push_back(std::move(op));
        }
        return true;
    };

    std::vector<std::string> dexes = perpDexNames();
    dexes.erase(std::remove(dexes.begin(), dexes.end(), std::string()), dexes.end());
    dexes.insert(dexes.begin(), "");

    for (const std::string& dex : dexes) {
        json body;
        body["type"] = "clearinghouseState";
        body["user"] = wallet;
        if (!dex.empty()) body["dex"] = dex;
        if (ingest(dex, infoPost(body, 0))) any = true;
    }

    bool gotMain = false;
    for (const auto& d : dexAccounts)
        if (d.first.empty()) { gotMain = true; break; }
    if (!gotMain) {
        json w2;
        w2["type"] = "webData2";
        w2["user"] = wallet;
        json j = infoPost(w2, 0);
        if (j.is_object() && j.contains("clearinghouseState") &&
            ingest("", j["clearinghouseState"]))
            any = true;
    }
    return any;
}

HlMessage buildPositionsLocked(Lang lang) {
    std::stringstream t;
    const char* const dm = dirMark(lang);
    t << dm << "\U0001F4C8 <b>" << tr(lang, "hl_open_positions") << "</b>\n"
      << HL_CARD_SEPARATOR << "\n\n"
      << dm << "\U0001F512 " << tr(lang, "hl_locked_body");
    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));
    return {t.str(), kb.dump()};
}

HlMessage buildPositionsPicker(const std::string& chatId, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    if (!isPremium(chatId)) return buildPositionsLocked(lang);

    const char* const dm = dirMark(lang);
    const std::vector<HlUserWallet> wallets = hlUserWallets(chatId);

    const int total = static_cast<int>(wallets.size());
    const int pages = total > 0 ? (total + HL_PICKER_PER_PAGE - 1) / HL_PICKER_PER_PAGE : 1;
    if (page < 1) page = 1;
    if (page > pages) page = pages;
    const int from = (page - 1) * HL_PICKER_PER_PAGE;
    const int to = std::min(from + HL_PICKER_PER_PAGE, total);

    json kb;
    kb["inline_keyboard"] = json::array();
    for (int i = from; i < to; i++) {
        const HlUserWallet& w = wallets[static_cast<size_t>(i)];
        const std::string label = w.label.empty() ? shortAddress(w.address) : safeString(w.label, 32);
        kb["inline_keyboard"].push_back(json::array({
            {{"text", "\U0001F4BC " + label}, {"callback_data", "hl_pos:" + w.address}}
        }));
    }

    if (pages > 1) {
        json nav = json::array();
        if (page > 1)
            nav.push_back({{"text", "\u2B05\uFE0F"},
                           {"callback_data", "hl_pospage:" + std::to_string(page - 1)}});
        nav.push_back({{"text", std::to_string(page) + "/" + std::to_string(pages)},
                       {"callback_data", "hl_posnoop"}});
        if (page < pages)
            nav.push_back({{"text", "\u27A1\uFE0F"},
                           {"callback_data", "hl_pospage:" + std::to_string(page + 1)}});
        kb["inline_keyboard"].push_back(nav);
    }

    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    std::stringstream t;
    t << dm << "\U0001F4C8 <b>" << tr(lang, "hl_open_positions") << "</b>\n"
      << HL_CARD_SEPARATOR << "\n\n";
    t << dm << (wallets.empty() ? tr(lang, "mw_no_wallets") : tr(lang, "hl_positions_choose"));
    return {t.str(), kb.dump()};
}

HlMessage buildWalletPositions(const std::string& chatId, const std::string& address) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    if (!isPremium(chatId)) return buildPositionsLocked(lang);

    const char* const dm = dirMark(lang);
    const std::string addr = toLower(address);

    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    std::string label = shortAddress(addr);
    for (const auto& w : hlUserWallets(chatId))
        if (w.address == addr && !w.label.empty()) { label = safeString(w.label, 32); break; }

    std::stringstream t;
    t << dm << "\U0001F4BC <b>" << label << "</b>\n";
    t << dm << "<code>" << safeString(addr, 42) << "</code>\n";

    SpotRankInfo sr;
    t << dm << "\n\U0001F7E1 <b>BSC " << tr(lang, "wl_spot_rank") << "</b>";
    if (spotRankOf(addr, sr) && sr.rank <= 100) {
        t << " — #" << sr.rank << "\n"
          << dm << "\U0001F4B5 PnL: " << formatUsdNanosSigned(sr.pnlNanos, true) << "\n"
          << dm << "\U0001F4C8 " << tr(lang, "rk_roi_per_trade") << ": "
          << formatPercent(sr.roiPercent, true) << "\n"
          << dm << "\U0001F3AF " << tr(lang, "ws_winrate") << ": " << sr.winRatePercent << "%\n"
          << dm << "\U0001F504 " << tr(lang, "rk_trades") << ": " << sr.completedTrades << "\n";
    } else {
        t << ": " << tr(lang, "wl_not_ranked") << "\n";
    }

    PerpRankInfo pr;
    t << dm << "\n\U0001F535 <b>Hyperliquid " << tr(lang, "wl_perp_rank") << "</b>";
    if (perpRankOf(addr, pr) && pr.rank <= 100) {
        t << " — #" << pr.rank << "\n"
          << dm << "\U0001F4B5 PnL: " << formatUsdNanosSigned(pr.pnlNanos, true) << "\n";
        if (pr.roiKnown)
            t << dm << "\U0001F4C8 " << tr(lang, "rk_roi_per_trade") << ": "
              << formatPercent(pr.roiPercent, true) << "\n";
        t << dm << "\U0001F3AF " << tr(lang, "ws_winrate") << ": " << pr.winRatePercent << "%\n"
          << dm << "\U0001F504 " << tr(lang, "rk_trades") << ": " << pr.closedTrades << "\n";
    } else {
        t << ": " << tr(lang, "wl_not_ranked") << "\n";
    }

    t << "\n" << HL_CARD_SEPARATOR << "\n\n";

    std::vector<OpenPosition> pos;
    long long accountValue = 0;
    std::vector<std::pair<std::string, long long>> dexAccounts;
    if (!fetchOpenPositions(addr, pos, accountValue, dexAccounts)) {
        t << dm << tr(lang, "generic_error_retry");
        return {t.str(), kb.dump()};
    }

    if (!dexAccounts.empty()) {
        t << dm << "\U0001F3E6 <b>" << tr(lang, "hl_account") << "</b>\n";
        for (const auto& d : dexAccounts) {
            t << dm << (d.first.empty() ? "Perps" : d.first) << ": <b>"
              << fmtUsd(d.second) << "</b>\n";
        }
        if (dexAccounts.size() > 1 && accountValue > 0)
            t << dm << "\u03A3 <b>" << fmtUsd(accountValue) << "</b>\n";
        t << "\n";
    }

    if (pos.empty()) {
        t << dm << tr(lang, "hl_no_open_positions");
        return {t.str(), kb.dump()};
    }

    std::sort(pos.begin(), pos.end(), [](const OpenPosition& a, const OpenPosition& b) {
        return a.marginNanos > b.marginNanos;
    });

    for (size_t i = 0; i < pos.size(); i++) {
        const OpenPosition& p = pos[i];
        if (i) t << "\n" << dm << HL_CARD_SEPARATOR << "\n\n";

        t << dm << (p.isLong ? "\U0001F7E2" : "\U0001F534") << " <b>"
          << safeString(p.coin, 32) << "</b>\n";
        t << dm << tr(lang, p.isLong ? "hl_side_long" : "hl_side_short") << ": <b>"
          << fmtUsd(p.marginNanos * (p.leverage > 0 ? p.leverage : 1)) << "</b>";
        if (p.leverage > 0)
            t << " \u00B7 " << p.leverage << "\u00D7 "
              << tr(lang, p.isolated ? "hl_isolated" : "hl_cross");
        t << "\n";
        if (p.marginNanos > 0) {
            t << dm << "\U0001F4B5 " << tr(lang, "hl_collateral") << ": <b>"
              << fmtUsd(p.marginNanos) << "</b>";
            t << "\n";
        }
        if (p.entryPxNanos > 0)
            t << dm << "\U0001F4CD " << tr(lang, "hl_entry_price") << ": <b>"
              << formatPriceNanos(p.entryPxNanos) << "</b>\n";
        if (p.markPxNanos > 0) {
            t << dm << "\U0001F4B9 " << tr(lang, "hl_mark_price") << ": <b>"
              << formatPriceNanos(p.markPxNanos) << "</b>";
            if (p.entryPxNanos > 0) {
                double move = 100.0 * (static_cast<double>(p.markPxNanos) -
                                       static_cast<double>(p.entryPxNanos))
                              / static_cast<double>(p.entryPxNanos);
                if (!p.isLong) move = -move;
                t << " (" << formatPercent(move, true) << ")";
            }
            t << "\n";
        }
        t << dm << (p.unrealizedNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ")
          << tr(lang, "hl_unrealized") << ": <b>"
          << formatUsdNanosSigned(p.unrealizedNanos, true) << "</b>";
        if (p.roePercent != 0.0) t << " (" << formatPercent(p.roePercent, true) << ")";
        t << "\n";
        if (p.liqPxNanos > 0)
            t << dm << "\u2620\uFE0F " << tr(lang, "hl_liq") << ": <b>"
              << formatPriceNanos(p.liqPxNanos) << "</b>\n";
        const long long av = p.dexAccountNanos > 0 ? p.dexAccountNanos : accountValue;
        if (av > 0 && p.marginNanos > 0) {
            const double share = 100.0 * static_cast<double>(p.marginNanos)
                                        / static_cast<double>(av);
            t << dm << "\U0001F3E6 " << (p.dex.empty() ? "Perps" : p.dex) << ": <b>"
              << fmtUsd(av) << "</b> — "
              << formatPercent(share, false) << " " << tr(lang, "hl_in_position") << "\n";
        }
    }
    return {t.str(), kb.dump()};
}

bool renderHyperliquidView(const std::string& chatId, const std::string& action,
                           const std::string& param, HlMessage& out) {
    if ((action == "hl_menu" || action == "hl_open" || action == "hl_page") &&
        !isPremium(chatId)) {
        out = buildPerpLocked(chatId);
        return true;
    }
    if (action == "hl_positions") { out = buildPositionsPicker(chatId, 1); return true; }
    if (action == "hl_pospage") {
        int page = 1;
        try { page = std::stoi(param); } catch (...) {}
        out = buildPositionsPicker(chatId, page);
        return true;
    }
    if (action == "hl_pos") { out = buildWalletPositions(chatId, param); return true; }
    if (action == "hl_menu") { out = buildPerpTopMenu(chatId); return true; }
    if (action == "hl_open") {
        std::string kindStr = param;
        int windowDays = 30;
        const size_t sep = param.find(':');
        if (sep != std::string::npos) {
            kindStr = param.substr(0, sep);
            try { windowDays = std::stoi(param.substr(sep + 1)); } catch (...) {}
        }
        PerpKind kind;
        if (!parsePerpKind(kindStr, kind)) return false;
        out = renderPerpPage(chatId, kind, 1, windowDays);
        return true;
    }
    if (action == "hl_page") {
        const size_t sep = param.find(':');
        if (sep == std::string::npos) return false;
        PerpKind kind;
        if (!parsePerpKind(param.substr(0, sep), kind)) return false;
        std::string rest = param.substr(sep + 1);
        int page = 1;
        int windowDays = 30;
        const size_t sep2 = rest.find(':');
        try {
            if (sep2 == std::string::npos) page = std::stoi(rest);
            else {
                page = std::stoi(rest.substr(0, sep2));
                windowDays = std::stoi(rest.substr(sep2 + 1));
            }
        } catch (...) {}
        out = renderPerpPage(chatId, kind, page, windowDays);
        return true;
    }
    return false;
}

bool handleHyperliquidCallback(const std::string& chatId, const std::string& action,
                               const std::string& param, const std::string& data,
                               long long messageId, const std::string& callbackQueryId) {
    if (action == "hl_posnoop") {
        if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId);
        return true;
    }

    HlMessage msg;
    try {
        if (!renderHyperliquidView(chatId, action, param, msg)) return false;
    } catch (const std::exception& e) {
        std::cerr << "[HL] ошибка при построении экрана " << action << ": " << e.what() << std::endl;
        const Lang lang = langFromCode(getUserLanguage(chatId));
        msg.text = tr(lang, "generic_error_retry");
        json kb;
        kb["inline_keyboard"] = json::array();
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
        }));
        msg.keyboard = kb.dump();
    }
    rememberView(chatId, data);
    replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId);
    return true;
}
