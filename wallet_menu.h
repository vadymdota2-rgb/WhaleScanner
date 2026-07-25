#pragma once

#include <string>
#include <cstddef>
#include <vector>
#include <boost/multiprecision/cpp_int.hpp>
using boost::multiprecision::cpp_int;
#include "alert_settings.h"
#include "ru.h"

// --- Сервисы main.cpp, используемые модулем кошельков (определены в main.cpp) ---
std::string getUserLanguage(const std::string& chatId);
void ensureUser(const std::string& chatId);
void rememberView(const std::string& chatId, const std::string& data);
std::string getLastView(const std::string& chatId);
bool navigateBack(const std::string& chatId, long long messageId);
void answerCallbackQuery(const std::string& callbackQueryId, const std::string& text = "", bool showAlert = false);
TelegramUI::UIMessage renderViewByData(const std::string& chatId, const std::string& data);
extern const std::string SERVICE_CHAT_ID;

// --- Форматирование, используемое в кошельковых представлениях ---
std::string shortAddress(const std::string& a);
std::string fmtPnlSigned(long long pnlNanos);
std::string fmtPctSigned(double p);

// --- Операции хранилища кошельков ---
enum class AddWhaleResult { OK, ALREADY_EXISTS, LIMIT_REACHED, BAD_ADDRESS, PERMANENTLY_BANNED, ERROR };

bool isTrackingWallet(const std::string& chatId, const std::string& address);
size_t countUserWhales(const std::string& chatId);
AddWhaleResult addUserWhale(const std::string& chatId, const std::string& address, const std::string& label);
bool removeUserWhale(const std::string& chatId, const std::string& address);
// Снимает кошелёк с СЕРВИСНОГО отслеживания (вызывается детектором ботов из
// ranking). Пользователей не затрагивает - они могут следить за ним как обычно.
void untrackWalletFromService(const std::string& wallet);

// --- Меню ---
namespace TelegramUI {
UIMessage buildWalletsList(const std::string& chatId, int page = 1);
UIMessage buildRemoveConfirm(const std::string& chatId, const std::string& address, const std::string& label, Lang lang);
}

// Одна позиция портфеля: монета, её количество и стоимость в долларах.
struct PortfolioItem {
    std::string symbol;
    std::string amount;    // уже отформатированное количество
    cpp_int usdNanos = 0;
};

// --- Сервисы main.cpp для экрана "Холд" ---
std::vector<std::string> getWalletTokens(const std::string& wallet, int limit);
bool getNativeBalance(const std::string& wallet, cpp_int& out);
bool getTokenBalance(const std::string& token, const std::string& wallet, cpp_int& out);
void forgetWalletToken(const std::string& wallet, const std::string& token);
int getDecimals(const std::string& addr);
std::string getSymbol(const std::string& addr);
uint64_t getPriceNanos(const std::string& token);

namespace TelegramUI {
UIMessage buildAccountMenu(const std::string& chatId);
UIMessage buildHoldWalletList(const std::string& chatId, int page = 1);
UIMessage buildHoldCard(const std::string& chatId, const std::string& address);
}

// --- Точки входа диспетчеризации ---
// Запуск диалога добавления кошелька (menu:add_wallet)
void startAddWalletFlow(const std::string& chatId, long long messageId);
// Обработка callback'ов: mw_page, wstats, rename, askremove, remove. true = обработано.
bool handleWalletCallback(const std::string& chatId, const std::string& action, const std::string& param,
                          const std::string& data, long long messageId, const std::string& callbackQueryId);
// Обработка текстовых состояний: AWAITING_WALLET_ADDRESS / AWAITING_WALLET_NAME /
// AWAITING_TRACK_NAME / AWAITING_RENAME. true = обработано.
bool handleWalletText(const std::string& chatId, const std::string& text, const UserSession& session);
