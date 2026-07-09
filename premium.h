#pragma once
// ==================== PREMIUM MODULE ====================
// Telegram Stars (XTR) subscription for Wallet Tracker.
//
// Self-contained: schema migration, premium/expiry checks, activation and
// renewal after payment, plan limits (wallets / Top Traders), invoice
// creation and payment-update handling all live here. main.cpp only calls
// these functions — it holds no subscription logic of its own, so future
// plans / prices can be changed without touching the bot core.
//
// Plans:
//   Free    — 1 tracked wallet, Top 10 Traders
//   Premium — 50 tracked wallets, Top 50 Traders, 30 days per purchase
//
// Buying Premium again before the current subscription ends EXTENDS it
// (current_expire + 30 days); buying after expiry starts from now + 30 days.
//
// All user-facing texts are English-only, per the spec.

#include <string>
#include "json.hpp"

// Runs the users-table migration (is_premium / premium_start /
// premium_expire columns, Unix timestamps) and stores the bot token for
// direct Telegram API calls (sendInvoice / answerPreCheckoutQuery /
// sendMessage). Call once at startup, right after initDB():
//   initPremium(TG_TOKEN, SERVICE_CHAT_ID);
// The token is passed in because TG_TOKEN in main.cpp is a namespace-scope
// `const std::string` and therefore has internal linkage (same reason
// ranking.cpp couldn't extern DB_FILE); SERVICE_CHAT_ID likewise.
//
// serviceChatId (optional) is treated as PERMANENTLY Premium: isPremium()
// returns true for it without touching the DB. In practice this opens
// Top 50 Traders and removes the upsell footer for the service account;
// wallet limits are unaffected — main.cpp's addUserWhale() already
// bypasses the limit check for SERVICE_CHAT_ID entirely (unlimited).
void initPremium(const std::string& botToken, const std::string& serviceChatId = "");

// True iff the user currently has an ACTIVE subscription
// (premium_expire > now). If the stored flag says premium but the expiry
// has passed, the user is automatically downgraded to Free right here —
// so every premium-gated feature only ever needs this one call.
// The service account (see initPremium) is always true, with no expiry.
bool isPremium(const std::string& chatId);

// Unix timestamp of the subscription end (0 if none was ever bought).
long long premiumExpireTs(const std::string& chatId);

// Activates Premium after a successful payment, or extends it:
//   still active  -> premium_expire += 30 days
//   expired/never -> premium_expire  = now + 30 days
// Also records premium_start (kept unchanged when merely extending).
void activateOrExtendPremium(const std::string& chatId);

// ---- Plan limits -------------------------------------------------------
// Max tracked wallets for this user right now (50 premium / 1 free).
// addUserWhale() should compare against this instead of a fixed constant.
size_t premiumMaxWallets(const std::string& chatId);

// How deep this user may see the global Top Traders leaderboards
// (50 premium / 10 free). Pass into the ranking render functions.
int premiumTopTradersLimit(const std::string& chatId);

// ---- UI ----------------------------------------------------------------
// Same {text, keyboard-JSON} convention as TelegramUI::UIMessage.
struct PremiumMessage {
    std::string text;
    std::string keyboard;
};

// The ⭐ Premium page: sales page with "⭐ Buy Premium" when inactive, or
// "⭐ Premium Active / Valid until DD.MM.YYYY" with "🔄 Renew Premium" when
// active. Both buttons send the "premium_buy" callback.
PremiumMessage buildPremiumPage(const std::string& chatId);

// Message shown when a Free user tries to add a 2nd wallet, with the
// "⭐ Upgrade to Premium" button (opens the Premium page).
PremiumMessage buildWalletLimitMessage();

// ---- Payments ----------------------------------------------------------
// Sends the Telegram Stars invoice (currency XTR, empty provider_token,
// payload "premium_30_days"). Returns false if the API call failed.
bool sendPremiumInvoice(const std::string& chatId);

// Answers a pre_checkout_query update (must be answered within 10 s or the
// payment fails). Approves only the known premium payload.
void handlePreCheckoutQuery(const nlohmann::json& preCheckoutQuery);

// Handles message.successful_payment: verifies the payload, activates /
// extends Premium and sends the confirmation message to the user.
// Returns true iff the payload belonged to this module.
bool handleSuccessfulPayment(const std::string& chatId,
                             const nlohmann::json& successfulPayment);
