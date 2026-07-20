#pragma once

#include <string>
#include "json.hpp"
#include "ru.h"

void initPremium(const std::string& botToken, const std::string& serviceChatId = "");

bool isPremium(const std::string& chatId);

void cleanupExpiredPremium();

long long premiumExpireTs(const std::string& chatId);

void activateOrExtendPremium(const std::string& chatId);

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
