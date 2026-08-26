#pragma once

#include <string>
#include <cstddef>
#include "alert_settings.h"
#include "ru.h"

std::string getUserLanguage(const std::string& chatId);
void ensureUser(const std::string& chatId, const std::string& tgLangCode = "");
void rememberView(const std::string& chatId, const std::string& data);
std::string getLastView(const std::string& chatId);
bool navigateBack(const std::string& chatId, long long messageId);
void answerCallbackQuery(const std::string& callbackQueryId, const std::string& text = "", bool showAlert = false);
TelegramUI::UIMessage renderViewByData(const std::string& chatId, const std::string& data);
extern const std::string SERVICE_CHAT_ID;

std::string shortAddress(const std::string& a);

enum class AddWhaleResult { OK, ALREADY_EXISTS, LIMIT_REACHED, BAD_ADDRESS, PERMANENTLY_BANNED, ERROR };

bool isTrackingWallet(const std::string& chatId, const std::string& address);
size_t countUserWhales(const std::string& chatId);
AddWhaleResult addUserWhale(const std::string& chatId, const std::string& address, const std::string& label);
bool removeUserWhale(const std::string& chatId, const std::string& address);
void untrackWalletFromService(const std::string& wallet);

namespace TelegramUI {
UIMessage buildWalletsList(const std::string& chatId, int page = 1);
UIMessage buildRemoveConfirm(const std::string& chatId, const std::string& address, const std::string& label, Lang lang);
UIMessage buildAccountMenu(const std::string& chatId);
}

void startAddWalletFlow(const std::string& chatId, long long messageId);
bool handleWalletCallback(const std::string& chatId, const std::string& action, const std::string& param,
                          const std::string& data, long long messageId, const std::string& callbackQueryId);
bool handleWalletText(const std::string& chatId, const std::string& text, const UserSession& session);
