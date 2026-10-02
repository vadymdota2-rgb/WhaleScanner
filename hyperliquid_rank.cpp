#include "hyperliquid.h"
#include "ranking.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "utils.h"

/*
 * Рейтинг фьючерсов Hyperliquid в сканере. Сам рейтинг показывает
 * приложение (whale_api.py считает его по тем же сделкам); здесь он нужен,
 * чтобы раз в 15 минут отмечать, кто из кошельков сегодня в рейтинге
 * (rank_presence) — по этим отметкам приложение видит «дней в рейтинге».
 */

#include "hyperliquid_internal.h"

using namespace hl;

namespace {

constexpr int HL_MIN_CLOSED_TRADES = 5;
constexpr int HL_MAX_CLOSED_TRADES_30D = 200;  // как спот: >200/30д = бот
constexpr long long HL_RANK_INTERVAL_SEC = 15 * 60;

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
long long g_rankBuiltAt = 0;

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
        if (lev > 0) ser.lev = lev;

        /* Маржа сделки — закрытый номинал, делённый на плечо. Считается на
         * каждом закрытии, в том числе частичном.
         *
         * Раньше она прибавлялась только там, где серия заканчивалась
         * (flat=1 или переворот с ликвидацией), а прибыль серии складывалась
         * со всех её филов. Кто выходит из позиции частями — а так торгует
         * большинство, — получал знаменатель от одного последнего куска и
         * доходность во столько раз выше, на сколько частей разбит выход: у
         * 0x767a…ace выходило 31660% вместо 188%.
         *
         * Закрыто не больше, чем стояло в позиции: у переворота (лонг в шорт)
         * половина объёма открывает новую, и закрытой она не была. Долив в ту
         * же сторону не закрывает ничего и в знаменатель не идёт. */
        long long shut = 0;
        if (startKnown) {
            if (start > 0 && signedSz < 0) shut = (-signedSz < start) ? -signedSz : start;
            else if (start < 0 && signedSz > 0) shut = (signedSz < -start) ? signedSz : -start;
        } else if (flat == 1 || dir >= DIR_CLOSE_LONG || pnl != 0) {
            shut = sz;               /* размера позиции нет — строка закрывающая целиком */
        }
        if (shut > 0) {
            long long closedNtl = 0;
            if (px > 0)
                closedNtl = static_cast<long long>(
                    (static_cast<__int128>(shut) * static_cast<__int128>(px)) / 1000000000LL);
            else if (sz > 0)
                closedNtl = static_cast<long long>(
                    (static_cast<__int128>(notional) * static_cast<__int128>(shut)) / sz);
            int effLev = lev > 0 ? lev : ser.lev;
            if (effLev <= 0 && a.levN > 0)
                effLev = static_cast<int>((a.levSum + a.levN / 2) / a.levN);
            if (effLev > 0 && closedNtl > 0) a.margin += closedNtl / effLev;
            else if (margin > 0) a.margin += margin;
        }

        /* Сделка засчитывается на каждом закрытии, а не только там, где
         * позиция обнулилась. Прежде серия сливалась в счёт лишь по флагу
         * flat (или перевороту с ликвидацией): у того, кто выходит частями,
         * за 30 дней набиралось одно закрытие вместо полусотни, накопленная
         * прибыль одной монеты приписывалась ему целиком, а прибыль второй,
         * не успевшей закрыться, пропадала вовсе — у 0x767a…ace выходило
         * $1,67M вместо $1,43M на одной «сделке». Теперь числитель,
         * знаменатель и счётчик сделок считают одни и те же строки, и бот с
         * приложением показывают одно и то же. Филы одной заявки по-прежнему
         * склеиваются по oid и сделкой считаются одной. */
        if (shut <= 0 && flat != 1 && dir < DIR_FLIP) continue;

        const long long id = oid > 0 ? oid : tid;
        if (id != 0 && id == ser.closeOid) {
            a.closedPnl += ser.pnl;
        } else {
            closeTrade(a);
            ser.closeOid = id;
        }
        ser.pnl = 0;
        ser.fee = 0;
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

namespace hl {
void rebuildRankCache() {
    const long long now = nowSec();
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        if (g_rankBuiltAt > 0 && now - g_rankBuiltAt < HL_RANK_INTERVAL_SEC) return;
    }
    bool ok = false;
    std::vector<PerpRow> fresh = computeRanking(30LL * 86400LL, ok);
    if (!ok) return;
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        g_rankBuiltAt = now;
    }
    std::vector<std::string> top;
    top.reserve(fresh.size());
    for (const auto& r : fresh) top.push_back(r.wallet);
    if (!top.empty())
        markRankPresence("perp", top);
}

void invalidateRankCache() {
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        g_rankBuiltAt = 0;
    }
    std::cout << "[HL] рейтинг сброшен, пересчёт при следующем проходе" << std::endl;
}

}

void invalidateRankCache() {
    hl::invalidateRankCache();
}
