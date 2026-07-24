#include "wallet_menu.h"

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
#include "tx_analyzer.h"

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;

// ==================== Память навигации по списку =========================
// Кнопка "Назад" с карточки кошелька или из подтверждения удаления должна
// возвращать на ТУ ЖЕ страницу списка, где был пользователь, а не на первую.
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

// Клавиатура для экранов ошибок: без неё пользователь оставался в тупике -
// сообщение об ошибке вообще без кнопок, выйти можно было только через /start.
std::string errorBackKeyboard(const std::string& chatId, Lang lang) {
    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", backToWalletsData(chatId)}}
    }));
    return kb.dump();
}

// ============================ Форматирование ============================

std::string shortAddress(const std::string& a) {
    if (a.length() <= 12) return a;
    return a.substr(0, 6) + "..." + a.substr(a.length() - 4);
}
std::string fmtPnlSigned(long long pnlNanos) {
    cpp_int a = pnlNanos < 0 ? cpp_int(-pnlNanos) : cpp_int(pnlNanos);
    return (pnlNanos < 0 ? "-" : "+") + formatUsd(a);
}
std::string fmtPctSigned(double p) {
    long long r = static_cast<long long>(p >= 0 ? p + 0.5 : p - 0.5);
    return (r < 0 ? "-" : "+") + std::to_string(r < 0 ? -r : r) + "%";
}


// ========================= Операции хранилища ==========================

bool isTrackingWallet(const std::string& chatId, const std::string& address) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT 1 FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id WHERE uw.user_id=? AND wa.address=?")) return false;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,address.c_str(),-1,SQLITE_TRANSIENT);
    bool e=sqlite3_step(s)==SQLITE_ROW; sqlite3_finalize(s); return e;
}

size_t countUserWhales(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT COUNT(*) FROM user_whales WHERE user_id=?")) return 0;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    size_t n=0; if (sqlite3_step(s)==SQLITE_ROW) n=sqlite3_column_int64(s,0); sqlite3_finalize(s); return n;
}


AddWhaleResult addUserWhale(const std::string& chatId, const std::string& address, const std::string& label) {
    if (!isValidAddress(address)) return AddWhaleResult::BAD_ADDRESS;
    ensureUser(chatId);

    // Сервисному аккаунту нельзя повторно взять на отслеживание кошелёк,
    // который детектор пометил как бота - чтобы не вернуть его случайно.
    // На обычных пользователей это не распространяется: им можно следить
    // за любым адресом, бан касается только рейтинга и сервисного аккаунта.
    if (chatId == SERVICE_CHAT_ID && isPermanentlyBanned(address)) {
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

    if (!prepareOrLog(db,&s,"INSERT INTO user_whales(user_id,whale_id,label,created_at) VALUES(?,?,?,?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,whaleId);
    sqlite3_bind_text(s,3,label.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,4,time(nullptr));
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

bool removeUserWhale(const std::string& chatId, const std::string& address) {
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
    if (sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] removeUserWhale COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
        return false;
    }
    return removed;
}


// ================================ Меню =================================

namespace TelegramUI {

UIMessage buildWalletsList(const std::string& chatId, int page) {

    bool premium = isPremium(chatId) || chatId == SERVICE_CHAT_ID;
    Lang lang = langFromCode(getUserLanguage(chatId));

    std::vector<std::pair<std::string, std::string>> walletRows;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (!prepareOrLog(db, &s,
            "SELECT wa.address, uw.label FROM user_whales uw "
            "JOIN whale_addresses wa ON wa.id = uw.whale_id "
            "WHERE uw.user_id = ? ORDER BY uw.created_at")) {
            return {tr(lang, "err_loading_wallets"), ""};
        }
        sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(s) == SQLITE_ROW)
            walletRows.emplace_back(safeColumnText(s, 0), safeColumnText(s, 1));
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
        const std::string& address = walletRows[i].first;
        const std::string& label = walletRows[i].second;
        size_t idx = static_cast<size_t>(i);

        if (i > startIdx) text << "━━━━━━━━━━━━━━━━━━━━━━\n";
        std::string status = premium ? "" : (idx == 0 ? " 🔔" : " ⏸");
        std::string shownLabel = (toLower(label) == address) ? tr(lang, "alert_wallet") : safeString(label, 32);
        TraderStats ts;
        bool hasStats = getTraderStats(address, ts);
        if (hasStats && ts.rank > 0) text << "🏆 <b>#" << ts.rank << "</b>" << status << "\n";
        else text << "🏆 —" << status << "\n";
        text << "👤 <b>" << shownLabel << "</b>\n";
        if (hasStats && ts.rank > 0) {
            text << "💰 " << fmtPnlSigned(ts.pnlNanos)
                 << " | 📈 " << fmtPctSigned(ts.roiPercent)
                 << " | 🎯 " << ts.winRatePercent << "%\n";
        }
        text << "<code>" << shortAddress(address) << "</code>\n\n";

        json row;
        row.push_back({{"text", "📊 " + shortAddress(address)}, {"callback_data", "wstats:" + address}});
        row.push_back({{"text", "✏️"}, {"callback_data", "rename:" + address}});
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
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
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

// ======================= Диспетчеризация callback'ов ====================

void startAddWalletFlow(const std::string& chatId, long long messageId) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    g_sessionManager.setState(chatId, UserState::AWAITING_WALLET_ADDRESS, "", messageId);
    replyInPlace(chatId, messageId, tr(lang, "add_wallet_title"),
            TelegramUI::buildCancelButton(lang));
}

bool handleWalletCallback(const std::string& chatId, const std::string& action, const std::string& param,
                          const std::string& data, long long messageId, const std::string& callbackQueryId) {
    if (false) {}
    else if (action == "mw_page") {
        rememberView(chatId, data);
        int page = 1;
        try { page = std::stoi(param); } catch (...) {}
        auto msg = TelegramUI::buildWalletsList(chatId, page);
        replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    }
    else if (action == "wstats") {
        rememberView(chatId, data);
        Lang lang = langFromCode(getUserLanguage(chatId));
        std::string address = toLower(param);
        std::string wlabel = address;
        {
            std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
            if (prepareOrLog(db,&s,"SELECT uw.label FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id WHERE uw.user_id=? AND wa.address=?")) {
                sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
                sqlite3_bind_text(s,2,address.c_str(),-1,SQLITE_TRANSIENT);
                if (sqlite3_step(s)==SQLITE_ROW) wlabel = safeColumnText(s,0);
                sqlite3_finalize(s);
            }
        }
        TraderStats ts;
        bool hasStats = getTraderStats(address, ts);
        std::stringstream card;
        card << tr(lang, "ws_title") << "\n\n";
        if (hasStats && ts.rank > 0) card << "🏆 " << tr(lang, "ws_rank") << ": <b>#" << ts.rank << "</b>\n";
        else card << "🏆 " << tr(lang, "ws_rank") << ": <b>—</b> " << tr(lang, "ws_not_in_ranking") << "\n";
        card << "👤 <b>" << safeString(wlabel, 32) << "</b>\n";
        if (hasStats && ts.rank > 0) {
            card << "💰 PnL: <b>" << fmtPnlSigned(ts.pnlNanos) << "</b>\n";
            card << "📈 ROI: <b>" << fmtPctSigned(ts.roiPercent) << "</b>\n";
            card << "🎯 " << tr(lang, "ws_winrate") << ": <b>" << ts.winRatePercent << "%</b>\n";
        }
        card << "🔄 " << tr(lang, "ws_trades_30d") << ": <b>" << ts.trades << "</b>\n";
        if (ts.lastTs > 0) {
            time_t t = static_cast<time_t>(ts.lastTs);
            char buf[32];
            strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M UTC", gmtime(&t));
            card << "📅 " << tr(lang, "ws_last_trade") << ": <b>" << buf << "</b>\n";
        } else {
            card << "📅 " << tr(lang, "ws_last_trade") << ": <b>—</b>\n";
        }
        card << "\n<code>" << safeString(address, 42) << "</code>";
        json kb;
        kb["inline_keyboard"] = json::array();
        kb["inline_keyboard"].push_back(json::array({
            {{"text", "🔗 " + chainCtx().explorerName}, {"url", chainCtx().explorerUrl + "/address/" + address}}
        }));
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "back_button")}, {"callback_data", backToWalletsData(chatId)}}
        }));
        replyInPlace(chatId, messageId, card.str(), kb.dump());
    }
    else if (action == "rename") {
        std::string address = toLower(param);
        Lang lang = langFromCode(getUserLanguage(chatId));
        if (!isValidAddress(address)) {
            replyInPlace(chatId, messageId, tr(lang, "err_invalid_address"), errorBackKeyboard(chatId, lang));
            return true;
        }

        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (!prepareOrLog(db, &s,
            "SELECT uw.label FROM user_whales uw "
            "JOIN whale_addresses wa ON wa.id = uw.whale_id "
            "WHERE uw.user_id = ? AND wa.address = ?")) {
            replyInPlace(chatId, messageId, tr(lang, "err_loading_wallet"), errorBackKeyboard(chatId, lang));
            return true;
        }
        sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, address.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(s) == SQLITE_ROW) {
            std::string currentLabel = safeColumnText(s, 0);
            sqlite3_finalize(s);

            g_sessionManager.setState(chatId, UserState::AWAITING_RENAME, address, messageId);
            replyInPlace(chatId, messageId, tr(lang, "rename_title") + "\n\n" + tr(lang, "rename_current_name") + " <b>" + safeString(currentLabel, 32) +
                    "</b>\n\n" + tr(lang, "rename_enter_new"), TelegramUI::buildCancelButton(lang));
        } else {
            sqlite3_finalize(s);
            replyInPlace(chatId, messageId, tr(lang, "err_wallet_not_found"), errorBackKeyboard(chatId, lang));
        }
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
            if (!navigateBack(chatId, messageId)) {
                auto msg = TelegramUI::buildWalletsList(chatId);
                replyInPlace(chatId, messageId, msg.text, msg.keyboard);
            }
        } else {
            if (!callbackQueryId.empty()) answerCallbackQuery(callbackQueryId, tr(lang, "err_wallet_not_found"), true);
            replyInPlace(chatId, messageId, tr(lang, "err_wallet_not_found"), errorBackKeyboard(chatId, lang));
        }
    }
    else return false;
    return true;
}

// ======================= Текстовые состояния ============================

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
            // Возвращаем в список кошельков, а не в главное меню: пользователь
            // сразу видит только что добавленный кошелёк на своём месте.
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

        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (prepareOrLog(db, &s,
                "UPDATE user_whales SET label = ? "
                "WHERE user_id = ? AND whale_id = (SELECT id FROM whale_addresses WHERE address = ?)")) {
                sqlite3_bind_text(s, 1, newLabel.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, chatId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, session.pendingAddress.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
        }

        refreshWatchers();
        std::string back = getLastView(chatId);
        g_sessionManager.clearSession(chatId);
        auto msg = back.empty() ? TelegramUI::buildMainMenu(chatId) : renderViewByData(chatId, back);
        replyInPlace(chatId, session.promptMessageId, tr(lang, "rename_success") + "\n\n" + tr(lang, "rename_new_name") + " <b>" + safeString(newLabel, 32) + "</b>.\n\n" + msg.text,
                msg.keyboard);
        return true;
    }

    return false;
}
