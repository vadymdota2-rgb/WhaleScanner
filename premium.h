#pragma once

#include <string>
#include <vector>
#include <set>
#include "json.hpp"
#include "ru.h"

bool initPremium(const std::string& botToken, const std::string& serviceChatId = "");

bool isPremium(const std::string& chatId);

std::set<std::string> premiumSubsetOf(const std::vector<std::string>& chatIds);

void cleanupExpiredPremium();

long long premiumExpireTs(const std::string& chatId);

/** Счёт на оплату подписки в USD₮ сети TON. */
struct UsdtInvoice {
    std::string memo;
    std::string wallet;
    /** Сумма в единицах жетона: у USD₮ шесть знаков после запятой. */
    long long units = 0;
    double amount = 0.0;
};

bool tonPaymentsAvailable();
bool createUsdtInvoice(const std::string& chatId, UsdtInvoice& out);
/** Опрос переводов USD₮ — и по счетам из чата, и по счетам из мини-аппа:
 *  занять счёт одним обновлением, потом выдать подписку. */
void pollUsdtPayments();

bool grantPremiumDays(const std::string& chatId, int days);

/** Забыть чат в памяти модуля: вызывается при удалении данных по /forgetme. */
void premiumForgetChat(const std::string& chatId);

size_t premiumMaxWallets(const std::string& chatId);

int premiumTopTradersLimit(const std::string& chatId);

struct PremiumMessage {
    std::string text;
    std::string keyboard;
};

PremiumMessage buildPremiumPage(const std::string& chatId);

PremiumMessage buildWalletLimitMessage(Lang lang);

bool sendPremiumInvoice(const std::string& chatId);

void handlePreCheckoutQuery(const nlohmann::json& preCheckoutQuery);

bool handleSuccessfulPayment(const std::string& chatId,
                             const nlohmann::json& successfulPayment);
