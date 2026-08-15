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

struct TonInvoice {
    std::string memo;
    std::string wallet;
    long long nanoAmount = 0;
    double gramAmount = 0.0;
};

bool tonPaymentsAvailable();
bool createTonInvoice(const std::string& chatId, TonInvoice& out);
void pollTonPayments();

bool grantPremiumDays(const std::string& chatId, int days);

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
