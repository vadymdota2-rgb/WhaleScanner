#include "big_trades.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
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

constexpr int PER_PAGE  = 5;
constexpr int MAX_ROWS  = 30;
constexpr int FREE_ROWS = 10;
constexpr long long MAX_SPOT_USD_NANOS = 10000000000000000LL;

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

const char* hlDirKey(const std::string& dir) {
    if (dir.find("Open Long")   != std::string::npos) return "hl_open_long";
    if (dir.find("Open Short")  != std::string::npos) return "hl_open_short";
    if (dir.find("Long")  != std::string::npos) return "hl_open_long";
    if (dir.find("Short") != std::string::npos) return "hl_open_short";
    return "hl_open_long";
}

bool hlSideUp(const std::string& dir) {
    const char* k = hlDirKey(dir);
    return std::string(k) == "hl_open_long";
}

struct BigRow {
    std::string wallet;
    std::string asset;
    std::string side;
    std::string amountStr;
    std::string pxStr;
    std::string txHash;
    long long usdNanos = 0;
    long long closedPnlNanos = 0;
    long long marginNanos = 0;
    long long accountValueNanos = 0;
    int leverage = 0;
    bool isBuy = false;
};

std::vector<BigRow> perpRows(long long sinceSec, int limit) {
    std::vector<BigRow> out;
    std::lock_guard<std::mutex> l(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return out;
    sqlite3_stmt* s;
    if (!prepareOrLog(hl::g_hlDb, &s,
        "SELECT wallet, coin, dir, notional_nanos, leverage, px, sz, "
        "       closed_pnl_nanos, margin_nanos, account_value_nanos "
        "FROM hl_fills "
        "WHERE ts >= ? AND notional_nanos > 0 "
        "AND (dir LIKE '%Open Long%' OR dir LIKE '%Open Short%') "
        "AND wallet NOT IN (SELECT wallet FROM hl_banned) "
        "GROUP BY wallet "
        "HAVING notional_nanos = MAX(notional_nanos) "
        "ORDER BY notional_nanos DESC LIMIT ?")) return out;
    sqlite3_bind_int64(s, 1, sinceSec * 1000LL);
    sqlite3_bind_int(s, 2, limit);
    while (sqlite3_step(s) == SQLITE_ROW) {
        BigRow r;
        r.wallet             = safeColumnText(s, 0);
        r.asset              = safeColumnText(s, 1);
        r.side               = safeColumnText(s, 2);
        r.usdNanos           = sqlite3_column_int64(s, 3);
        r.leverage           = sqlite3_column_int(s, 4);
        r.pxStr              = safeColumnText(s, 5);
        r.amountStr          = safeColumnText(s, 6);
        r.closedPnlNanos     = sqlite3_column_int64(s, 7);
        r.marginNanos        = sqlite3_column_int64(s, 8);
        r.accountValueNanos  = sqlite3_column_int64(s, 9);
        r.isBuy              = hlSideUp(r.side);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(s);
    return out;
}

std::vector<BigRow> liqRows(long long sinceSec, int limit) {
    std::vector<BigRow> out;
    std::lock_guard<std::mutex> l(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return out;
    sqlite3_stmt* s;
    if (!prepareOrLog(hl::g_hlDb, &s,
        "SELECT wallet, coin, dir, notional_nanos, leverage, px, sz, "
        "       closed_pnl_nanos, margin_nanos, account_value_nanos "
        "FROM hl_fills "
        "WHERE ts >= ? AND notional_nanos > 0 "
        "AND dir LIKE '%Liquidat%' "
        "AND wallet NOT IN (SELECT wallet FROM hl_banned) "
        "GROUP BY wallet "
        "HAVING notional_nanos = MAX(notional_nanos) "
        "ORDER BY notional_nanos DESC LIMIT ?")) return out;
    sqlite3_bind_int64(s, 1, sinceSec * 1000LL);
    sqlite3_bind_int(s, 2, limit);
    while (sqlite3_step(s) == SQLITE_ROW) {
        BigRow r;
        r.wallet             = safeColumnText(s, 0);
        r.asset              = safeColumnText(s, 1);
        r.side               = safeColumnText(s, 2);
        r.usdNanos           = sqlite3_column_int64(s, 3);
        r.leverage           = sqlite3_column_int(s, 4);
        r.pxStr              = safeColumnText(s, 5);
        r.amountStr          = safeColumnText(s, 6);
        r.closedPnlNanos     = sqlite3_column_int64(s, 7);
        r.marginNanos        = sqlite3_column_int64(s, 8);
        r.accountValueNanos  = sqlite3_column_int64(s, 9);
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
        "SELECT t.wallet, t.token, t.is_buy, t.usd_nanos, t.token_amount, t.tx_hash "
        "FROM trades t "
        "WHERE t.timestamp >= ? AND t.usd_nanos > 0 "
        "AND t.usd_nanos <= ? "
        "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw "
        "                WHERE iw.wallet = t.wallet AND iw.permanent = 1) "
        "GROUP BY t.wallet "
        "HAVING t.usd_nanos = MAX(t.usd_nanos) "
        "ORDER BY t.usd_nanos DESC LIMIT ?")) return out;
    sqlite3_bind_int64(s, 1, sinceSec);
    sqlite3_bind_int64(s, 2, MAX_SPOT_USD_NANOS);
    sqlite3_bind_int(s, 3, limit);
    while (sqlite3_step(s) == SQLITE_ROW) {
        BigRow r;
        r.wallet    = safeColumnText(s, 0);
        r.asset     = safeColumnText(s, 1);
        r.isBuy     = sqlite3_column_int(s, 2) != 0;
        r.side      = r.isBuy ? "Buy" : "Sell";
        r.usdNanos  = sqlite3_column_int64(s, 3);
        r.amountStr = safeColumnText(s, 4);
        r.txHash    = safeColumnText(s, 5);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(s);
    return out;
}

json backRow(Lang lang, const std::string& data) {
    return json::array({ {{"text", tr(lang, "back_button")}, {"callback_data", data}} });
}

std::string spotAssetLabel(const std::string& tokenAddr) {
    std::string sym = getSymbol(tokenAddr);
    if (sym.empty()) return shortAddress(tokenAddr);
    return safeString(sym, 16);
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
    {
        std::string perpBtn = tr(lang, "big_btn_perp");
        if (!isPremium(chatId)) perpBtn += " \U0001F512";
        kb["inline_keyboard"].push_back(json::array({
            {{"text", perpBtn}, {"callback_data", "bg_open:perp:24h"}}
        }));
    }
    {
        std::string liqBtn = tr(lang, "big_btn_liq");
        if (!isPremium(chatId)) liqBtn += " \U0001F512";
        kb["inline_keyboard"].push_back(json::array({
            {{"text", liqBtn}, {"callback_data", "bg_open:liq:24h"}}
        }));
    }
    kb["inline_keyboard"].push_back(backRow(lang, "menu:main"));
    return {t.str(), kb.dump()};
}

BigTradesMessage buildBigList(const std::string& chatId, const std::string& venue,
                              const std::string& window, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    const bool liq  = venue == "liq";
    const bool perp = venue == "perp" || liq;
    const bool premium = isPremium(chatId);

    if (perp && !premium) {
        std::ostringstream t;
        t << "\U0001F525 <b>" << tr(lang, liq ? "big_liq_title" : "big_perp_title") << "</b>\n\n"
          << "\U0001F512 " << tr(lang, "hl_locked_body");
        json kb;
        kb["inline_keyboard"] = json::array();
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
        }));
        kb["inline_keyboard"].push_back(backRow(lang, "menu:big"));
        return {t.str(), kb.dump()};
    }

    const int maxRows = premium ? MAX_ROWS : FREE_ROWS;

    const long long since = static_cast<long long>(time(nullptr)) - windowSeconds(window);
    std::vector<BigRow> rows = liq  ? liqRows(since, maxRows)
                             : perp ? perpRows(since, maxRows)
                                    : spotRows(since, maxRows);
    if (static_cast<int>(rows.size()) > maxRows)
        rows.resize(static_cast<size_t>(maxRows));

    std::ostringstream t;
    t << "\U0001F525 <b>" << tr(lang, liq ? "big_liq_title" : perp ? "big_perp_title" : "big_spot_title") << "</b>\n"
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

            if (perp) {
                const char* dk = hlDirKey(r.side);
                const bool up = hlSideUp(r.side);
                const std::string coin = safeString(r.asset.empty() ? "?" : r.asset, 16);

                t << "<b>" << (i + 1) << ".</b> "
                  << (up ? "\U0001F7E2" : "\U0001F534") << " <b>"
                  << tr(lang, dk) << " " << coin << "</b>\n";
                t << "\U0001F4B0 " << tr(lang, "hl_trade_size") << ": <b>"
                  << formatUsd(cpp_int(r.usdNanos)) << "</b>";
                if (r.leverage > 0)
                    t << " \u00B7 " << r.leverage << "\u00D7";
                t << "\n";
                if (r.marginNanos > 0) {
                    t << "\U0001F4B5 " << tr(lang, "hl_collateral") << ": <b>"
                      << formatUsd(cpp_int(r.marginNanos)) << "</b>";
                    if (r.accountValueNanos > 0) {
                        const double share = 100.0 * static_cast<double>(r.marginNanos)
                                                   / static_cast<double>(r.accountValueNanos);
                        t << " <i>(" << formatPercent(share, false) << " "
                          << tr(lang, "hl_of_account") << ")</i>";
                    }
                    t << "\n";
                }
                if (!r.pxStr.empty())
                    t << "\U0001F4CD " << tr(lang, "hl_price") << ": <b>"
                      << safeString(r.pxStr, 24) << "</b>\n";
                if (!r.amountStr.empty())
                    t << "\U0001F4E6 " << tr(lang, "hl_qty") << ": <b>"
                      << safeString(r.amountStr, 24) << " " << coin << "</b>\n";
                if (r.accountValueNanos > 0)
                    t << "\U0001F3E6 " << tr(lang, "hl_account") << ": <b>"
                      << formatUsd(cpp_int(r.accountValueNanos)) << "</b>\n";
                if (r.closedPnlNanos != 0)
                    t << (r.closedPnlNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ")
                      << tr(lang, "hl_pnl") << ": <b>"
                      << formatUsdNanosSigned(r.closedPnlNanos, true) << "</b>\n";
                t << "\U0001F4BC <code>" << r.wallet << "</code>\n\n";

                const std::string btn = std::to_string(i + 1) + ". " + coin + " \u00B7 "
                                      + tr(lang, "big_track_btn");
                kb["inline_keyboard"].push_back(json::array({
                    {{"text", btn}, {"callback_data", "tt_track:" + r.wallet}}
                }));
            } else {
                const std::string sym = spotAssetLabel(r.asset);
                const int dec = getDecimals(r.asset);
                cpp_int raw = 0;
                try {
                    if (!r.amountStr.empty()) raw = cpp_int(r.amountStr);
                } catch (...) { raw = 0; }

                t << "<b>" << (i + 1) << ".</b> "
                  << (r.isBuy ? "\U0001F7E2" : "\U0001F534") << " <b>"
                  << tr(lang, r.isBuy ? "alert_buy" : "alert_sell") << "</b>\n";
                t << "\U0001F4B0 " << tr(lang, "alert_amount") << ": <b>"
                  << formatUsd(cpp_int(r.usdNanos)) << "</b>\n";
                t << "\U0001FA99 " << tr(lang, "alert_token") << ": <b>"
                  << sym << "</b>\n";
                if (raw > 0)
                    t << "\U0001F4E6 " << tr(lang, "alert_qty") << ": <b>"
                      << formatAmount(raw, dec) << "</b>\n";
                if (raw > 0 && r.usdNanos > 0) {
                    const cpp_int unit = calcUnitPriceNanos(cpp_int(r.usdNanos), raw, dec);
                    if (unit > 0)
                        t << "\U0001F4B5 "
                          << tr(lang, r.isBuy ? "alert_buy_price" : "alert_sell_price")
                          << ": <b>" << formatPriceUsd(unit) << "</b>\n";
                }
                t << (r.isBuy ? "\U0001F4C9 " : "\U0001F4C8 ")
                  << tr(lang, r.isBuy ? "alert_spent" : "alert_received")
                  << ": <b>" << formatUsd(cpp_int(r.usdNanos)) << "</b>\n";
                if (!r.asset.empty())
                    t << "\U0001F4DC " << tr(lang, "alert_contract") << ": <code>"
                      << safeString(r.asset) << "</code>\n";
                if (!r.txHash.empty())
                    t << "\U0001F194 TX: <code>" << safeString(r.txHash) << "</code>\n";
                t << "\U0001F4BC <code>" << r.wallet << "</code>\n\n";

                const std::string btn = std::to_string(i + 1) + ". " + sym + " \u00B7 "
                                      + tr(lang, "big_track_btn");
                kb["inline_keyboard"].push_back(json::array({
                    {{"text", btn}, {"callback_data", "tt_track:" + r.wallet}}
                }));
            }
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
