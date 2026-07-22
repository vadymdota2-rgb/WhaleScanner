#include "premium.h"

#include <sqlite3.h>
#include <mutex>
#include <ctime>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <atomic>
#include "utils.h"
#include "ru.h"

std::string getUserLanguage(const std::string& chatId);

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;

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
constexpr int    PREMIUM_TOP_TRADERS = 100;

std::string g_botToken;

std::string g_serviceChatId;

std::mutex g_lastInvoiceMutex;
std::unordered_map<std::string, long long> g_lastInvoiceMsgId;

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

std::atomic<bool> g_premiumSchemaOk{false};

}

bool initPremium(const std::string& botToken, const std::string& serviceChatId) {
    g_botToken = botToken;
    g_serviceChatId = serviceChatId;

    std::lock_guard<std::mutex> l(dbMutex);

    bool ok = true;
    const char* alters[] = {
        "ALTER TABLE users ADD COLUMN is_premium INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE users ADD COLUMN premium_start INTEGER DEFAULT 0",
        "ALTER TABLE users ADD COLUMN premium_expire INTEGER DEFAULT 0",
    };
    for (const char* sql : alters) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "";
            if (e.find("duplicate column") == std::string::npos) {
                std::cerr << "[PREMIUM][FATAL] migration failed: " << e << std::endl;
                ok = false;
            }
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
        ok = false;
    }
    if (perr) sqlite3_free(perr);

    g_premiumSchemaOk = ok;
    if (ok) {
        std::cout << "[PREMIUM] Module initialized (price: " << PREMIUM_PRICE_STARS
                  << " Stars / 30 days)" << std::endl;
    } else {
        std::cerr << "[PREMIUM][FATAL] Module init failed — payments are DISABLED until fixed" << std::endl;
    }
    return ok;
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
        int rc = sqlite3_step(s);
        if (rc != SQLITE_DONE) {
            std::cerr << "[PREMIUM] cleanup failed: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_finalize(s);
            return;
        }
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

namespace {

bool extendPremiumLocked(const std::string& chatId, long long now, bool& wasAlreadyActive) {
    int flag = 0; long long start = 0, expire = 0;
    if (!readPremiumRowLocked(chatId, flag, start, expire)) return false;

    wasAlreadyActive = (flag != 0 && expire > now);
    long long newExpire = (wasAlreadyActive ? expire : now) + PREMIUM_DURATION_SECONDS;
    long long newStart = wasAlreadyActive ? (start > 0 ? start : now) : now;

    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "UPDATE users SET is_premium=1, premium_start=?, premium_expire=? WHERE chat_id=?"))
        return false;
    sqlite3_bind_int64(s, 1, newStart);
    sqlite3_bind_int64(s, 2, newExpire);
    sqlite3_bind_text(s, 3, chatId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

}

bool activateOrExtendPremium(const std::string& chatId) {
    ensureUser(chatId);
    long long now = static_cast<long long>(time(nullptr));

    std::lock_guard<std::mutex> l(dbMutex);

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[PREMIUM] activate: BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    bool wasAlreadyActive = false;
    if (!extendPremiumLocked(chatId, now, wasAlreadyActive)) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "[PREMIUM] activate: failed for " << chatId << std::endl;
        return false;
    }

    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "[PREMIUM] activate: COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    std::cout << "[PREMIUM] " << (wasAlreadyActive ? "Extended" : "Activated")
              << " for " << chatId << std::endl;
    return true;
}

PaymentApplyResult applySuccessfulPayment(const std::string& chatId, const nlohmann::json& sp) {
    if (!sp.is_object()) return PaymentApplyResult::Rejected;

    std::string payload, currency, chargeId, providerChargeId;
    long long amount = 0;
    try {
        payload = sp.value("invoice_payload", "");
        currency = sp.value("currency", "");
        amount = sp.value("total_amount", 0);
        chargeId = sp.value("telegram_payment_charge_id", "");
        providerChargeId = sp.value("provider_payment_charge_id", "");
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[PREMIUM] successful_payment malformed JSON (chat " << chatId
                  << "): " << e.what() << std::endl;
        return PaymentApplyResult::Rejected;
    }

    if (payload != PREMIUM_PAYLOAD) {
        std::cerr << "[PREMIUM] successful_payment with unknown payload: "
                  << payload << " (chat " << chatId << ")" << std::endl;
        return PaymentApplyResult::Rejected;
    }

    if (currency != "XTR" || amount != PREMIUM_PRICE_STARS) {
        std::cerr << "[PREMIUM] successful_payment amount/currency mismatch: "
                  << amount << " " << currency << " (chat " << chatId << ")" << std::endl;
        return PaymentApplyResult::Rejected;
    }

    if (chargeId.empty()) {
        std::cerr << "[PREMIUM] successful_payment missing telegram_payment_charge_id "
                     "(chat " << chatId << ") — rejected, no history record possible" << std::endl;
        return PaymentApplyResult::Rejected;
    }

    if (!g_premiumSchemaOk) {
        std::cerr << "[PREMIUM] apply: schema not ready, cannot process payment for " << chatId << std::endl;
        return PaymentApplyResult::Error;
    }

    long long paidAt = static_cast<long long>(time(nullptr));

    ensureUser(chatId);

    std::lock_guard<std::mutex> l(dbMutex);

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[PREMIUM] apply: BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return PaymentApplyResult::Error;
    }

    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "INSERT INTO premium_payments"
        "(chat_id,telegram_payment_charge_id,provider_payment_charge_id,"
        "payload,amount,currency,paid_at) VALUES(?,?,?,?,?,?,?)")) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return PaymentApplyResult::Error;
    }
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, chargeId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, providerChargeId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 5, amount);
    sqlite3_bind_text(s, 6, currency.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 7, paidAt);
    int rc = sqlite3_step(s);
    int extendedRc = sqlite3_extended_errcode(db);
    sqlite3_finalize(s);

    if (rc == SQLITE_CONSTRAINT && extendedRc == SQLITE_CONSTRAINT_UNIQUE) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cout << "[PREMIUM] Duplicate payment ignored (charge "
                  << chargeId << ", chat " << chatId << ")" << std::endl;
        return PaymentApplyResult::Duplicate;
    }
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "[PREMIUM] apply: payment INSERT failed (" << rc << "): "
                  << sqlite3_errmsg(db) << std::endl;
        return PaymentApplyResult::Error;
    }

    bool wasAlreadyActive = false;
    if (!extendPremiumLocked(chatId, paidAt, wasAlreadyActive)) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "[PREMIUM] apply: extend failed for " << chatId
                  << " (charge " << chargeId << "), payment record rolled back too" << std::endl;
        return PaymentApplyResult::Error;
    }

    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "[PREMIUM] apply: COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        return PaymentApplyResult::Error;
    }

    std::cout << "[PREMIUM] " << (wasAlreadyActive ? "Extended" : "Activated")
              << " for " << chatId << " (charge " << chargeId << ")" << std::endl;
    {
        std::lock_guard<std::mutex> l(g_lastInvoiceMutex);
        g_lastInvoiceMsgId.erase(chatId);
    }

    return wasAlreadyActive ? PaymentApplyResult::Extended : PaymentApplyResult::Activated;
}


size_t premiumMaxWallets(const std::string& chatId) {
    return isPremium(chatId) ? PREMIUM_MAX_WALLETS : FREE_MAX_WALLETS;
}

int premiumTopTradersLimit(const std::string& chatId) {
    return isPremium(chatId) ? PREMIUM_TOP_TRADERS : FREE_TOP_TRADERS;
}

PremiumMessage buildPremiumPage(const std::string& chatId) {
    Lang lang = langFromCode(getUserLanguage(chatId));

    if (!g_serviceChatId.empty() && chatId == g_serviceChatId) {
        json kb;
        kb["inline_keyboard"] = json::array();
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
        }));
        return {tr(lang, "pr_active_title") + "\n\n" + tr(lang, "pr_service_account"), kb.dump()};
    }

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    bool active = isPremium(chatId);

    std::stringstream text;
    if (active) {
        long long expire = premiumExpireTs(chatId);
        long long secsLeft = expire - static_cast<long long>(time(nullptr));
        long long daysLeft = secsLeft > 0 ? (secsLeft + 86399LL) / 86400LL : 0;
        text << tr(lang, "pr_active_title") << "\n\n"
             << tr(lang, "pr_valid_until_inline") << " <b>" << formatDateDDMMYYYY(expire)
             << "</b> (" << tr(lang, "pr_days_left") << " " << daysLeft << ")\n\n"
             << tr(lang, "pr_active_benefits");

        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "pr_renew")}, {"callback_data", "premium_buy"}}
        }));
    } else {
        text << tr(lang, "pr_title") << "\n\n"
             << tr(lang, "pr_unlock") << "\n\n"
             << tr(lang, "pr_includes") << "\n"
             << tr(lang, "pr_50_wallets") << "\n"
             << tr(lang, "pr_top100") << "\n\n"
             << tr(lang, "pr_subscription_label") << " · "
             << tr(lang, "pr_price_label") << " ⭐ " << PREMIUM_PRICE_STARS << " Stars";

        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "pr_buy")}, {"callback_data", "premium_buy"}}
        }));
    }

    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));

    return {text.str(), keyboard.dump()};
}

PremiumMessage buildWalletLimitMessage(Lang lang) {
    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));

    std::string text =
        tr(lang, "pr_limit_title") + "\n\n" +
        tr(lang, "pr_limit_free") + "\n\n" +
        tr(lang, "pr_limit_upgrade");
    return {text, keyboard.dump()};
}

bool sendPremiumInvoice(const std::string& chatId) {
    if (!g_premiumSchemaOk) {
        std::cerr << "[PREMIUM] sendInvoice blocked: schema not initialized" << std::endl;
        return false;
    }

    Lang lang = langFromCode(getUserLanguage(chatId));

    {
        long long staleId = 0;
        {
            std::lock_guard<std::mutex> l(g_lastInvoiceMutex);
            auto it = g_lastInvoiceMsgId.find(chatId);
            if (it != g_lastInvoiceMsgId.end()) { staleId = it->second; g_lastInvoiceMsgId.erase(it); }
        }
        if (staleId != 0) {
            json d; d["chat_id"] = chatId; d["message_id"] = staleId;
            http(apiUrl("deleteMessage"), d.dump(), 10);
        }
    }

    json j;
    j["chat_id"] = chatId;
    j["title"] = tr(lang, "invoice_title");
    j["description"] = tr(lang, "invoice_description");
    j["payload"] = PREMIUM_PAYLOAD;
    j["provider_token"] = "";
    j["currency"] = "XTR";
    j["prices"] = json::array();
    j["prices"].push_back({{"label", tr(lang, "invoice_price_label")},
                           {"amount", PREMIUM_PRICE_STARS}});

    auto r = http(apiUrl("sendInvoice"), j.dump(), 10);
    try {
        auto p = json::parse(r);
        if (p.value("ok", false)) {
            if (p.contains("result") && p["result"].is_object() && p["result"].contains("message_id")) {
                long long msgId = p["result"]["message_id"].get<long long>();
                std::lock_guard<std::mutex> l(g_lastInvoiceMutex);
                g_lastInvoiceMsgId[chatId] = msgId;
            }
            return true;
        }
        std::cerr << "[PREMIUM] sendInvoice failed: "
                  << p.value("description", "(no description)") << std::endl;
    } catch (...) {
        std::cerr << "[PREMIUM] sendInvoice: bad API response" << std::endl;
    }
    return false;
}

void handlePreCheckoutQuery(const json& q) {
    if (!q.is_object() || !q.contains("id") || !q["id"].is_string()) return;

    std::string payload, currency;
    long long amount = 0;
    bool parseOk = true;
    try {
        payload = q.value("invoice_payload", "");
        currency = q.value("currency", "");
        amount = q.value("total_amount", 0);
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[PREMIUM] pre_checkout malformed JSON: " << e.what() << std::endl;
        parseOk = false;
    }

    Lang lang = Lang::EN;
    if (q.contains("from") && q["from"].is_object() &&
        q["from"].contains("id") && q["from"]["id"].is_number_integer()) {
        std::string buyerChatId = std::to_string(q["from"]["id"].get<long long>());
        lang = langFromCode(getUserLanguage(buyerChatId));
    }

    json a;
    a["pre_checkout_query_id"] = q["id"].get<std::string>();
    bool schemaReady = g_premiumSchemaOk.load();
    bool valid = parseOk && schemaReady && payload == PREMIUM_PAYLOAD && currency == "XTR" && amount == PREMIUM_PRICE_STARS;
    if (valid) {
        a["ok"] = true;
    } else {
        a["ok"] = false;
        a["error_message"] = schemaReady ? tr(lang, "invoice_unknown_product") : tr(lang, "payments_unavailable");
        std::cerr << "[PREMIUM] pre_checkout rejected: schemaReady=" << schemaReady
                  << " payload=" << payload << " currency=" << currency << " amount=" << amount << std::endl;
    }

    auto r = http(apiUrl("answerPreCheckoutQuery"), a.dump(), 8);
    try {
        auto p = json::parse(r);
        if (!p.value("ok", false)) {
            std::cerr << "[PREMIUM] answerPreCheckoutQuery failed: "
                      << p.value("description", "(no description)") << std::endl;
        }
    } catch (...) {
        std::cerr << "[PREMIUM] answerPreCheckoutQuery: bad API response" << std::endl;
    }
}

bool handleSuccessfulPayment(const std::string& chatId, const json& sp) {
    PaymentApplyResult result = applySuccessfulPayment(chatId, sp);

    if (result == PaymentApplyResult::Rejected) return false;
    if (result == PaymentApplyResult::Error) return false;
    if (result == PaymentApplyResult::Duplicate) return true;

    refreshWatchers();

    Lang lang = langFromCode(getUserLanguage(chatId));
    json j;
    j["chat_id"] = chatId;
    j["text"] = tr(lang, "payment_success_title") + "\n\n" +
                tr(lang, "payment_success_activated") + "\n\n" +
                tr(lang, "payment_success_duration");
    auto r = http(apiUrl("sendMessage"), j.dump(), 10);
    try {
        auto p = json::parse(r);
        if (!p.value("ok", false)) {
            std::cerr << "[PREMIUM] success notification failed for " << chatId
                      << ": " << p.value("description", "(no description)") << std::endl;
        }
    } catch (...) {
        std::cerr << "[PREMIUM] success notification: bad API response for " << chatId << std::endl;
    }
    return true;
}

