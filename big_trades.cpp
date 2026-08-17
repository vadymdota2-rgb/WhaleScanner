#include "big_trades.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <sqlite3.h>

#include "json.hpp"
#include "utils.h"
#include "alert_settings.h"
#include "premium.h"
#include "tx_analyzer.h"
#include "wallet_menu.h"
#include "hyperliquid_internal.h"

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;

std::string getUserLanguage(const std::string& chatId);
void rememberView(const std::string& chatId, const std::string& data);

namespace {

constexpr int PER_PAGE   = 5;
constexpr int MAX_ROWS   = 30;
constexpr int FREE_ROWS  = 10;

long long windowSeconds(const std::string& w) {
    if (w == "1h") return 3600LL;
    if (w == "7d") return 7LL * 86400LL;
    return 86400LL;
}

const char* windowKey(const std::string& w) {
    if (w == "1h") return "big_win_1h";
    if (w == "7d") return "big_win_7d";
    return "big_win_24h";
}

struct BigRow {
    std::string wallet, asset, side;
    long long usdNanos = 0;
    long long ts = 0;
    int leverage = 0;
};

std::vector<BigRow> perpRows(long long sinceSec, int limit) {
    std::vector<BigRow> out;
    std::lock_guard<std::mutex> l(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return out;
    sqlite3_stmt* s;
    if (!prepareOrLog(hl::g_hlDb, &s,
        "SELECT wallet, coin, dir, notional_nanos, leverage, ts FROM hl_fills "
        "WHERE ts >= ? AND notional_nanos > 0 "
        "AND wallet NOT IN (SELECT wallet FROM hl_banned) "
        "ORDER BY notional_nanos DESC LIMIT ?")) return out;
    sqlite3_bind_int64(s, 1, sinceSec * 1000LL);
    sqlite3_bind_int(s, 2, limit);
    while (sqlite3_step(s) == SQLITE_ROW) {
        BigRow r;
        r.wallet   = safeColumnText(s, 0);
        r.asset    = safeColumnText(s, 1);
        r.side     = safeColumnText(s, 2);
        r.usdNanos = sqlite3_column_int64(s, 3);
        r.leverage = sqlite3_column_int(s, 4);
        r.ts       = sqlite3_column_int64(s, 5) / 1000;
        out.push_back(std::move(r));
    }
    sqlite3_finalize(s);
    return out;
}

std::vector<BigRow> spotRows(long long sinceSec, int limit) {
    std::vector<BigRow> out;
    std::lock_guard<std::mutex> l(dbMutex);
    if (!db) return out;
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT t.wallet, t.token, t.is_buy, t.usd_nanos, t.timestamp FROM trades t "
        "WHERE t.timestamp >= ? AND t.usd_nanos > 0 "
        "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw "
        "                WHERE iw.wallet = t.wallet AND iw.permanent = 1) "
        "ORDER BY t.usd_nanos DESC LIMIT ?")) return out;
    sqlite3_bind_int64(s, 1, sinceSec);
    sqlite3_bind_int(s, 2, limit);
    while (sqlite3_step(s) == SQLITE_ROW) {
        BigRow r;
        r.wallet   = safeColumnText(s, 0);
        r.asset    = safeColumnText(s, 1);
        r.side     = sqlite3_column_int(s, 2) ? "Buy" : "Sell";
        r.usdNanos = sqlite3_column_int64(s, 3);
        r.ts       = sqlite3_column_int64(s, 4);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(s);
    return out;
}

json backRow(Lang lang, const std::string& data) {
    return json::array({ {{"text", tr(lang, "back_button")}, {"callback_data", data}} });
}

}

BigTradesMessage buildBigMenu(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    std::ostringstream t;
    t << "\U0001F525 <b>" << tr(lang, "big_title") << "</b>\n\n"
      << tr(lang, "big_menu_hint");

    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "big_btn_spot")}, {"callback_data", "bg_open:spot:24h"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "big_btn_perp")}, {"callback_data", "bg_open:perp:24h"}}
    }));
    kb["inline_keyboard"].push_back(backRow(lang, "menu:main"));
    return {t.str(), kb.dump()};
}

BigTradesMessage buildBigList(const std::string& chatId, const std::string& venue,
                              const std::string& window, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    const bool perp = venue == "perp";
    const bool premium = isPremium(chatId);
    const int maxRows = premium ? MAX_ROWS : FREE_ROWS;

    const long long since = static_cast<long long>(time(nullptr)) - windowSeconds(window);
    std::vector<BigRow> rows = perp ? perpRows(since, maxRows) : spotRows(since, maxRows);

    std::ostringstream t;
    t << "\U0001F525 <b>" << tr(lang, perp ? "big_perp_title" : "big_spot_title") << "</b>\n"
      << "<i>" << tr(lang, windowKey(window)) << "</i>\n\n";

    json kb;
    kb["inline_keyboard"] = json::array();

    if (rows.empty()) {
        t << tr(lang, "big_empty");
    } else {
        const int total = static_cast<int>(rows.size());
        const int pages = std::max(1, (total + PER_PAGE - 1) / PER_PAGE);
        if (page < 1) page = 1;
        if (page > pages) page = pages;
        const int from = (page - 1) * PER_PAGE;
        const int to   = std::min(from + PER_PAGE, total);

        for (int i = from; i < to; i++) {
            const BigRow& r = rows[i];
            const bool up = r.side.find("Long") != std::string::npos ||
                            r.side.find("Buy")  != std::string::npos;
            t << "<b>" << (i + 1) << ".</b> " << (up ? "\U0001F7E2" : "\U0001F534")
              << " <b>" << safeString(r.asset, 16) << "</b>\n"
              << "\U0001F4B0 " << formatUsd(static_cast<cpp_int>(r.usdNanos));
            if (perp && r.leverage > 0) t << " \u00B7 " << r.leverage << "\u00D7";
            t << "\n<code>" << shortAddress(r.wallet) << "</code>\n\n";

            if (perp)
                kb["inline_keyboard"].push_back(json::array({
                    {{"text", std::to_string(i + 1) + ". " + tr(lang, "big_positions_btn")},
                     {"callback_data", "hl_pos:" + r.wallet}}
                }));
            else
                kb["inline_keyboard"].push_back(json::array({
                    {{"text", std::to_string(i + 1) + ". " + tr(lang, "big_track_btn")},
                     {"callback_data", "tt_track:" + r.wallet}}
                }));
        }

        if (!premium) t << "<i>" << tr(lang, "big_free_note") << "</i>\n\n";

        json nav = json::array();
        if (page > 1)
            nav.push_back({{"text", "\u2B05\uFE0F"},
                           {"callback_data", "bg_page:" + venue + ":" + window + ":" + std::to_string(page - 1)}});
        nav.push_back({{"text", std::to_string(page) + "/" + std::to_string(pages)},
                       {"callback_data", "bg_noop"}});
        if (page < pages)
            nav.push_back({{"text", "\u27A1\uFE0F"},
                           {"callback_data", "bg_page:" + venue + ":" + window + ":" + std::to_string(page + 1)}});
        if (nav.size() > 1) kb["inline_keyboard"].push_back(nav);
    }

    json wins = json::array();
    for (const char* w : {"1h", "24h", "7d"}) {
        const bool cur = window == w;
        wins.push_back({{"text", std::string(cur ? "\u2705 " : "") + tr(lang, windowKey(w))},
                        {"callback_data", cur ? "bg_noop" : "bg_open:" + venue + ":" + w}});
    }
    kb["inline_keyboard"].push_back(wins);
    kb["inline_keyboard"].push_back(backRow(lang, "menu:big"));
    return {t.str(), kb.dump()};
}

bool renderBigTradesView(const std::string& chatId, const std::string& action,
                         const std::string& param, BigTradesMessage& out) {
    if (action == "bg_open") {
        const size_t sep = param.find(':');
        if (sep == std::string::npos) return false;
        out = buildBigList(chatId, param.substr(0, sep), param.substr(sep + 1), 1);
        return true;
    }
    if (action == "bg_page") {
        const size_t a = param.find(':');
        if (a == std::string::npos) return false;
        const size_t b = param.find(':', a + 1);
        if (b == std::string::npos) return false;
        int page = 1;
        try { page = std::stoi(param.substr(b + 1)); } catch (...) {}
        out = buildBigList(chatId, param.substr(0, a), param.substr(a + 1, b - a - 1), page);
        return true;
    }
    return false;
}

bool handleBigTradesCallback(const std::string& chatId, const std::string& action,
                             const std::string& param, const std::string& data,
                             long long messageId, const std::string& callbackQueryId) {
    if (action == "bg_noop") {
        if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId);
        return true;
    }
    BigTradesMessage msg;
    if (!renderBigTradesView(chatId, action, param, msg)) return false;
    rememberView(chatId, data);
    replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId);
    return true;
}
