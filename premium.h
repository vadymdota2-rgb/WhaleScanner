#pragma once

#include <string>
#include <vector>
#include <set>
#include "json.hpp"

/*
 * Премиум. Купить его можно только в приложении: счёт в звёздах создаёт
 * whale_api.py (createInvoiceLink), счёт в USD₮ он же кладёт в ton_invoices.
 * Бот подтверждает оплату звёздами, видит приход USD₮ и продлевает срок —
 * выдача подписки живёт здесь, в одном месте.
 */

bool initPremium(const std::string& serviceChatId = "");

bool isPremium(const std::string& chatId);

std::set<std::string> premiumSubsetOf(const std::vector<std::string>& chatIds);

void cleanupExpiredPremium();

/** Опрос переводов USD₮ по счетам из приложения: занять счёт одним
 *  обновлением, потом выдать подписку. */
void pollUsdtPayments();

bool grantPremiumDays(const std::string& chatId, int days);

/** Событие воронки продаж (таблица funnel_events, её же пишет API). */
void trackFunnel(const std::string& chatId, const char* ev, const char* src = "");

size_t premiumMaxWallets(const std::string& chatId);

void handlePreCheckoutQuery(const nlohmann::json& preCheckoutQuery);

bool handleSuccessfulPayment(const std::string& chatId,
                             const nlohmann::json& successfulPayment);
