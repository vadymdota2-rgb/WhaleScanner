#include "hyperliquid.h"
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
constexpr int HL_PER_PAGE = 5;
constexpr long long HL_RANK_CACHE_SEC = 300;

struct PerpRow {
    std::string wallet;
    long long pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int closedTrades = 0;
    double avgLeverage = 0.0;
    bool roiKnown = false;
    long long volumeNanos = 0;
    long long lastTs = 0;
};

std::mutex g_rankMutex;
long long g_rankBuiltAt = 0;
std::vector<PerpRow> g_rankCache;

std::vector<PerpRow> computeRanking(bool& ok) {
    ok = false;
    std::vector<PerpRow> rows;
    const long long sinceMs = (nowSec() - HL_RANK_WINDOW_SEC) * 1000LL;

    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return rows;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT wallet,"
            " SUM(closed_pnl_nanos),"
            " SUM(CASE WHEN closed_pnl_nanos != 0 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN closed_pnl_nanos > 0 THEN 1 ELSE 0 END),"
            " SUM(notional_nanos),"
            " AVG(CASE WHEN leverage > 0 THEN leverage END),"
            " AVG(CASE WHEN account_value_nanos > 0 THEN account_value_nanos END),"
            " MAX(ts)"
            " FROM hl_fills f WHERE f.ts >= ?"
            " AND NOT EXISTS (SELECT 1 FROM hl_banned b WHERE b.wallet = f.wallet)"
            " GROUP BY wallet"))
        return rows;
    sqlite3_bind_int64(s, 1, sinceMs);

    int stepRc;
    while ((stepRc = sqlite3_step(s)) == SQLITE_ROW) {
        PerpRow r;
        r.wallet = safeColumnText(s, 0);
        r.pnlNanos = sqlite3_column_int64(s, 1);
        r.closedTrades = sqlite3_column_int(s, 2);
        const int wins = sqlite3_column_int(s, 3);
        r.volumeNanos = sqlite3_column_int64(s, 4);
        r.avgLeverage = sqlite3_column_double(s, 5);
        const double avgAccount = sqlite3_column_double(s, 6);
        r.lastTs = sqlite3_column_int64(s, 7);

        if (r.closedTrades < HL_MIN_CLOSED_TRADES) continue;
        r.winRatePercent = static_cast<int>((100LL * wins) / r.closedTrades);
        if (avgAccount > 0.0) {
            r.roiPercent = 100.0 * static_cast<double>(r.pnlNanos) / avgAccount;
            r.roiKnown = true;
        }
        rows.push_back(r);
    }
    const bool complete = stepRc == SQLITE_DONE;
    sqlite3_finalize(s);
    if (!complete) {
        std::cerr << "[HL] рейтинг: чтение прервано, старый кэш сохранён" << std::endl;
        return {};
    }
    ok = true;
    return rows;
}



void rankingSnapshot(std::vector<PerpRow>& localCopy) {
    std::lock_guard<std::mutex> l(g_rankMutex);
    localCopy = g_rankCache;
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
        case PerpKind::ROI:     return "\U0001F4C8 " + tr(lang, "rk_top_roi_30d");
        case PerpKind::WINRATE: return "\U0001F3AF " + tr(lang, "rk_top_winrate_30d");
        case PerpKind::ACTIVE:  return "\U0001F504 " + tr(lang, "rk_most_active_30d");
        default:                return "\U0001F4B5 " + tr(lang, "rk_top_pnl_30d");
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

HlMessage renderPerpPage(const std::string& chatId, PerpKind kind, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    int maxRank = premiumTopTradersLimit(chatId);
    if (maxRank < 1) maxRank = 1;
    const bool showUpgrade = !isPremium(chatId);

    std::vector<PerpRow> rows;
    rankingSnapshot(rows);
    sortByKind(rows, kind);

    const int visible = std::min(static_cast<int>(rows.size()), maxRank);
    const int totalPages = std::max(1, (visible + HL_PER_PAGE - 1) / HL_PER_PAGE);
    page = std::max(1, std::min(page, totalPages));
    const int startIdx = (page - 1) * HL_PER_PAGE;
    const int endIdx = std::min(visible, startIdx + HL_PER_PAGE);

    const char* const dm = dirMark(lang);

    std::stringstream text;
    text << dm << "\U0001F535 <b>Hyperliquid \u2014 " << perpTitle(kind, lang) << "</b>\n\n";

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
                text << dm << "\U0001F4C8 <b>" << tr(lang, "hl_rk_roi_account") << ":</b> " << formatPercent(r.roiPercent, true) << "\n";
            text << dm << "\U0001F3AF <b>" << tr(lang, "ws_winrate") << ":</b> " << r.winRatePercent << "%\n";
            text << dm << "\U0001F504 <b>" << tr(lang, "rk_trades") << ":</b> " << r.closedTrades << "\n";
            if (r.avgLeverage > 0.0) {
                text << dm << "\u2699\uFE0F <b>" << tr(lang, "hl_rk_leverage") << ":</b> "
                     << static_cast<int>(r.avgLeverage + 0.5) << "\u00D7\n";
            }
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
    json navRow = json::array();
    if (page > 1)
        navRow.push_back({{"text", "\u2B05\uFE0F"}, {"callback_data", "hl_page:" + kindParam + ":" + std::to_string(page - 1)}});
    navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "tt_noop"}});
    if (page < totalPages)
        navRow.push_back({{"text", "\u27A1\uFE0F"}, {"callback_data", "hl_page:" + kindParam + ":" + std::to_string(page + 1)}});
    keyboard["inline_keyboard"].push_back(navRow);
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
    { std::lock_guard<std::mutex> l(g_queueMutex); queued = g_enrichQueue.size(); }

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

    ss << "\n\n\U0001F916 <b>Фильтры</b>"
       << "\n\u2022 ботов исключено всего: " << formatThousands(static_cast<uint64_t>(bannedTotal));
    ss << "\n\u2022 пропущено по лимиту API: " << formatThousands(skips);
    if (recon > 0) ss << "\n\u2022 обрывов связи: " << recon;

    return ss.str();
}

namespace hl {
void rebuildRankCache() {
    bool ok = false;
    std::vector<PerpRow> fresh = computeRanking(ok);
    std::lock_guard<std::mutex> l(g_rankMutex);
    if (ok) {
        g_rankCache.swap(fresh);
        g_rankBuiltAt = nowSec();
    }
}

void invalidateRankCache() {
    std::lock_guard<std::mutex> l(g_rankMutex);
    g_rankBuiltAt = 0;
    g_rankCache.clear();
}
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
    bool isLong = false;
    long long sizeNanos = 0;
    long long entryPxNanos = 0;
    long long markPxNanos = 0;
    long long liqPxNanos = 0;
    long long marginNanos = 0;
    long long unrealizedNanos = 0;
    double roePercent = 0.0;
    int leverage = 0;
    bool isolated = false;
};

bool fetchOpenPositions(const std::string& wallet, std::vector<OpenPosition>& out) {
    json body;
    body["type"] = "clearinghouseState";
    body["user"] = wallet;
    body["dex"] = "";
    json j = infoPost(body, HL_WEIGHT_CLEARINGHOUSE);
    if (!j.is_object() || !j.contains("assetPositions") || !j["assetPositions"].is_array())
        return false;

    for (const auto& ap : j["assetPositions"]) {
        if (!ap.is_object() || !ap.contains("position") || !ap["position"].is_object()) continue;
        const json& p = ap["position"];

        OpenPosition op;
        op.wallet = wallet;
        op.coin = jstr(p, "coin");
        if (op.coin.empty()) continue;

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
    t << HL_CARD_SEPARATOR << "\n\n";

    std::vector<OpenPosition> pos;
    if (!fetchOpenPositions(addr, pos)) {
        t << dm << tr(lang, "generic_error_retry");
        return {t.str(), kb.dump()};
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
        if (p.marginNanos > 0)
            t << dm << "\U0001F4B5 " << tr(lang, "hl_collateral") << ": <b>"
              << fmtUsd(p.marginNanos) << "</b>\n";
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
        PerpKind kind;
        if (!parsePerpKind(param, kind)) return false;
        out = renderPerpPage(chatId, kind, 1);
        return true;
    }
    if (action == "hl_page") {
        const size_t sep = param.find(':');
        if (sep == std::string::npos) return false;
        PerpKind kind;
        if (!parsePerpKind(param.substr(0, sep), kind)) return false;
        int page = 1;
        try { page = std::stoi(param.substr(sep + 1)); } catch (...) {}
        out = renderPerpPage(chatId, kind, page);
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
    return true;
}
