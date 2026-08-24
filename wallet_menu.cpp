#include "wallet_menu.h"
#include "hyperliquid.h"

#include <sstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <mutex>
#include <ctime>
#include <sqlite3.h>

#include "json.hpp"
#include "utils.h"
#include "premium.h"
#include "ranking.h"
#include "alert_settings.h"

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;

namespace {
std::mutex g_walletPageMutex;
std::map<std::string, int> g_lastWalletPage;
}

void rememberWalletPage(const std::string& chatId, int page) {
    std::lock_guard<std::mutex> l(g_walletPageMutex);
    g_lastWalletPage[chatId] = page < 1 ? 1 : page;
}

int lastWalletPage(const std::string& chatId) {
    std::lock_guard<std::mutex> l(g_walletPageMutex);
    auto it = g_lastWalletPage.find(chatId);
    return it != g_lastWalletPage.end() ? it->second : 1;
}

std::string backToWalletsData(const std::string& chatId) {
    return "mw_page:" + std::to_string(lastWalletPage(chatId));
}

std::string errorBackKeyboard(const std::string& chatId, Lang lang) {
    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", backToWalletsData(chatId)}}
    }));
    return kb.dump();
}

std::string shortAddress(const std::string& a) {
    if (a.length() <= 12) return a;
    return a.substr(0, 6) + "..." + a.substr(a.length() - 4);
}
bool isTrackingWallet(const std::string& chatId, const std::string& address) {
    const std::string addr = toLower(address);
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT 1 FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id WHERE uw.user_id=? AND wa.address=?")) return false;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,addr.c_str(),-1,SQLITE_TRANSIENT);
    bool e=sqlite3_step(s)==SQLITE_ROW; sqlite3_finalize(s); return e;
}

size_t countUserWhales(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT COUNT(*) FROM user_whales WHERE user_id=?")) return 0;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    size_t n=0; if (sqlite3_step(s)==SQLITE_ROW) n=sqlite3_column_int64(s,0); sqlite3_finalize(s); return n;
}

namespace {
void reassignPrimaryLocked(const std::string& chatId) {
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "UPDATE user_whales SET is_primary=1 WHERE user_id=? AND whale_id=("
        "SELECT whale_id FROM user_whales WHERE user_id=? ORDER BY created_at ASC, rowid ASC LIMIT 1) "
        "AND NOT EXISTS (SELECT 1 FROM user_whales WHERE user_id=? AND is_primary=1)")) return;
    for (int i = 1; i <= 3; i++) sqlite3_bind_text(s, i, chatId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);
}
}

void untrackWalletFromService(const std::string& wallet) {
    const std::string addr = toLower(wallet);
    std::vector<std::pair<std::string, std::string>> recipients; // chatId, label
    int removed = 0;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s,
                "SELECT uw.user_id, uw.label FROM user_whales uw "
                "JOIN whale_addresses wa ON wa.id = uw.whale_id "
                "WHERE wa.address = ?")) {
            sqlite3_bind_text(s, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(s) == SQLITE_ROW) {
                std::string cid = safeColumnText(s, 0);
                std::string lab = safeColumnText(s, 1);
                if (!cid.empty()) recipients.emplace_back(std::move(cid), std::move(lab));
            }
            sqlite3_finalize(s);
        }
        if (!prepareOrLog(db, &s,
            "DELETE FROM user_whales WHERE whale_id=("
            "SELECT id FROM whale_addresses WHERE address=?)")) return;
        sqlite3_bind_text(s, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_DONE) removed = sqlite3_changes(db);
        else std::cerr << "[WATCHERS] bot untrack failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s);

        if (removed > 0)
            for (const auto& r : recipients) reassignPrimaryLocked(r.first);
    }
    if (removed <= 0) return;

    std::cout << "[WATCHERS] Bot wallet untracked from " << removed
              << " watchlist(s): " << addr << std::endl;
    refreshWatchers();

    for (const auto& [cid, label] : recipients) {
        if (cid == SERVICE_CHAT_ID) continue;
        Lang lang = langFromCode(getUserLanguage(cid));
        std::string shown = label.empty() || toLower(label) == addr
            ? shortAddress(addr)
            : safeString(label, 32);
        std::string msg = tr(lang, "wallet_bot_removed");
        msg += "\n\n💼 <b>" + shown + "</b>\n<code>" + safeString(addr, 42) + "</code>";
        sendMsg(cid, msg);
    }
}

AddWhaleResult addUserWhale(const std::string& chatId, const std::string& addressArg, const std::string& label) {
    const std::string address = toLower(addressArg);
    if (!isValidAddress(address)) return AddWhaleResult::BAD_ADDRESS;
    ensureUser(chatId);

    if (isPermanentlyBanned(address)) {
        return AddWhaleResult::PERMANENTLY_BANNED;
    }

    if (chatId != SERVICE_CHAT_ID &&
        countUserWhales(chatId) >= premiumMaxWallets(chatId))
    {
        return AddWhaleResult::LIMIT_REACHED;
    }

    std::lock_guard<std::mutex> l(dbMutex);
    if (sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] addUserWhale BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return AddWhaleResult::ERROR;
    }
    sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR IGNORE INTO whale_addresses(address) VALUES(?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,address.c_str(),-1,SQLITE_TRANSIENT);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] whale_addresses INSERT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR;
    }
    sqlite3_finalize(s);
    long long whaleId=-1;
    if (!prepareOrLog(db,&s,"SELECT id FROM whale_addresses WHERE address=?")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,address.c_str(),-1,SQLITE_TRANSIENT);
    if (sqlite3_step(s)==SQLITE_ROW) whaleId=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);
    if (whaleId<0) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }

    if (!prepareOrLog(db,&s,"SELECT 1 FROM user_whales WHERE user_id=? AND whale_id=?")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,whaleId);
    bool exists = sqlite3_step(s)==SQLITE_ROW; sqlite3_finalize(s);
    if (exists) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ALREADY_EXISTS; }

    bool firstWallet = false;
    {
        sqlite3_stmt* c;
        if (prepareOrLog(db,&c,"SELECT 1 FROM user_whales WHERE user_id=? LIMIT 1")) {
            sqlite3_bind_text(c,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
            firstWallet = sqlite3_step(c) != SQLITE_ROW;
            sqlite3_finalize(c);
        }
    }
    if (!prepareOrLog(db,&s,"INSERT INTO user_whales(user_id,whale_id,label,created_at,is_primary) VALUES(?,?,?,?,?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,whaleId);
    sqlite3_bind_text(s,3,label.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,4,time(nullptr));
    sqlite3_bind_int(s,5,firstWallet ? 1 : 0);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] user_whales INSERT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR;
    }
    sqlite3_finalize(s);
    if (sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] addUserWhale COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
        return AddWhaleResult::ERROR;
    }
    return AddWhaleResult::OK;
}

bool removeUserWhale(const std::string& chatId, const std::string& addressArg) {
    const std::string address = toLower(addressArg);
    std::lock_guard<std::mutex> l(dbMutex);
    if (sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] removeUserWhale BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_stmt* s;
    long long whaleId=-1;
    if (prepareOrLog(db,&s,"SELECT id FROM whale_addresses WHERE address=?")) {
        sqlite3_bind_text(s,1,address.c_str(),-1,SQLITE_TRANSIENT);
        if (sqlite3_step(s)==SQLITE_ROW) whaleId=sqlite3_column_int64(s,0);
        sqlite3_finalize(s);
    }
    if (whaleId<0) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }

    if (!prepareOrLog(db,&s,"DELETE FROM user_whales WHERE user_id=? AND whale_id=?")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,whaleId);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] user_whales DELETE failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false;
    }
    bool removed = sqlite3_changes(db)>0; sqlite3_finalize(s);

    if (!prepareOrLog(db,&s,"DELETE FROM whale_addresses WHERE id=? AND NOT EXISTS (SELECT 1 FROM user_whales WHERE whale_id=?)")) {
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false;
    }
    sqlite3_bind_int64(s,1,whaleId); sqlite3_bind_int64(s,2,whaleId);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] whale_addresses cleanup failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false;
    }
    sqlite3_finalize(s);

    if (removed) reassignPrimaryLocked(chatId);

    if (sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] removeUserWhale COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
        return false;
    }
    return removed;
}

namespace TelegramUI {

UIMessage buildAccountMenu(const std::string& chatId) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    size_t count = countUserWhales(chatId);

    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_my_wallets") + " (" + std::to_string(count) + ")"},
         {"callback_data", "menu:my_wallets"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_positions")}, {"callback_data", "hl_positions"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    std::stringstream text;
    text << tr(lang, "account_title") << "\n\n" << tr(lang, "account_desc");
    return {text.str(), kb.dump()};
}

UIMessage buildWalletsList(const std::string& chatId, int page) {

    bool premium = isPremium(chatId);
    Lang lang = langFromCode(getUserLanguage(chatId));

    struct WalletRow { std::string address, label; bool primary; };
    std::vector<WalletRow> walletRows;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (!prepareOrLog(db, &s,
            "SELECT wa.address, uw.label, uw.is_primary FROM user_whales uw "
            "JOIN whale_addresses wa ON wa.id = uw.whale_id "
            "WHERE uw.user_id = ? "
            "ORDER BY uw.is_primary DESC, uw.created_at")) {
            return {tr(lang, "err_loading_wallets"), ""};
        }
        sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(s) == SQLITE_ROW)
            walletRows.push_back({safeColumnText(s, 0), safeColumnText(s, 1),
                                  sqlite3_column_int(s, 2) != 0});
        sqlite3_finalize(s);
    }

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    std::stringstream text;
    text << tr(lang, "menu_my_wallets");

    constexpr int PER_PAGE = 5;
    const int total = static_cast<int>(walletRows.size());
    const int totalPages = total > 0 ? (total + PER_PAGE - 1) / PER_PAGE : 1;
    if (page < 1) page = 1;
    if (page > totalPages) page = totalPages;
    if (totalPages > 1) text << " (" << page << "/" << totalPages << ")";
    rememberWalletPage(chatId, page);
    text << "\n\n";
    const int startIdx = (page - 1) * PER_PAGE;
    const int endIdx = std::min(total, startIdx + PER_PAGE);

    bool any = total > 0;
    for (int i = startIdx; i < endIdx; i++) {
        const std::string& address = walletRows[i].address;
        const std::string& label = walletRows[i].label;

        if (i > startIdx) text << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        const bool isPrimary = walletRows[i].primary;
        std::string status;
        if (isPrimary) status = " 🔔 " + tr(lang, "wl_main_wallet");
        else if (!premium) status = " ⏸ " + tr(lang, "wl_paused");
        std::string shownLabel = (toLower(label) == address) ? tr(lang, "alert_wallet") : safeString(label, 32);
        text << "👤 <b>" << shownLabel << "</b>" << status << "\n";
        text << "<code>" << safeString(address, 42) << "</code>\n";

        SpotRankInfo sr;
        text << "\n🟡 <b>BSC " << tr(lang, "wl_spot_rank") << "</b>";
        if (spotRankOf(address, sr) && sr.rank <= 100) {
            text << " — #" << sr.rank << "\n"
                 << "💵 PnL: " << formatUsdNanosSigned(sr.pnlNanos, true) << "\n"
                 << "📈 " << tr(lang, "rk_roi_per_trade") << ": "
                 << formatPercent(sr.roiPercent, true) << "\n"
                 << "🎯 " << tr(lang, "ws_winrate") << ": " << sr.winRatePercent << "%\n"
                 << "🔄 " << tr(lang, "rk_trades") << ": " << sr.completedTrades << "\n";
        } else {
            text << ": " << tr(lang, "wl_not_ranked") << "\n";
        }

        PerpRankInfo pr;
        text << "\n🔵 <b>Hyperliquid " << tr(lang, "wl_perp_rank") << "</b>";
        if (perpRankOf(address, pr) && pr.rank <= 100) {
            text << " — #" << pr.rank << "\n"
                 << "💵 PnL: " << formatUsdNanosSigned(pr.pnlNanos, true) << "\n";
            if (pr.roiKnown)
                text << "📈 " << tr(lang, "hl_rk_roi_account") << ": "
                     << formatPercent(pr.roiPercent, true) << "\n";
            text << "🎯 " << tr(lang, "ws_winrate") << ": " << pr.winRatePercent << "%\n"
                 << "🔄 " << tr(lang, "rk_trades") << ": " << pr.closedTrades << "\n";
        } else {
            text << ": " << tr(lang, "wl_not_ranked") << "\n";
        }
        text << "\n";

        json row;
        std::string btnLabel = (toLower(label) == address)
                             ? tr(lang, "alert_wallet")
                             : truncateUtf8(label, 32);
        row.push_back({{"text", "✏️ " + btnLabel}, {"callback_data", "rename:" + address}});
        if (walletRows.size() > 1 && !isPrimary)
            row.push_back({{"text", "🔔"}, {"callback_data", "setmain:" + address}});
        row.push_back({{"text", "🗑️"}, {"callback_data", "askremove:" + address}});
        keyboard["inline_keyboard"].push_back(row);
    }

    if (totalPages > 1) {
        json navRow = json::array();
        if (page > 1) navRow.push_back({{"text", "⬅️"}, {"callback_data", "mw_page:" + std::to_string(page - 1)}});
        navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "mw_noop"}});
        if (page < totalPages) navRow.push_back({{"text", "➡️"}, {"callback_data", "mw_page:" + std::to_string(page + 1)}});
        keyboard["inline_keyboard"].push_back(navRow);
    }

    if (!any) {
        text << tr(lang, "mw_no_wallets") << "\n\n";
        text << tr(lang, "mw_tap_add");
    } else if (!premium && total > 1) {
        text << tr(lang, "mw_free_notice1") << "\n";
        text << tr(lang, "mw_free_notice2");
        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
        }));
    }

    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "menu_add_wallet")}, {"callback_data", "menu:add_wallet"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "back"}}
    }));

    return {text.str(), keyboard.dump()};
}

UIMessage buildRemoveConfirm(const std::string& chatId, const std::string& address, const std::string& label, Lang lang) {
    std::stringstream text;
    text << tr(lang, "remove_confirm_title") << "\n\n";
    if (toLower(label) == toLower(address)) {
        text << "💼 <b>" << tr(lang, "alert_wallet") << "</b>\n";
    } else {
        text << "💼 <b>" << safeString(label, 32) << "</b>\n";
    }
    text << "<code>" << safeString(address, 42) << "</code>\n\n";
    text << tr(lang, "remove_confirm_notice");

    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "remove_yes")}, {"callback_data", "remove:" + address}},
        {{"text", tr(lang, "cancel_button")}, {"callback_data", backToWalletsData(chatId)}}
    }));
    return {text.str(), keyboard.dump()};
}

}

void startAddWalletFlow(const std::string& chatId, long long messageId) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    g_sessionManager.setState(chatId, UserState::AWAITING_WALLET_ADDRESS, "", messageId);
    replyInPlace(chatId, messageId, tr(lang, "add_wallet_title"),
            TelegramUI::buildCancelWithTopTraders(lang));
}

bool handleWalletCallback(const std::string& chatId, const std::string& action, const std::string& param,
                          const std::string& data, long long messageId, const std::string& callbackQueryId) {
    if (action == "mw_page") {
        g_sessionManager.clearSession(chatId);
        rememberView(chatId, data);
        int page = 1;
        try { page = std::stoi(param); } catch (...) {}
        auto msg = TelegramUI::buildWalletsList(chatId, page);
        replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    }
    else if (action == "rename") {
        std::string address = toLower(param);
        Lang lang = langFromCode(getUserLanguage(chatId));
        if (!isValidAddress(address)) {
            replyInPlace(chatId, messageId, tr(lang, "err_invalid_address"), errorBackKeyboard(chatId, lang));
            return true;
        }

        bool prepFailed = false, found = false;
        std::string currentLabel;
        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (!prepareOrLog(db, &s,
                "SELECT uw.label FROM user_whales uw "
                "JOIN whale_addresses wa ON wa.id = uw.whale_id "
                "WHERE uw.user_id = ? AND wa.address = ?")) {
                prepFailed = true;
            } else {
                sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, address.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(s) == SQLITE_ROW) {
                    found = true;
                    currentLabel = safeColumnText(s, 0);
                }
                sqlite3_finalize(s);
            }
        }

        if (prepFailed) {
            replyInPlace(chatId, messageId, tr(lang, "err_loading_wallet"), errorBackKeyboard(chatId, lang));
            return true;
        }
        if (!found) {
            replyInPlace(chatId, messageId, tr(lang, "err_wallet_not_found"), errorBackKeyboard(chatId, lang));
            return true;
        }

        g_sessionManager.setState(chatId, UserState::AWAITING_RENAME, address, messageId);

        json cancelKb;
        cancelKb["inline_keyboard"] = json::array({
            json::array({
                {{"text", tr(lang, "cancel_button")}, {"callback_data", backToWalletsData(chatId)}}
            })
        });

        replyInPlace(chatId, messageId,
            tr(lang, "rename_title") + "\n\n" +
            tr(lang, "rename_current_name") + " <b>" + safeString(currentLabel, 32) + "</b>\n\n" +
            tr(lang, "rename_enter_new"),
            cancelKb.dump());
    }
    else if (action == "setmain") {
        const Lang lang = langFromCode(getUserLanguage(chatId));
        const std::string address = toLower(param);
        if (!isValidAddress(address)) {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, tr(lang, "err_invalid_address"), true);
            return true;
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
                std::cerr << "[WALLET] setmain: BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
            } else {
                bool ok = false;
                if (prepareOrLog(db, &s, "UPDATE user_whales SET is_primary=0 WHERE user_id=?")) {
                    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
                    ok = sqlite3_step(s) == SQLITE_DONE;
                    sqlite3_finalize(s);
                }
                if (ok && prepareOrLog(db, &s,
                    "UPDATE user_whales SET is_primary=1 WHERE user_id=? AND whale_id=("
                    "SELECT id FROM whale_addresses WHERE address=?)")) {
                    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(s, 2, address.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(s) == SQLITE_DONE) changed = sqlite3_changes(db) > 0;
                    else ok = false;
                    sqlite3_finalize(s);
                } else ok = false;
                if (ok && changed) sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
                else {
                    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
                    changed = false;
                }
            }
        }
        if (!changed) {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, tr(lang, "err_wallet_not_found"), true);
            return true;
        }
        refreshWatchers();
        if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, tr(lang, "toast_main_wallet_set"), false);
        auto msg = TelegramUI::buildWalletsList(chatId, lastWalletPage(chatId));
        replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    }
    else if (action == "askremove") {
        std::string address = toLower(param);
        if (!isValidAddress(address)) {
            replyInPlace(chatId, messageId, tr(langFromCode(getUserLanguage(chatId)), "err_invalid_address"), errorBackKeyboard(chatId, langFromCode(getUserLanguage(chatId))));
            return true;
        }
        std::string label = address;
        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (prepareOrLog(db, &s,
                "SELECT uw.label FROM user_whales uw "
                "JOIN whale_addresses wa ON wa.id = uw.whale_id "
                "WHERE uw.user_id = ? AND wa.address = ?")) {
                sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, address.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(s) == SQLITE_ROW) label = safeColumnText(s, 0);
                sqlite3_finalize(s);
            }
        }
        auto msg = TelegramUI::buildRemoveConfirm(chatId, address, label, langFromCode(getUserLanguage(chatId)));
        replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    }
    else if (action == "remove") {
        std::string address = toLower(param);
        Lang lang = langFromCode(getUserLanguage(chatId));
        if (!isValidAddress(address)) {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, tr(lang, "err_invalid_address"), true);
            replyInPlace(chatId, messageId, tr(lang, "err_invalid_address"), errorBackKeyboard(chatId, lang));
            return true;
        }

        bool removed = removeUserWhale(chatId, address);
        if (removed) {
            refreshWatchers();
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, tr(lang, "toast_wallet_removed"), false);
            int page = lastWalletPage(chatId);
            auto msg = TelegramUI::buildWalletsList(chatId, page);
            rememberView(chatId, "mw_page:" + std::to_string(page));
            replyInPlace(chatId, messageId, msg.text, msg.keyboard);
        } else {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, tr(lang, "err_wallet_not_found"), true);
            replyInPlace(chatId, messageId, tr(lang, "err_wallet_not_found"), errorBackKeyboard(chatId, lang));
        }
    }
    else return false;
    return true;
}

bool handleWalletText(const std::string& chatId, const std::string& text, const UserSession& session) {
    if (session.state == UserState::AWAITING_WALLET_ADDRESS) {
        std::string address = toLower(trim(text));
        Lang lang = langFromCode(getUserLanguage(chatId));

        if (!isValidAddress(address)) {
            replyInPlace(chatId, session.promptMessageId, tr(lang, "add_wallet_invalid"),
                    TelegramUI::buildCancelButton(lang));
            return true;
        }

        g_sessionManager.setState(chatId, UserState::AWAITING_WALLET_NAME, address, session.promptMessageId);
        replyInPlace(chatId, session.promptMessageId, tr(lang, "add_wallet_addr_ok"),
                TelegramUI::buildCancelButton(lang));
        return true;
    }

    if (session.state == UserState::AWAITING_WALLET_NAME) {
        std::string label = trim(text);
        Lang lang = langFromCode(getUserLanguage(chatId));

        if (label.empty()) {
            replyInPlace(chatId, session.promptMessageId, tr(lang, "err_name_empty"),
                    TelegramUI::buildCancelButton(lang));
            return true;
        }

        if (label.length() > 32) {
            replyInPlace(chatId, session.promptMessageId, tr(lang, "err_name_too_long"),
                    TelegramUI::buildCancelButton(lang));
            return true;
        }

        auto result = addUserWhale(chatId, session.pendingAddress, label);

        if (result == AddWhaleResult::OK) {
            refreshWatchers();
            g_sessionManager.clearSession(chatId);
            auto msg = TelegramUI::buildWalletsList(chatId, lastWalletPage(chatId));
            replyInPlace(chatId, session.promptMessageId, tr(lang, "add_wallet_success") + "\n\n" + tr(lang, "add_wallet_name_label") + " <b>" + safeString(label, 32) +
                    "</b>\n" + tr(lang, "add_wallet_address_label") + " <code>" + session.pendingAddress + "</code>\n\n" + tr(lang, "add_wallet_tracking_enabled") + "\n\n" + msg.text,
                    msg.keyboard);
        }
        else if (result == AddWhaleResult::ALREADY_EXISTS) {
            g_sessionManager.setState(chatId, UserState::AWAITING_WALLET_ADDRESS, "", session.promptMessageId);
            replyInPlace(chatId, session.promptMessageId, tr(lang, "already_tracking_retry"),
                    TelegramUI::buildCancelButton(lang));
        }
        else if (result == AddWhaleResult::PERMANENTLY_BANNED) {
            g_sessionManager.setState(chatId, UserState::AWAITING_WALLET_ADDRESS, "", session.promptMessageId);
            replyInPlace(chatId, session.promptMessageId, tr(lang, "wallet_bot_banned"),
                    TelegramUI::buildCancelButton(lang));
        }
        else if (result == AddWhaleResult::LIMIT_REACHED) {
            g_sessionManager.clearSession(chatId);
            if (isPremium(chatId)) {
                auto msg = TelegramUI::buildMainMenu(chatId);
                replyInPlace(chatId, session.promptMessageId, tr(lang, "limit_50_reached") + "\n\n" + msg.text, msg.keyboard);
            } else {
                auto lim = buildWalletLimitMessage(lang);
                replyInPlace(chatId, session.promptMessageId, lim.text, lim.keyboard);
            }
        }
        else {
            replyInPlace(chatId, session.promptMessageId, tr(lang, "generic_error_retry"), TelegramUI::buildCancelButton(lang));
        }

        return true;
    }

    if (session.state == UserState::AWAITING_TRACK_NAME) {
        std::string address = session.pendingAddress;
        std::string label = trim(text);

        if (label.empty() || label == "-" || label == "." || toLower(label) == address) {
            label = shortAddress(address);
        }

        if (label.length() > 32) {
            Lang lang = langFromCode(getUserLanguage(chatId));
            replyInPlace(chatId, session.promptMessageId, tr(lang, "err_name_too_long"),
                    TelegramUI::buildCancelButton(lang));
            return true;
        }

        auto result = addUserWhale(chatId, address, label);

        if (result == AddWhaleResult::OK) {
            refreshWatchers();
            Lang lang = langFromCode(getUserLanguage(chatId));
            std::string back = getLastView(chatId);
            g_sessionManager.clearSession(chatId);
            auto msg = back.empty() ? TelegramUI::buildMainMenu(chatId) : renderViewByData(chatId, back);
            replyInPlace(chatId, session.promptMessageId, tr(lang, "track_now_tracked") + " \"" + safeString(label, 32) + "\" " + tr(lang, "track_now_tracked_suffix") + "\n\n" + msg.text, msg.keyboard);
        }
        else if (result == AddWhaleResult::ALREADY_EXISTS) {
            Lang lang = langFromCode(getUserLanguage(chatId));
            std::string back = getLastView(chatId);
            g_sessionManager.clearSession(chatId);
            auto msg = back.empty() ? TelegramUI::buildMainMenu(chatId) : renderViewByData(chatId, back);
            replyInPlace(chatId, session.promptMessageId, tr(lang, "already_tracking") + "\n\n" + msg.text, msg.keyboard);
        }
        else if (result == AddWhaleResult::PERMANENTLY_BANNED) {
            Lang lang = langFromCode(getUserLanguage(chatId));
            std::string back = getLastView(chatId);
            g_sessionManager.clearSession(chatId);
            auto msg = back.empty() ? TelegramUI::buildMainMenu(chatId) : renderViewByData(chatId, back);
            replyInPlace(chatId, session.promptMessageId, tr(lang, "wallet_bot_banned") + "\n\n" + msg.text, msg.keyboard);
        }
        else if (result == AddWhaleResult::LIMIT_REACHED) {
            g_sessionManager.clearSession(chatId);
            Lang lang = langFromCode(getUserLanguage(chatId));
            if (isPremium(chatId)) {
                auto msg = TelegramUI::buildMainMenu(chatId);
                replyInPlace(chatId, session.promptMessageId, tr(lang, "limit_50_reached") + "\n\n" + msg.text, msg.keyboard);
            } else {
                auto lim = buildWalletLimitMessage(lang);
                replyInPlace(chatId, session.promptMessageId, lim.text, lim.keyboard);
            }
        }
        else {
            replyInPlace(chatId, session.promptMessageId, tr(langFromCode(getUserLanguage(chatId)), "generic_error_retry"), TelegramUI::buildCancelButton(langFromCode(getUserLanguage(chatId))));
        }
        return true;
    }

    if (session.state == UserState::AWAITING_RENAME) {
        std::string newLabel = trim(text);
        Lang lang = langFromCode(getUserLanguage(chatId));

        if (newLabel.empty()) {
            replyInPlace(chatId, session.promptMessageId, tr(lang, "err_name_empty"),
                    TelegramUI::buildCancelButton(lang));
            return true;
        }

        if (newLabel.length() > 32) {
            replyInPlace(chatId, session.promptMessageId, tr(lang, "err_name_too_long"),
                    TelegramUI::buildCancelButton(lang));
            return true;
        }

        bool renamed = false;
        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (prepareOrLog(db, &s,
                "UPDATE user_whales SET label = ? "
                "WHERE user_id = ? AND whale_id = (SELECT id FROM whale_addresses WHERE address = ?)")) {
                sqlite3_bind_text(s, 1, newLabel.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, chatId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, session.pendingAddress.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(s) == SQLITE_DONE) renamed = sqlite3_changes(db) > 0;
                sqlite3_finalize(s);
            }
        }

        if (!renamed) {
            g_sessionManager.clearSession(chatId);
            replyInPlace(chatId, session.promptMessageId, tr(lang, "err_wallet_not_found"),
                    errorBackKeyboard(chatId, lang));
            return true;
        }

        refreshWatchers();
        g_sessionManager.clearSession(chatId);
        auto msg = TelegramUI::buildWalletsList(chatId, lastWalletPage(chatId));
        replyInPlace(chatId, session.promptMessageId,
                tr(lang, "rename_success") + "\n\n" +
                tr(lang, "rename_new_name") + " <b>" + safeString(newLabel, 32) + "</b>.\n\n" +
                msg.text,
                msg.keyboard);
        return true;
    }

    return false;
}
