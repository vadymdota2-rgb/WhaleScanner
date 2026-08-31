#include "big_trades.h"
#include "token_prices.h"
#include "rpc_client.h"
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdio>
#include <map>

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

struct FundingRow {
    std::string symbol;
    std::string rawSymbol;
    double rate = 0.0;
    int payoutsPerDay = 3;
    double oiUsd = 0.0;
    double volUsd = 0.0;
};

std::mutex g_fundMutex;
std::vector<FundingRow> g_fundCache;
time_t g_fundAt = 0;
std::atomic<bool> g_fundLoading{false};

constexpr double FUNDING_MIN_ABS = 0.001;
constexpr int    FUNDING_MAX_ROWS = 15;
constexpr int    FUNDING_REFRESH_SEC = 240;
constexpr double FUNDING_MIN_VOL_USD = 500000.0;

constexpr int PER_PAGE  = 5;
constexpr int MAX_ROWS  = 30;
constexpr long long MAX_SPOT_USD_NANOS = 10000000000000000LL;

long long windowSeconds(const std::string& w) {
    if (w == "1h") return 3600LL;
    if (w == "7d") return 7LL * 86400LL;
    if (w == "30d") return 30LL * 86400LL;
    return 86400LL;
}

const char* windowKey(const std::string& w) {
    if (w == "1h") return "big_win_1h";
    if (w == "7d") return "big_win_7d";
    if (w == "30d") return "big_win_30d";
    return "big_win_24h";
}

const char* hlDirKey(int code) {
    switch (code) {
        case DIR_LIQ_SHORT:   return "hl_liq_short";
        case DIR_LIQ_LONG:
        case DIR_LIQ_OTHER:   return "hl_liq_long";
        case DIR_OPEN_SHORT:
        case DIR_CLOSE_SHORT: return "hl_open_short";
        default:              return "hl_open_long";
    }
}

bool hlSideUp(int code) { return !(code == DIR_OPEN_SHORT || code == DIR_CLOSE_SHORT ||
                                   code == DIR_LIQ_SHORT); }

struct BigRow {
    std::string wallet;
    std::string asset;
    std::string side;
    int dirCode = 0;
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
        "       closed_pnl_nanos, margin_nanos, account_value_nanos, dir_code "
        "FROM hl_fills "
        "WHERE ts >= ? AND notional_nanos > 0 "
        "AND dir_code IN (1,2) "
        "AND wallet NOT IN (SELECT wallet FROM hl_banned) "
        "GROUP BY wallet "
        "HAVING notional_nanos = MAX(notional_nanos) "
        "ORDER BY notional_nanos DESC LIMIT ?")) return out;
    sqlite3_bind_int64(s, 1, sinceSec * 1000LL);   // hl_fills: время в мс
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
        r.dirCode            = sqlite3_column_int(s, 10);
        r.isBuy              = hlSideUp(r.dirCode);
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
        "       closed_pnl_nanos, margin_nanos, account_value_nanos, dir_code "
        "FROM hl_fills "
        "WHERE ts >= ? AND notional_nanos > 0 "
        "AND dir_code IN (6,7,8) "
        "AND wallet NOT IN (SELECT wallet FROM hl_banned) "
        "GROUP BY wallet "
        "HAVING notional_nanos = MAX(notional_nanos) "
        "ORDER BY notional_nanos DESC LIMIT ?")) return out;
    sqlite3_bind_int64(s, 1, sinceSec * 1000LL);   // hl_fills: время в мс
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
        r.dirCode            = sqlite3_column_int(s, 10);
        r.isBuy              = hlSideUp(r.dirCode);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(s);
    return out;
}

struct FlowRow {
    std::string token;
    long long boughtNanos = 0;
    long long soldNanos = 0;
    int buys = 0;
    int sells = 0;
    int wallets = 0;
};

std::mutex g_listCacheMutex;

template <typename T>
struct ListCache {
    std::map<std::string, std::pair<time_t, std::vector<T>>> byWindow;
};

ListCache<FlowRow> g_flowCache;
ListCache<BigRow>  g_spotCache;

long long cacheTtlFor(const std::string& w) {
    if (w == "1h")  return 60;
    if (w == "24h") return 300;
    if (w == "7d")  return 900;
    return 1800;                     // 30d
}

std::vector<FlowRow> flowRows(long long sinceSec, int limit) {
    std::vector<FlowRow> out;
    std::lock_guard<std::mutex> l(dbMutex);
    if (!db) return out;
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT t.token, "
        "  SUM(CASE WHEN t.is_buy=1 THEN t.usd_nanos ELSE 0 END), "
        "  SUM(CASE WHEN t.is_buy=0 THEN t.usd_nanos ELSE 0 END), "
        "  SUM(CASE WHEN t.is_buy=1 THEN 1 ELSE 0 END), "
        "  SUM(CASE WHEN t.is_buy=0 THEN 1 ELSE 0 END), "
        "  COUNT(DISTINCT t.wallet) "
        "FROM trades t "
        "WHERE t.timestamp >= ? AND t.usd_nanos > 0 AND t.usd_nanos <= ? "
        "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw "
        "                WHERE iw.wallet = t.wallet AND iw.permanent = 1) "
        "GROUP BY t.token "
        "ORDER BY ABS(SUM(CASE WHEN t.is_buy=1 THEN t.usd_nanos ELSE -t.usd_nanos END)) DESC "
        "LIMIT ?")) return out;
    sqlite3_bind_int64(s, 1, sinceSec);
    sqlite3_bind_int64(s, 2, MAX_SPOT_USD_NANOS);
    sqlite3_bind_int(s, 3, limit);
    while (sqlite3_step(s) == SQLITE_ROW) {
        FlowRow r;
        r.token        = safeColumnText(s, 0);
        r.boughtNanos  = sqlite3_column_int64(s, 1);
        r.soldNanos    = sqlite3_column_int64(s, 2);
        r.buys         = sqlite3_column_int(s, 3);
        r.sells        = sqlite3_column_int(s, 4);
        r.wallets      = sqlite3_column_int(s, 5);
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
    sqlite3_bind_int64(s, 1, sinceSec);           // trades: время в секундах
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

bool hasKnownSymbol(const std::string& tokenAddr) {
    const std::string sym = getSymbol(tokenAddr);
    return !sym.empty() && sym != "UNKNOWN";
}

std::string spotAssetLabel(const std::string& tokenAddr) {
    return safeString(getSymbol(tokenAddr), 16);
}

}

namespace {
std::string compactUsd(double v) {
    char b[48];
    if (v >= 1e9)      std::snprintf(b, sizeof(b), "$%.1fB", v / 1e9);
    else if (v >= 1e6) std::snprintf(b, sizeof(b), "$%.1fM", v / 1e6);
    else if (v >= 1e3) std::snprintf(b, sizeof(b), "$%.0fK", v / 1e3);
    else               std::snprintf(b, sizeof(b), "$%.0f", v);
    return b;
}

double jnum(const json& v) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try { return std::stod(v.get<std::string>()); } catch (...) { return 0.0; }
    }
    return 0.0;
}

std::string unwrapTicker(std::string t) {
    if (t.rfind("NCSK", 0) == 0 && t.size() > 8) {
        std::string inner = t.substr(4);
        if (inner.size() > 4 && inner.compare(inner.size() - 4, 4, "2USD") == 0)
            inner = inner.substr(0, inner.size() - 4);
        if (!inner.empty() && inner.size() <= 12) return inner;
    }
    return t;
}

std::string baseSymbol(std::string sym) {
    const size_t dash = sym.find('-');
    if (dash != std::string::npos) sym = sym.substr(0, dash);
    return sym;
}
}

void refreshFundingCache() {
    if (!running.load(std::memory_order_relaxed)) return;
    if (g_fundLoading.exchange(true, std::memory_order_acq_rel)) return;
    struct Guard {
        ~Guard() { g_fundLoading.store(false, std::memory_order_release); }
    } guard;

    const std::string body = http(
        "https://open-api.bingx.com/openApi/swap/v2/quote/premiumIndex", "", 12);
    if (body.empty()) {
        std::cerr << "[FUNDING] пустой ответ по ставкам — кэш не тронут" << std::endl;
        return;
    }

    json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) {
        std::cerr << "[FUNDING] неразбираемый ответ по ставкам" << std::endl;
        return;
    }

    std::vector<FundingRow> fresh;
    for (const auto& it : j["data"]) {
        if (!it.is_object()) continue;
        if (!it.contains("symbol") || !it["symbol"].is_string()) continue;
        if (!it.contains("lastFundingRate")) continue;

        double rate = 0.0;
        if (it["lastFundingRate"].is_string()) {
            try { rate = std::stod(it["lastFundingRate"].get<std::string>()); } catch (...) { continue; }
        } else if (it["lastFundingRate"].is_number()) {
            rate = it["lastFundingRate"].get<double>();
        } else continue;

        if (!std::isfinite(rate) || std::fabs(rate) < FUNDING_MIN_ABS) continue;
        if (std::fabs(rate) > 1.0) continue;

        const std::string sym = it["symbol"].get<std::string>();
        if (sym.empty() || sym.size() > 32) continue;
        const std::string base = unwrapTicker(baseSymbol(sym));
        if (base.empty()) continue;

        FundingRow row;
        row.symbol = base;
        row.rawSymbol = sym;
        row.rate = rate;

        if (it.contains("nextFundingTime") && it.contains("lastFundingTime")) {
            const double nextMs = jnum(it["nextFundingTime"]);
            const double lastMs = jnum(it["lastFundingTime"]);
            const double stepH = (nextMs - lastMs) / 3600000.0;
            if (stepH >= 0.9 && stepH <= 24.5)
                row.payoutsPerDay = static_cast<int>(24.0 / stepH + 0.5);
        }
        fresh.push_back(std::move(row));
    }

    if (fresh.empty()) return;

    {
        const std::string tick = http(
            "https://open-api.bingx.com/openApi/swap/v2/quote/ticker", "", 12);
        std::map<std::string, std::pair<double,double>> extra;   // символ -> {интерес, оборот}
        if (!tick.empty()) {
            json tj = json::parse(tick, nullptr, false);
            if (tj.is_object() && tj.contains("data") && tj["data"].is_array()) {
                for (const auto& it : tj["data"]) {
                    if (!it.is_object() || !it.contains("symbol") || !it["symbol"].is_string()) continue;
                    const std::string sym = it["symbol"].get<std::string>();
                    if (sym.empty()) continue;

                    double last = it.contains("lastPrice") ? jnum(it["lastPrice"]) : 0.0;
                    double oi = 0.0;
                    if (it.contains("openInterest")) oi = jnum(it["openInterest"]);

                    double vol = 0.0;
                    if (it.contains("quoteVolume")) vol = jnum(it["quoteVolume"]);
                    else if (it.contains("volume")) {
                        vol = jnum(it["volume"]);
                        if (vol > 0.0 && last > 0.0) vol *= last;
                    }

                    if (!std::isfinite(oi) || oi < 0.0) oi = 0.0;
                    if (!std::isfinite(vol) || vol < 0.0) vol = 0.0;
                    if (oi > 1e12) oi = 0.0;
                    if (vol > 1e12) vol = 0.0;
                    extra[sym] = {oi, vol};
                }
            }
        }

        int withVol = 0;
        for (auto& r : fresh) {
            auto it = extra.find(r.rawSymbol);
            if (it == extra.end()) continue;
            r.oiUsd = it->second.first;
            r.volUsd = it->second.second;
            if (r.volUsd > 0.0) withVol++;
        }

        if (withVol > 0) {
            std::vector<FundingRow> kept;
            kept.reserve(fresh.size());
            for (const auto& r : fresh)
                if (r.volUsd >= FUNDING_MIN_VOL_USD) kept.push_back(r);
            fresh.swap(kept);
        } else {
            std::cerr << "[FUNDING] оборот не пришёл — отсечка не применена" << std::endl;
        }
    }

    std::sort(fresh.begin(), fresh.end(), [](const FundingRow& a, const FundingRow& b) {
        return std::fabs(a.rate) > std::fabs(b.rate);
    });
    if (fresh.size() > static_cast<size_t>(FUNDING_MAX_ROWS))
        fresh.resize(static_cast<size_t>(FUNDING_MAX_ROWS));

    const time_t oiDeadline = time(nullptr) + 45;
    for (auto& r : fresh) {
        if (!running.load(std::memory_order_relaxed)) break;
        if (time(nullptr) > oiDeadline) break;
        if (r.oiUsd > 0.0) continue;

        const std::string oiBody = http(
            "https://open-api.bingx.com/openApi/swap/v2/quote/openInterest?symbol="
            + r.rawSymbol, "", 4);
        if (oiBody.empty()) continue;

        json oj = json::parse(oiBody, nullptr, false);
        if (!oj.is_object() || !oj.contains("data")) continue;
        const json& d = oj["data"];
        if (!d.is_object() || !d.contains("openInterest")) continue;

        double oi = jnum(d["openInterest"]);
        if (!std::isfinite(oi) || oi <= 0.0) continue;

        if (oi > 1e12) continue;
        if (r.volUsd > 0.0 && oi > r.volUsd * 100.0) continue;
        r.oiUsd = oi;
    }

    std::lock_guard<std::mutex> l(g_fundMutex);
    g_fundCache.swap(fresh);
    g_fundAt = time(nullptr);
}

BigTradesMessage buildBigMenu(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    std::ostringstream t;
    t << "\U0001F4CA <b>" << tr(lang, "big_title") << "</b>\n\n"
      << tr(lang, "big_menu_hint");

    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "flow_btn")}, {"callback_data", "bg_open:flow:24h"}}
    }));
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
        kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "big_btn_liq")}, {"callback_data", "bg_open:liq:24h"}}
    }));
    {
        std::string fundBtn = tr(lang, "fund_btn");
        if (!isPremium(chatId)) fundBtn += " \U0001F512";
        kb["inline_keyboard"].push_back(json::array({
            {{"text", fundBtn}, {"callback_data", "bg_open:fund:24h"}}
        }));
    }
    kb["inline_keyboard"].push_back(backRow(lang, "menu:main"));
    return {t.str(), kb.dump()};
}

constexpr int FLOW_MAX_ROWS = 2000;
constexpr int FLOW_PER_PAGE = 10;

BigTradesMessage buildFlowList(const std::string& chatId, const std::string& window, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    const long long since = static_cast<long long>(time(nullptr)) - windowSeconds(window);

    std::ostringstream t;
    json kb;
    kb["inline_keyboard"] = json::array();

    std::vector<FlowRow> rows;
    {
        std::lock_guard<std::mutex> l(g_listCacheMutex);
        auto it = g_flowCache.byWindow.find(window);
        if (it != g_flowCache.byWindow.end() &&
            time(nullptr) - it->second.first < cacheTtlFor(window))
            rows = it->second.second;
    }
    if (rows.empty()) {
        rows = flowRows(since, FLOW_MAX_ROWS * 3);
        std::lock_guard<std::mutex> l(g_listCacheMutex);
        g_flowCache.byWindow[window] = {time(nullptr), rows};
    }
    std::vector<FlowRow> named;
    named.reserve(rows.size());
    for (auto& r : rows)
        if (hasKnownSymbol(r.token)) named.push_back(std::move(r));
    if (named.size() > static_cast<size_t>(FLOW_MAX_ROWS))
        named.resize(static_cast<size_t>(FLOW_MAX_ROWS));

    // Итог по всему окну, а не по странице: сколько денег вообще прошло
    // через отслеживаемые кошельки за период.
    long long totalBought = 0, totalSold = 0;
    for (const auto& r : named) { totalBought += r.boughtNanos; totalSold += r.soldNanos; }

    t << "\U0001F433 <b>" << tr(lang, "flow_title") << "</b> \u00B7 "
      << tr(lang, windowKey(window)) << "\n"
      << tr(lang, "flow_hint") << " "
      << compactUsd(static_cast<double>(totalBought) / 1e9) << " "
      << tr(lang, "flow_total_buys") << " \u00B7 "
      << compactUsd(static_cast<double>(totalSold) / 1e9) << " "
      << tr(lang, "flow_total_sells") << " \u00B7 "
      << named.size() << " " << tr(lang, "flow_coins") << "\n\n";

    const int total = static_cast<int>(named.size());
    const int pages = std::max(1, (total + FLOW_PER_PAGE - 1) / FLOW_PER_PAGE);
    if (page < 1) page = 1;
    if (page > pages) page = pages;
    const int from = (page - 1) * FLOW_PER_PAGE;
    const int to   = std::min(from + FLOW_PER_PAGE, total);

    if (named.empty()) {
        t << tr(lang, "flow_empty");
    } else {
        int n = from;
        for (int i = from; i < to; i++) {
            const FlowRow& r = named[static_cast<size_t>(i)];
            const long long net = r.boughtNanos - r.soldNanos;
            const bool inflow = net >= 0;
            t << "<b>" << (++n) << ".</b> " << (inflow ? "\U0001F7E2" : "\U0001F534")
              << " <b>" << spotAssetLabel(r.token) << "</b>  <code>"
              << (inflow ? "+" : "\u2212") << formatUsd(cpp_int(net < 0 ? -net : net))
              << "</code>\n"
              << "   " << tr(lang, "flow_bought") << " "
              << compactUsd(static_cast<double>(r.boughtNanos) / 1e9)
              << " \u00B7 " << tr(lang, "flow_sold") << " "
              << compactUsd(static_cast<double>(r.soldNanos) / 1e9) << "\n"
              << "   " << r.buys << " " << tr(lang, "flow_buys") << " / "
              << r.sells << " " << tr(lang, "flow_sells") << " \u00B7 "
              << r.wallets << " " << tr(lang, "flow_wallets") << "\n\n";
        }
    }

    if (pages > 1) {
        json nav = json::array();
        if (page > 1)
            nav.push_back({{"text", "\u2B05\uFE0F"},
                           {"callback_data", "bg_page:flow:" + window + ":" + std::to_string(page - 1)}});
        nav.push_back({{"text", std::to_string(page) + "/" + std::to_string(pages)},
                       {"callback_data", "bg_noop"}});
        if (page < pages)
            nav.push_back({{"text", "\u27A1\uFE0F"},
                           {"callback_data", "bg_page:flow:" + window + ":" + std::to_string(page + 1)}});
        kb["inline_keyboard"].push_back(nav);
    }

    json winRow = json::array();
    for (const char* w : {"1h", "24h", "7d", "30d"}) {
        const bool cur = (window == w);
        winRow.push_back({{"text", std::string(cur ? "\u2705 " : "") + tr(lang, windowKey(w))},
                          {"callback_data", std::string("bg_open:flow:") + w}});
    }
    kb["inline_keyboard"].push_back(winRow);

    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "flow_search_btn")}, {"callback_data", "bg_find:" + window}}
    }));
    kb["inline_keyboard"].push_back(backRow(lang, "menu:big"));
    return {t.str(), kb.dump()};
}

// Поиск по тикеру: список тот же, из кэша, отбор по подстроке.
BigTradesMessage buildFlowSearch(const std::string& chatId,
                                 const std::string& window,
                                 const std::string& query) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    const long long since = static_cast<long long>(time(nullptr)) - windowSeconds(window);

    std::vector<FlowRow> rows;
    {
        std::lock_guard<std::mutex> l(g_listCacheMutex);
        auto it = g_flowCache.byWindow.find(window);
        if (it != g_flowCache.byWindow.end() &&
            time(nullptr) - it->second.first < cacheTtlFor(window))
            rows = it->second.second;
    }
    if (rows.empty()) {
        rows = flowRows(since, FLOW_MAX_ROWS);
        std::lock_guard<std::mutex> l(g_listCacheMutex);
        g_flowCache.byWindow[window] = {time(nullptr), rows};
    }

    std::string needle;
    for (char c : query)
        needle += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;

    std::ostringstream t;
    json kb;
    kb["inline_keyboard"] = json::array();
    t << "\U0001F50D <b>" << safeString(query, 24) << "</b> \u00B7 "
      << tr(lang, windowKey(window)) << "\n\n";

    int shown = 0;
    for (const auto& r : rows) {
        if (!hasKnownSymbol(r.token)) continue;
        std::string sym = spotAssetLabel(r.token);
        std::string up;
        for (char c : sym) up += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
        if (up.find(needle) == std::string::npos) continue;

        const long long net = r.boughtNanos - r.soldNanos;
        const bool inflow = net >= 0;
        t << "<b>" << (++shown) << ".</b> " << (inflow ? "\U0001F7E2" : "\U0001F534")
          << " <b>" << sym << "</b>  <code>"
          << (inflow ? "+" : "\u2212") << formatUsd(cpp_int(net < 0 ? -net : net))
          << "</code>\n"
          << "   " << tr(lang, "flow_bought") << " "
          << compactUsd(static_cast<double>(r.boughtNanos) / 1e9)
          << " \u00B7 " << tr(lang, "flow_sold") << " "
          << compactUsd(static_cast<double>(r.soldNanos) / 1e9) << "\n"
          << "   " << r.buys << " " << tr(lang, "flow_buys") << " / "
          << r.sells << " " << tr(lang, "flow_sells") << " \u00B7 "
          << r.wallets << " " << tr(lang, "flow_wallets") << "\n\n";
        if (shown >= 10) break;
    }
    if (shown == 0) t << tr(lang, "flow_search_none");

    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "flow_search_btn")}, {"callback_data", "bg_find:" + window}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "bg_open:flow:" + window}}
    }));
    return {t.str(), kb.dump()};
}

BigTradesMessage buildFundingList(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    std::ostringstream t;
    json kb;
    kb["inline_keyboard"] = json::array();

    if (!isPremium(chatId)) {
        t << "\U0001F4A2 <b>" << tr(lang, "fund_title") << "</b>\n\n"
          << "\U0001F512 " << tr(lang, "hl_locked_body");
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
        }));
        kb["inline_keyboard"].push_back(backRow(lang, "menu:big"));
        return {t.str(), kb.dump()};
    }

    std::vector<FundingRow> rows;
    time_t at = 0;
    {
        std::lock_guard<std::mutex> l(g_fundMutex);
        rows = g_fundCache;
        at = g_fundAt;
    }

    const bool stale = rows.empty() ||
                       (time(nullptr) - at) > 2 * FUNDING_REFRESH_SEC;
    if (stale && running.load(std::memory_order_relaxed) &&
        !g_fundLoading.load(std::memory_order_acquire)) {
        std::thread(refreshFundingCache).detach();
    }

    t << "\U0001F4A2 <b>" << tr(lang, "fund_title") << "</b>\n"
      << "" << tr(lang, "fund_hint") << "\n\n";

    if (rows.empty()) {
        t << tr(lang, at > 0 ? "fund_empty" : "fund_loading");
    } else {
        int n = 0;
        for (const auto& r : rows) {
            const bool longsPay = r.rate > 0.0;
            const double pct = r.rate * 100.0;
            const double apr = pct * static_cast<double>(r.payoutsPerDay) * 365.0;

            char buf[96];
            std::snprintf(buf, sizeof(buf), "%+.3f%%", pct);
            char aprBuf[64];
            std::snprintf(aprBuf, sizeof(aprBuf), "%.0f%%", std::fabs(apr));

            t << "<b>" << (++n) << ".</b> " << (longsPay ? "\U0001F4C8" : "\U0001F4C9")
              << " <b>" << safeString(r.symbol, 16) << "</b>  <code>" << buf << "</code>"
              << " \u00B7 " << tr(lang, longsPay ? "fund_longs_pay" : "fund_shorts_pay") << "\n"
              << "   " << tr(lang, "fund_apr") << " " << aprBuf << "\n";

            if (r.oiUsd > 0.0 || r.volUsd > 0.0) {
                t << "   ";
                if (r.oiUsd > 0.0)
                    t << tr(lang, "fund_oi") << " " << compactUsd(r.oiUsd);
                if (r.oiUsd > 0.0 && r.volUsd > 0.0) t << " \u00B7 ";
                if (r.volUsd > 0.0)
                    t << tr(lang, "fund_vol") << " " << compactUsd(r.volUsd);
                t << "\n";
            }
            t << "\n";
        }
        if (at > 0) {
            const long long mins = (static_cast<long long>(time(nullptr)) - at) / 60;
            t << "" << tr(lang, "fund_updated") << " " << mins << " "
              << tr(lang, "fund_min_ago") << "";
        }
    }

    kb["inline_keyboard"].push_back(backRow(lang, "menu:big"));
    return {t.str(), kb.dump()};
}

BigTradesMessage buildBigList(const std::string& chatId, const std::string& venue,
                              const std::string& window, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    const bool liq  = venue == "liq";
    const bool perp = venue == "perp" || liq;
    const bool premium = isPremium(chatId);

    if (perp && !liq && !premium) {
        std::ostringstream t;
        t << (liq ? "\U0001F480 " : "\U0001F535 ")
          << "<b>" << tr(lang, liq ? "big_liq_title" : "big_perp_title") << "</b>\n\n"
          << "\U0001F512 " << tr(lang, "hl_locked_body");
        json kb;
        kb["inline_keyboard"] = json::array();
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
        }));
        kb["inline_keyboard"].push_back(backRow(lang, "menu:big"));
        return {t.str(), kb.dump()};
    }

    const int maxRows = MAX_ROWS;

    const long long since = static_cast<long long>(time(nullptr)) - windowSeconds(window);
    const int fetchRows = perp ? maxRows : maxRows * 3;
    const std::string cacheKey = venue + ":" + window;
    std::vector<BigRow> rows;
    {
        std::lock_guard<std::mutex> l(g_listCacheMutex);
        auto it = g_spotCache.byWindow.find(cacheKey);
        if (it != g_spotCache.byWindow.end() &&
            time(nullptr) - it->second.first < cacheTtlFor(window))
            rows = it->second.second;
    }
    if (rows.empty()) {
        rows = liq  ? liqRows(since, fetchRows)
             : perp ? perpRows(since, fetchRows)
                    : spotRows(since, fetchRows);
        std::lock_guard<std::mutex> l(g_listCacheMutex);
        g_spotCache.byWindow[cacheKey] = {time(nullptr), rows};
    }

    if (!perp) {
        std::vector<BigRow> named;
        named.reserve(rows.size());
        for (auto& r : rows)
            if (hasKnownSymbol(r.asset)) named.push_back(std::move(r));
        rows.swap(named);
    }
    if (static_cast<int>(rows.size()) > maxRows)
        rows.resize(static_cast<size_t>(maxRows));

    std::ostringstream t;
    t << (liq ? "\U0001F480 " : perp ? "\U0001F535 " : "\U0001F7E1 ")
      << "<b>" << tr(lang, liq ? "big_liq_title" : perp ? "big_perp_title" : "big_spot_title") << "</b>\n"
      << "" << tr(lang, windowKey(window)) << "\n\n";

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
                const char* dk = hlDirKey(r.dirCode);
                const bool up = hlSideUp(r.dirCode);
                const std::string coin = safeString(r.asset.empty() ? "?" : r.asset, 16);
                const char* mark = liq ? "\U0001F480"
                                       : (up ? "\U0001F7E2" : "\U0001F534");

                t << "<b>" << (i + 1) << ".</b> "
                  << mark << " <b>"
                  << tr(lang, dk) << " \u00B7 " << coin << "</b>\n";

                if (liq) {
                    if (r.closedPnlNanos != 0)
                        t << "\U0001F4B8 <b>" << tr(lang, "big_liq_loss") << ":</b> "
                          << formatUsdNanosSigned(r.closedPnlNanos, true) << "\n";
                    t << "\U0001F4CA " << tr(lang, "big_liq_position") << ": <b>"
                      << formatUsd(cpp_int(r.usdNanos)) << "</b>";
                    if (r.leverage > 0)
                        t << " \u00B7 " << tr(lang, "hl_leverage") << " "
                          << r.leverage << "\u00D7";
                    t << "\n";
                    if (!r.pxStr.empty())
                        t << "\u2620\uFE0F " << tr(lang, "big_liq_closed_at") << ": <b>"
                          << safeString(r.pxStr, 24) << "</b>\n";
                    if (!r.amountStr.empty())
                        t << "\U0001F4E6 " << tr(lang, "hl_qty") << ": <b>"
                          << safeString(r.amountStr, 24) << " " << coin << "</b>\n";
                    if (r.accountValueNanos > 0)
                        t << "\U0001F3E6 " << tr(lang, "big_liq_account_was") << ": <b>"
                          << formatUsd(cpp_int(r.accountValueNanos)) << "</b>\n";
                } else {
                    t << "\U0001F4B0 " << tr(lang, "hl_trade_size") << ": <b>"
                      << formatUsd(cpp_int(r.usdNanos)) << "</b>\n";
                    if (r.leverage > 0)
                        t << "\u2699\uFE0F " << tr(lang, "hl_leverage") << ": <b>"
                          << r.leverage << "\u00D7</b>\n";
                    if (r.marginNanos > 0) {
                        t << "\U0001F4B5 " << tr(lang, "hl_collateral") << ": <b>"
                          << formatUsd(cpp_int(r.marginNanos)) << "</b>";
                        if (r.accountValueNanos > 0) {
                            const double share = 100.0 * static_cast<double>(r.marginNanos)
                                                       / static_cast<double>(r.accountValueNanos);
                            t << " (" << formatPercent(share, false) << " "
                              << tr(lang, "hl_of_account") << ")";
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
                }
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
    for (const char* w : {"1h", "24h", "7d", "30d"}) {
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
        if (param.substr(0, sep) == "fund") { out = buildFundingList(chatId); return true; }
        if (param.substr(0, sep) == "flow") {
            out = buildFlowList(chatId, param.substr(sep + 1), 1);
            return true;
        }
        out = buildBigList(chatId, param.substr(0, sep), param.substr(sep + 1), 1);
        return true;
    }
    if (action == "bg_find") {
        const Lang lang = langFromCode(getUserLanguage(chatId));
        g_sessionManager.setState(chatId, UserState::AWAITING_FLOW_SEARCH, param);
        out.text = tr(lang, "flow_search_prompt");
        json kb;
        kb["inline_keyboard"] = json::array();
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "back_button")}, {"callback_data", "bg_open:flow:" + param}}
        }));
        out.keyboard = kb.dump();
        return true;
    }
    if (action == "bg_page") {
        const size_t a = param.find(':');
        if (a == std::string::npos) return false;
        const size_t b = param.find(':', a + 1);
        if (b == std::string::npos) return false;
        int page = 1;
        try { page = std::stoi(param.substr(b + 1)); } catch (...) {}
        const std::string venue = param.substr(0, a);
        const std::string window = param.substr(a + 1, b - a - 1);
        if (venue == "flow") out = buildFlowList(chatId, window, page);
        else                 out = buildBigList(chatId, venue, window, page);
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
