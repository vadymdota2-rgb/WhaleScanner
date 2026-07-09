#include "premium.h"

#include <sqlite3.h>
#include <mutex>
#include <ctime>
#include <cstdio>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;

bool prepareOrLog(sqlite3* db, sqlite3_stmt** stmt, const char* sql);
std::string safeColumnText(sqlite3_stmt* stmt, int col);
std::string http(const std::string& url, const std::string& post, int timeout);
void ensureUser(const std::string& chatId);
void refreshWatchers();

namespace {

constexpr long long PREMIUM_DURATION_SECONDS = 30LL * 86400LL;
constexpr int       PREMIUM_PRICE_STARS      = 250;
const char* const   PREMIUM_PAYLOAD          = "premium_30_days";

constexpr size_t FREE_MAX_WALLETS    = 1;
constexpr size_t PREMIUM_MAX_WALLETS = 50;
constexpr int    FREE_TOP_TRADERS    = 10;
constexpr int    PREMIUM_TOP_TRADERS = 50;

std::string g_botToken;

std::string g_serviceChatId;

std::string apiUrl(const char* method) {
    return "https://api.telegram.org/bot" + g_botToken + "/" + method;
}

std::string formatDateDDMMYYYY(long long ts) {
    time_t t = static_cast<time_t>(ts);
    struct tm tmv{};
    localtime_r(&t, &tmv);
    char buf[16];
    if (std::strftime(buf, sizeof(buf), "%d.%m.%Y", &tmv) == 0) return "??.??.????";
    return buf;
}

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

}

void initPremium(const std::string& botToken, const std::string& serviceChatId) {
    g_botToken = botToken;
    g_serviceChatId = serviceChatId;

    std::lock_guard<std::mutex> l(dbMutex);

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

    const char* paymentsSql = R"(
        CREATE TABLE IF NOT EXISTS premium_payments (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            chat_id TEXT NOT NULL,
            telegram_payment_charge_id TEXT NOT NULL UNIQUE,
            provider_payment_charge_id TEXT,
            payload TEXT NOT NULL,
            amount INTEGER NOT NULL,
            currency TEXT NOT NULL,
            paid_at INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_premium_chat ON premium_payments(chat_id);
        CREATE INDEX IF NOT EXISTS idx_premium_paid ON premium_payments(paid_at);
    )";
    char* perr = nullptr;
    if (sqlite3_exec(db, paymentsSql, nullptr, nullptr, &perr) != SQLITE_OK) {
        std::cerr << "[PREMIUM][FATAL] premium_payments schema failed: "
                  << (perr ? perr : "") << std::endl;
    }
    if (perr) sqlite3_free(perr);

    std::cout << "[PREMIUM] Module initialized (price: " << PREMIUM_PRICE_STARS
              << " Stars / 30 days)" << std::endl;
}

bool isPremium(const std::string& chatId) {

    if (!g_serviceChatId.empty() && chatId == g_serviceChatId) return true;

    long long now = static_cast<long long>(time(nullptr));

    std::lock_guard<std::mutex> l(dbMutex);
    int flag = 0; long long start = 0, expire = 0;
    if (!readPremiumRowLocked(chatId, flag, start, expire)) return false;
    return flag != 0 && expire > now;
}

void cleanupExpiredPremium() {
    long long now = static_cast<long long>(time(nullptr));
    int changed = 0;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (!prepareOrLog(db, &s,
            "UPDATE users SET is_premium=0 WHERE is_premium=1 AND premium_expire<=?"))
            return;
        sqlite3_bind_int64(s, 1, now);
        sqlite3_step(s);
        changed = sqlite3_changes(db);
        sqlite3_finalize(s);
    }
    if (changed > 0) {
        std::cout << "[PREMIUM] Expired -> Free: " << changed << " user(s)" << std::endl;

        refreshWatchers();
    }
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

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[PREMIUM] activate: BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    int flag = 0; long long start = 0, expire = 0;
    if (!readPremiumRowLocked(chatId, flag, start, expire)) {

        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "[PREMIUM] activate: user row missing for " << chatId << std::endl;
        return;
    }

    bool stillActive = (flag != 0 && expire > now);

    long long newExpire = (stillActive ? expire : now) + PREMIUM_DURATION_SECONDS;
    long long newStart = stillActive ? (start > 0 ? start : now) : now;

    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "UPDATE users SET is_premium=1, premium_start=?, premium_expire=? WHERE chat_id=?")) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return;
    }
    sqlite3_bind_int64(s, 1, newStart);
    sqlite3_bind_int64(s, 2, newExpire);
    sqlite3_bind_text(s, 3, chatId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);

    if (rc != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "[PREMIUM] activate: UPDATE failed (" << rc << ") for "
                  << chatId << ", rolled back" << std::endl;
        return;
    }
    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "[PREMIUM] activate: COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    std::cout << "[PREMIUM] " << (stillActive ? "Extended" : "Activated")
              << " for " << chatId << " until " << newExpire << std::endl;
}

size_t premiumMaxWallets(const std::string& chatId) {
    return isPremium(chatId) ? PREMIUM_MAX_WALLETS : FREE_MAX_WALLETS;
}

int premiumTopTradersLimit(const std::string& chatId) {
    return isPremium(chatId) ? PREMIUM_TOP_TRADERS : FREE_TOP_TRADERS;
}

PremiumMessage buildPremiumPage(const std::string& chatId) {

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

bool sendPremiumInvoice(const std::string& chatId) {

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

    http(apiUrl("answerPreCheckoutQuery"), a.dump(), 10);
}

namespace {

bool recordPaymentOnce(const std::string& chatId, const std::string& chargeId,
                       const nlohmann::json& sp) {
    std::string providerChargeId = sp.value("provider_payment_charge_id", "");
    std::string payload = sp.value("invoice_payload", "");
    long long amount = sp.value("total_amount", 0);
    std::string currency = sp.value("currency", "");
    long long paidAt = static_cast<long long>(time(nullptr));

    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "INSERT OR IGNORE INTO premium_payments"
        "(chat_id,telegram_payment_charge_id,provider_payment_charge_id,"
        "payload,amount,currency,paid_at) VALUES(?,?,?,?,?,?,?)")) {

        return true;
    }
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, chargeId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, providerChargeId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 5, amount);
    sqlite3_bind_text(s, 6, currency.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 7, paidAt);
    sqlite3_step(s);
    bool inserted = sqlite3_changes(db) > 0;
    sqlite3_finalize(s);
    return inserted;
}

}

bool handleSuccessfulPayment(const std::string& chatId, const json& sp) {
    if (!sp.is_object()) return false;
    std::string payload = sp.value("invoice_payload", "");
    if (payload != PREMIUM_PAYLOAD) {
        std::cerr << "[PREMIUM] successful_payment with unknown payload: "
                  << payload << " (chat " << chatId << ")" << std::endl;
        return false;
    }

    std::string chargeId = sp.value("telegram_payment_charge_id", "");
    if (chargeId.empty()) {

        std::cerr << "[PREMIUM] ⚠️ successful_payment without "
                     "telegram_payment_charge_id (chat " << chatId
                  << ") — activating without a history record" << std::endl;
    } else if (!recordPaymentOnce(chatId, chargeId, sp)) {
        std::cout << "[PREMIUM] Duplicate payment ignored (charge "
                  << chargeId << ", chat " << chatId << ")" << std::endl;
        return true;
    }

    activateOrExtendPremium(chatId);

    refreshWatchers();

    json j;
    j["chat_id"] = chatId;
    j["text"] = "✅ Payment successful!\n\n"
                "Wallet Tracker Premium has been activated.\n\n"
                "Valid for 30 days.";
    http(apiUrl("sendMessage"), j.dump(), 10);
    return true;
}
