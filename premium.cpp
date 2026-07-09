#include "premium.h"

#include <sqlite3.h>
#include <mutex>
#include <ctime>
#include <cstdio>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

// ---- Shared state / helpers from main.cpp ----
// Ordinary (non-static) globals and free functions defined in main.cpp;
// declaring them here gives this translation unit the symbols it needs
// without touching main.cpp's own definitions (same pattern as ranking.cpp).
extern sqlite3* db;
extern std::mutex dbMutex;

bool prepareOrLog(sqlite3* db, sqlite3_stmt** stmt, const char* sql);
std::string safeColumnText(sqlite3_stmt* stmt, int col);
std::string http(const std::string& url, const std::string& post, int timeout);
void ensureUser(const std::string& chatId);
void refreshWatchers();

namespace {

// ==================== TARIFF CONSTANTS ====================
// The single place to change when tariffs evolve. New subscription types
// would become new payloads + rows in this style.
constexpr long long PREMIUM_DURATION_SECONDS = 30LL * 86400LL;  // 30 days
constexpr int       PREMIUM_PRICE_STARS      = 250;             // ⭐ XXX — set the real price here
const char* const   PREMIUM_PAYLOAD          = "premium_30_days";

constexpr size_t FREE_MAX_WALLETS    = 1;
constexpr size_t PREMIUM_MAX_WALLETS = 50;
constexpr int    FREE_TOP_TRADERS    = 10;
constexpr int    PREMIUM_TOP_TRADERS = 50;

std::string g_botToken;
// Chat id that is always treated as Premium (the bot's service account).
// Empty string = no such account. See initPremium() docs in premium.h.
std::string g_serviceChatId;

std::string apiUrl(const char* method) {
    return "https://api.telegram.org/bot" + g_botToken + "/" + method;
}

// DD.MM.YYYY (server-local time), for "Valid until:".
std::string formatDateDDMMYYYY(long long ts) {
    time_t t = static_cast<time_t>(ts);
    struct tm tmv{};
    localtime_r(&t, &tmv);
    char buf[16];
    if (std::strftime(buf, sizeof(buf), "%d.%m.%Y", &tmv) == 0) return "??.??.????";
    return buf;
}

// Reads (is_premium, premium_start, premium_expire) for a user.
// Returns false if the user row doesn't exist. Caller must hold dbMutex.
bool readPremiumRowLocked(const std::string& chatId,
                          int& flag, long long& start, long long& expire) {
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "SELECT is_premium, premium_start, premium_expire FROM users WHERE chat_id=?"))
        return false;
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        found = true;
        flag = sqlite3_column_int(s, 0);
        start = sqlite3_column_int64(s, 1);
        expire = sqlite3_column_int64(s, 2);
    }
    sqlite3_finalize(s);
    return found;
}

} // namespace

// ==================== INIT / SCHEMA ====================
void initPremium(const std::string& botToken, const std::string& serviceChatId) {
    g_botToken = botToken;
    g_serviceChatId = serviceChatId;

    std::lock_guard<std::mutex> l(dbMutex);
    // SQLite has no "ADD COLUMN IF NOT EXISTS": run each ALTER and treat the
    // "duplicate column name" error as the normal already-migrated case.
    const char* alters[] = {
        "ALTER TABLE users ADD COLUMN is_premium INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE users ADD COLUMN premium_start INTEGER DEFAULT 0",
        "ALTER TABLE users ADD COLUMN premium_expire INTEGER DEFAULT 0",
    };
    for (const char* sql : alters) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "";
            if (e.find("duplicate column") == std::string::npos)
                std::cerr << "[PREMIUM][FATAL] migration failed: " << e << std::endl;
        }
        if (err) sqlite3_free(err);
    }
    std::cout << "[PREMIUM] Module initialized (price: " << PREMIUM_PRICE_STARS
              << " Stars / 30 days)" << std::endl;
}

// ==================== PREMIUM STATE ====================
bool isPremium(const std::string& chatId) {
    // The service account is permanently Premium — no DB row, no expiry.
    // (Opens Top 50 Traders and removes upsell footers for it; its wallet
    // limit is already bypassed separately in main.cpp's addUserWhale.)
    if (!g_serviceChatId.empty() && chatId == g_serviceChatId) return true;

    long long now = static_cast<long long>(time(nullptr));

    std::lock_guard<std::mutex> l(dbMutex);
    int flag = 0; long long start = 0, expire = 0;
    if (!readPremiumRowLocked(chatId, flag, start, expire)) return false;

    if (flag != 0 && expire > now) return true;

    if (flag != 0) {
        // Subscription ran out — automatically move the user back to Free.
        sqlite3_stmt* u;
        if (prepareOrLog(db, &u, "UPDATE users SET is_premium=0 WHERE chat_id=?")) {
            sqlite3_bind_text(u, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(u);
            sqlite3_finalize(u);
            std::cout << "[PREMIUM] Expired -> Free: " << chatId << std::endl;
        }
    }
    return false;
}

long long premiumExpireTs(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex);
    int flag = 0; long long start = 0, expire = 0;
    if (!readPremiumRowLocked(chatId, flag, start, expire)) return 0;
    return expire;
}

void activateOrExtendPremium(const std::string& chatId) {
    ensureUser(chatId);
    long long now = static_cast<long long>(time(nullptr));

    std::lock_guard<std::mutex> l(dbMutex);
    int flag = 0; long long start = 0, expire = 0;
    readPremiumRowLocked(chatId, flag, start, expire);

    bool stillActive = (flag != 0 && expire > now);
    // Renewal before expiry EXTENDS the current subscription;
    // after expiry (or first purchase) it starts fresh from now.
    long long newExpire = (stillActive ? expire : now) + PREMIUM_DURATION_SECONDS;
    long long newStart = stillActive ? (start > 0 ? start : now) : now;

    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "UPDATE users SET is_premium=1, premium_start=?, premium_expire=? WHERE chat_id=?"))
        return;
    sqlite3_bind_int64(s, 1, newStart);
    sqlite3_bind_int64(s, 2, newExpire);
    sqlite3_bind_text(s, 3, chatId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);

    std::cout << "[PREMIUM] " << (stillActive ? "Extended" : "Activated")
              << " for " << chatId << " until " << newExpire << std::endl;
}

// ==================== PLAN LIMITS ====================
size_t premiumMaxWallets(const std::string& chatId) {
    return isPremium(chatId) ? PREMIUM_MAX_WALLETS : FREE_MAX_WALLETS;
}

int premiumTopTradersLimit(const std::string& chatId) {
    return isPremium(chatId) ? PREMIUM_TOP_TRADERS : FREE_TOP_TRADERS;
}

// ==================== UI ====================
PremiumMessage buildPremiumPage(const std::string& chatId) {
    // Service account: permanent access, so no expiry date and no point in
    // showing a Renew button.
    if (!g_serviceChatId.empty() && chatId == g_serviceChatId) {
        json kb;
        kb["inline_keyboard"] = json::array();
        kb["inline_keyboard"].push_back(json::array({
            {{"text", "← Back"}, {"callback_data", "menu:main"}}
        }));
        return {"⭐ <b>Premium Active</b>\n\n"
                "Service account — Premium access is permanent.", kb.dump()};
    }

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    bool active = isPremium(chatId);

    std::stringstream text;
    if (active) {
        long long expire = premiumExpireTs(chatId);
        text << "⭐ <b>Premium Active</b>\n\n"
             << "Your subscription is active.\n\n"
             << "<b>Valid until:</b>\n"
             << formatDateDDMMYYYY(expire);

        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", "🔄 Renew Premium"}, {"callback_data", "premium_buy"}}
        }));
    } else {
        text << "⭐ <b>Wallet Tracker Premium</b>\n\n"
             << "Unlock the full potential of Wallet Tracker.\n\n"
             << "<b>Premium includes:</b>\n"
             << "✅ Track up to 50 wallets\n"
             << "✅ Access Top 50 Traders\n"
             << "(Top 10 available for free)\n\n"
             << "<b>Subscription:</b>\n30 Days\n\n"
             << "<b>Price:</b>\n⭐ " << PREMIUM_PRICE_STARS << " Stars";

        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", "⭐ Buy Premium"}, {"callback_data", "premium_buy"}}
        }));
    }

    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "← Back"}, {"callback_data", "menu:main"}}
    }));

    return {text.str(), keyboard.dump()};
}

PremiumMessage buildWalletLimitMessage() {
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "⭐ Upgrade to Premium"}, {"callback_data", "menu:premium"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "← Back"}, {"callback_data", "menu:main"}}
    }));

    std::string text =
        "⚠️ <b>Wallet limit reached</b>\n\n"
        "Free plan allows tracking only 1 wallet.\n\n"
        "Upgrade to Premium to track up to 50 wallets.";
    return {text, keyboard.dump()};
}

// ==================== PAYMENTS ====================
bool sendPremiumInvoice(const std::string& chatId) {
    // Telegram Stars: currency XTR, provider_token empty, exactly one price
    // item whose amount is the Stars count.
    json j;
    j["chat_id"] = chatId;
    j["title"] = "Wallet Tracker Premium";
    j["description"] = "30-Day Premium Subscription";
    j["payload"] = PREMIUM_PAYLOAD;
    j["provider_token"] = "";
    j["currency"] = "XTR";
    j["prices"] = json::array();
    j["prices"].push_back({{"label", "Premium (30 Days)"},
                           {"amount", PREMIUM_PRICE_STARS}});

    auto r = http(apiUrl("sendInvoice"), j.dump(), 10);
    try {
        auto p = json::parse(r);
        if (p.value("ok", false)) return true;
        std::cerr << "[PREMIUM] sendInvoice failed: "
                  << p.value("description", "(no description)") << std::endl;
    } catch (...) {
        std::cerr << "[PREMIUM] sendInvoice: bad API response" << std::endl;
    }
    return false;
}

void handlePreCheckoutQuery(const json& q) {
    if (!q.is_object() || !q.contains("id") || !q["id"].is_string()) return;

    std::string payload = q.value("invoice_payload", "");

    json a;
    a["pre_checkout_query_id"] = q["id"].get<std::string>();
    if (payload == PREMIUM_PAYLOAD) {
        a["ok"] = true;
    } else {
        a["ok"] = false;
        a["error_message"] = "Unknown product. Please try again.";
        std::cerr << "[PREMIUM] pre_checkout with unknown payload: "
                  << payload << std::endl;
    }
    // Must be answered within 10 seconds or Telegram cancels the payment.
    http(apiUrl("answerPreCheckoutQuery"), a.dump(), 10);
}

bool handleSuccessfulPayment(const std::string& chatId, const json& sp) {
    if (!sp.is_object()) return false;
    std::string payload = sp.value("invoice_payload", "");
    if (payload != PREMIUM_PAYLOAD) {
        std::cerr << "[PREMIUM] successful_payment with unknown payload: "
                  << payload << " (chat " << chatId << ")" << std::endl;
        return false;
    }

    activateOrExtendPremium(chatId);

    // ТЗ №1 п.4: no data migration is needed on re-purchase — every wallet
    // stayed stored in user_whales while the user was Free. Rebuilding the
    // watchers here makes ALL of them active again immediately, instead of
    // waiting for the next periodic refresh.
    refreshWatchers();

    // Confirmation is sent directly (not through the alert delivery queue):
    // a payment confirmation must reach the user immediately.
    json j;
    j["chat_id"] = chatId;
    j["text"] = "✅ Payment successful!\n\n"
                "Wallet Tracker Premium has been activated.\n\n"
                "Valid for 30 days.";
    http(apiUrl("sendMessage"), j.dump(), 10);
    return true;
}
