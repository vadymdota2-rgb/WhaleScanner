#pragma once

#include <string>
#include <cstddef>
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

// --- Меню ---
namespace TelegramUI {
UIMessage buildWalletsList(const std::string& chatId, int page = 1);
UIMessage buildRemoveConfirm(const std::string& address, const std::string& label, Lang lang);
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
