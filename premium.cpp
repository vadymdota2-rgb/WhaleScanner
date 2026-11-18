#include "premium.h"
#include "premium_logic.h"
#include "lifecycle.h"
#include "rpc_client.h"
#include "telegram.h"
#include "utils.h"
#include "ru.h"

#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;

namespace {

using premium::STAR_PLANS;
using premium::starPlan;
using premium::withDays;

constexpr size_t FREE_MAX_WALLETS    = 3;
constexpr size_t PREMIUM_MAX_WALLETS = 50;

// Задаются один раз при запуске (initPremium), до потоков бота.
std::string g_serviceChatId;
std::string g_ownerChatId;
// Схема готова — оплата включена. До initPremium и после неудачного — нет.
std::atomic<bool> g_premiumSchemaOk{false};

// Деньги пришли, а Премиум не выдан — владельцу сразу, а не только в журнал:
// журнал никто не читает, а человек ждёт.
void alertOwner(const std::string& text) {
    if (!g_ownerChatId.empty()) sendMsg(g_ownerChatId, "⚠️ <b>Оплата</b>: " + text);
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

// Ответ Bot API: ok:true — да; иначе причина (description) для журнала.
// Без исключений на пустом и битом ответе.
bool tgAnswerOk(const std::string& resp, std::string& why) {
    const json r = json::parse(resp, nullptr, false);
    if (!r.is_object()) { why = "нет ответа"; return false; }
    const auto ok = r.find("ok");
    if (ok != r.end() && ok->is_boolean() && ok->get<bool>()) return true;
    const auto d = r.find("description");
    why = d != r.end() && d->is_string() ? d->get<std::string>() : "без описания";
    return false;
}

// Тело — внутри BEGIN IMMEDIATE … COMMIT; false из тела или сбой COMMIT —
// ROLLBACK, и в базе не остаётся ничего. Вызывать под dbMutex. API пишет в
// ту же базу из своего процесса: пока он держит запись, BEGIN IMMEDIATE ждёт
// (busy_timeout соединения, initDB), а не падает.
template <class Body>
bool inTransactionLocked(const char* what, Body&& body) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[PREMIUM] " << what << ": BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    if (!body()) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[PREMIUM] " << what << ": COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

// ALTER TABLE … ADD COLUMN: «столбец уже есть» — обычное дело на старой
// базе; любая другая ошибка — оплата выключается.
bool addColumnLocked(const char* sql) {
    char* err = nullptr;
    const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK;
    const std::string e = err ? err : "";
    sqlite3_free(err);
    if (ok) {
        std::cout << "[PREMIUM] migration: " << sql << std::endl;
        return true;
    }
    if (e.find("duplicate column") != std::string::npos) return true;
    std::cerr << "[PREMIUM][FATAL] migration failed: " << e << " (" << sql << ")" << std::endl;
    return false;
}

}

bool initPremium(const std::string& serviceChatId, const std::string& ownerChatId) {
    g_serviceChatId = serviceChatId;
    g_ownerChatId = ownerChatId;

    std::lock_guard<std::mutex> l(dbMutex);

    // Таблицу users заводит бот (initDB) — здесь только столбцы Премиума.
    // Без неё оплату включать нельзя: выдать подписку было бы некуда.
    bool ok = addColumnLocked("ALTER TABLE users ADD COLUMN is_premium INTEGER NOT NULL DEFAULT 0") &&
              addColumnLocked("ALTER TABLE users ADD COLUMN premium_start INTEGER DEFAULT 0") &&
              addColumnLocked("ALTER TABLE users ADD COLUMN premium_expire INTEGER DEFAULT 0");

    // Платежи звёздами и счета USD₮. Счёт в USD₮ кладёт API (whale_api.py,
    // pay_usdt) с kind='usdt' и сроком days; kind='ton' — старые счета на
    // перевод монеты TON из чата, их больше не выставляют.
    const char* tablesSql = R"(
        CREATE TABLE IF NOT EXISTS premium_payments (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            chat_id TEXT NOT NULL,
            telegram_payment_charge_id TEXT NOT NULL UNIQUE,
            provider_payment_charge_id TEXT,
            payload TEXT NOT NULL,
            amount INTEGER NOT NULL,
            currency TEXT NOT NULL,
            paid_at INTEGER NOT NULL,
            sub_until INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_premium_chat ON premium_payments(chat_id);
        CREATE INDEX IF NOT EXISTS idx_premium_paid ON premium_payments(paid_at);

        CREATE TABLE IF NOT EXISTS ton_invoices (
            memo TEXT PRIMARY KEY,
            chat_id TEXT NOT NULL,
            nano_amount INTEGER NOT NULL,
            status TEXT NOT NULL DEFAULT 'active',
            created_at INTEGER NOT NULL,
            paid_at INTEGER NOT NULL DEFAULT 0,
            tx_hash TEXT NOT NULL DEFAULT '',
            kind TEXT NOT NULL DEFAULT 'ton',
            days INTEGER NOT NULL DEFAULT 30
        );
        CREATE INDEX IF NOT EXISTS idx_ton_active ON ton_invoices(status, created_at);
        CREATE INDEX IF NOT EXISTS idx_ton_chat ON ton_invoices(chat_id);
    )";
    if (ok) {
        char* err = nullptr;
        if (sqlite3_exec(db, tablesSql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "[PREMIUM][FATAL] payment tables failed: " << (err ? err : "") << std::endl;
            ok = false;
        }
        sqlite3_free(err);
    }
    // Столбцы, которых не было у таблиц из старых версий: подписка Telegram
    // (до какого времени оплачен период — по нему cancelStarSubscriptions
    // находит, что отменять), вид счёта и его срок (старые счета — месяц).
    ok = ok &&
         addColumnLocked("ALTER TABLE premium_payments ADD COLUMN sub_until INTEGER NOT NULL DEFAULT 0") &&
         addColumnLocked("ALTER TABLE ton_invoices ADD COLUMN kind TEXT NOT NULL DEFAULT 'ton'") &&
         addColumnLocked("ALTER TABLE ton_invoices ADD COLUMN days INTEGER NOT NULL DEFAULT 30");

    // Сервисному аккаунту Премиума не бывает: старую отметку в базе снимаем,
    // чтобы он не считался подписчиком ни в /stats, ни в API.
    if (ok && !g_serviceChatId.empty()) {
        sqlite3_stmt* s = nullptr;
        if (prepareOrLog(db, &s, "UPDATE users SET is_premium=0, premium_expire=0 WHERE chat_id=?")) {
            sqlite3_bind_text(s, 1, g_serviceChatId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }

    g_premiumSchemaOk = ok;
    if (ok)
        std::cout << "[PREMIUM] Module initialized (plans: " << STAR_PLANS[0].stars << " Stars / "
                  << STAR_PLANS[0].days << " days, " << STAR_PLANS[1].stars << " / " << STAR_PLANS[1].days
                  << ")" << std::endl;
    else
        std::cerr << "[PREMIUM][FATAL] Module init failed — payments are DISABLED until fixed" << std::endl;
    return ok;
}

std::set<std::string> premiumSubsetOf(const std::vector<std::string>& chatIds) {
    std::set<std::string> out;
    if (chatIds.empty() || !g_premiumSchemaOk) return out;

    const long long now = static_cast<long long>(time(nullptr));
    // Кусками: у SQLite предел числа параметров в запросе (в старых сборках —
    // 999), и алерт большому кругу людей иначе ушёл бы всем как бесплатным.
    constexpr size_t CHUNK = 500;

    std::lock_guard<std::mutex> l(dbMutex);
    for (size_t from = 0; from < chatIds.size(); from += CHUNK) {
        const size_t n = std::min(CHUNK, chatIds.size() - from);
        std::string sql =
            "SELECT chat_id FROM users WHERE is_premium=1 AND premium_expire>? AND chat_id IN (";
        for (size_t i = 0; i < n; i++) sql += (i ? ",?" : "?");
        sql += ")";
        sqlite3_stmt* s;
        if (!prepareOrLog(db, &s, sql.c_str())) return {};
        sqlite3_bind_int64(s, 1, now);
        for (size_t i = 0; i < n; i++)
            sqlite3_bind_text(s, static_cast<int>(i + 2), chatIds[from + i].c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(s) == SQLITE_ROW) out.insert(safeColumnText(s, 0));
        sqlite3_finalize(s);
    }
    // Сервисный аккаунт — не подписчик, даже если в базе осталась отметка.
    if (!g_serviceChatId.empty()) out.erase(g_serviceChatId);
    return out;
}

/* Сервисный аккаунт держит всю базу кошельков, но Премиума у него нет и быть
   не может. Всё, что ему нужно, сделано отдельно: кошельки без лимита
   (addUserWhale), сканирование всех его адресов (refreshWatchers), и алерты
   ему не уходят вовсе. */
bool isPremium(const std::string& chatId) {
    if (!g_serviceChatId.empty() && chatId == g_serviceChatId) return false;
    const long long now = static_cast<long long>(time(nullptr));
    std::lock_guard<std::mutex> l(dbMutex);
    int flag = 0; long long start = 0, expire = 0;
    if (!readPremiumRowLocked(chatId, flag, start, expire)) return false;
    return flag != 0 && expire > now;
}

void cleanupExpiredPremium() {
    const long long now = static_cast<long long>(time(nullptr));
    std::vector<std::string> expired;
    bool ok = false;
    {
        // Кто истёк и снятие отметки — одной транзакцией: иначе API, в ту же
        // секунду продливший кому-то срок, оставил бы его в списке истёкших,
        // и человек с действующим Премиумом получил бы письмо «Премиум
        // закончился».
        std::lock_guard<std::mutex> l(dbMutex);
        ok = inTransactionLocked("cleanup", [&]() {
            sqlite3_stmt* s;
            if (!prepareOrLog(db, &s, "SELECT chat_id FROM users WHERE is_premium=1 AND premium_expire<=?"))
                return false;
            sqlite3_bind_int64(s, 1, now);
            while (sqlite3_step(s) == SQLITE_ROW) expired.push_back(safeColumnText(s, 0));
            sqlite3_finalize(s);
            if (expired.empty()) return true;
            if (!prepareOrLog(db, &s, "UPDATE users SET is_premium=0 WHERE is_premium=1 AND premium_expire<=?"))
                return false;
            sqlite3_bind_int64(s, 1, now);
            const int rc = sqlite3_step(s);
            sqlite3_finalize(s);
            return rc == SQLITE_DONE;
        });
    }
    if (!ok || expired.empty()) return;
    std::cout << "[PREMIUM] Expired -> Free: " << expired.size() << " user(s)" << std::endl;
    refreshWatchers();
    // Письмо об окончании: после пробной недели — с итогом и вводной ценой,
    // после оплаченного срока — что остаётся бесплатно.
    for (const std::string& cid : expired)
        if (!cid.empty() && cid != g_serviceChatId) sendPremiumEnded(cid);
}

namespace {

bool extendPremiumLocked(const std::string& chatId, long long now, int days, bool& wasAlreadyActive) {
    int flag = 0; long long start = 0, expire = 0;
    if (!readPremiumRowLocked(chatId, flag, start, expire)) return false;
    const auto term = premium::extendTerm(flag, start, expire, now, days);
    if (!term) {
        std::cerr << "[PREMIUM] extend: срок не продлить (chat " << chatId
                  << ", expire=" << expire << ", days=" << days << ")" << std::endl;
        return false;
    }
    wasAlreadyActive = term->wasActive;

    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "UPDATE users SET is_premium=1, premium_start=?, premium_expire=? WHERE chat_id=?"))
        return false;
    sqlite3_bind_int64(s, 1, term->start);
    sqlite3_bind_int64(s, 2, term->expire);
    sqlite3_bind_text(s, 3, chatId.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1;
}

// Переводы жетонов отдаёт только третья версия обозревателя: во второй
// пришлось бы разбирать тело сообщения вручную.
const char* const TON_API_V3 = "https://toncenter.com/api/v3/";

// Кошелёк приёма — тот же, что показывает API (whale_api.py, TON_WALLET):
// одна переменная окружения и одно значение по умолчанию у обоих.
std::string tonWallet() {
    const char* a = std::getenv("TON_WALLET_ADDRESS");
    if (a && *a) return a;
    return "UQDAiNYvy2KUIwjEcgD1ZxPVw-CPwdk4WbBQwpVsQQ5jsO6o";
}

// Опоздавшую оплату принимаем сутки: API показывает счёт час (PAY_TTL), но
// человек, заплативший позже, не должен терять деньги.
constexpr long long USDT_INVOICE_LIVE_SEC = 86400;

// Одно сообщение на случай, а не каждые 20 секунд опроса.
bool firstTime(std::set<std::string>& seen, const std::string& key) {
    if (seen.size() > 1000) seen.clear();
    return seen.insert(key).second;
}

}

void pollUsdtPayments() {
    // Счёт в USD₮ выставляет API, платит человек из своего кошелька с памяткой
    // счёта. Здесь: найти перевод с памяткой живого счёта, занять счёт и
    // продлить срок одной транзакцией, потом сообщить человеку. Зовётся из
    // главного цикла бота, всегда из одного потока.
    if (!g_premiumSchemaOk) return;
    static std::set<std::string> said;
    const std::string wallet = tonWallet();
    const std::string walletRaw = premium::tonRawAddress(wallet);
    if (walletRaw.empty()) {
        if (firstTime(said, "wallet"))
            std::cerr << "[USDT] адрес кошелька приёма не разобран — оплату USD₮ не проверяю" << std::endl;
        return;
    }

    const long long now = static_cast<long long>(time(nullptr));
    bool anyActive = false;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s,
            "UPDATE ton_invoices SET status='expired' WHERE status='active' AND kind='usdt' AND created_at <= ?")) {
            sqlite3_bind_int64(s, 1, now - USDT_INVOICE_LIVE_SEC);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        if (prepareOrLog(db, &s, "SELECT 1 FROM ton_invoices WHERE status='active' AND kind='usdt' LIMIT 1")) {
            anyActive = sqlite3_step(s) == SQLITE_ROW;
            sqlite3_finalize(s);
        }
    }
    // Живых счетов нет — обозреватель не спрашиваем вовсе.
    if (!anyActive) return;

    // Только переводы USD₮ (jetton_master): рассылки чужих жетонов на кошелёк
    // иначе вытеснили бы настоящую оплату из полусотни последних. Адреса — в
    // сыром виде: в нём нет «+» и «/», которые в адресе запроса пришлось бы
    // кодировать. Сверка мастера и получателя всё равно идёт ниже, у нас.
    const std::string url = std::string(TON_API_V3) + "jetton/transfers?owner_address=" + walletRaw +
                            "&jetton_master=" + premium::USDT_MASTER + "&direction=in&limit=50";
    const json j = json::parse(http(url, "", 15), nullptr, false);
    const auto list = j.is_object() ? j.find("jetton_transfers") : j.end();
    if (!j.is_object() || list == j.end() || !list->is_array()) return;

    for (const auto& item : *list) {
        const auto pay = premium::usdtTransfer(item, walletRaw);
        if (!pay) continue;
        const std::string& memo = pay->memo;

        std::string chatId;
        long long need = 0;
        int days = 30;  // месяц; год — только если так записано в счёте
        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (!prepareOrLog(db, &s,
                "SELECT chat_id, nano_amount, days FROM ton_invoices "
                "WHERE memo=? AND status='active' AND kind='usdt'")) continue;
            sqlite3_bind_text(s, 1, memo.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) == SQLITE_ROW) {
                chatId = safeColumnText(s, 0);
                need = sqlite3_column_int64(s, 1);
                if (sqlite3_column_int(s, 2) == 365) days = 365;
            }
            sqlite3_finalize(s);
        }
        // Нет живого счёта с такой памяткой (чужой перевод, уже оплаченный
        // или истёкший счёт) или сумма счёта ниже доллара — не наш случай.
        if (chatId.empty() || need < premium::USDT_MIN_UNITS) continue;

        if (!premium::paidEnough(pay->units, need)) {
            if (firstTime(said, "under:" + pay->hash))
                std::cerr << "[USDT] недоплата по " << memo << ": пришло " << pay->units
                          << ", ждали " << need << std::endl;
            continue;
        }
        if (!g_serviceChatId.empty() && chatId == g_serviceChatId) {
            if (firstTime(said, "service:" + memo))
                std::cerr << "[USDT] счёт " << memo << " выставлен сервисному аккаунту — Премиум ему не выдаётся" << std::endl;
            continue;
        }

        // Занять счёт и продлить срок — одной транзакцией: либо счёт оплачен
        // и подписка выдана, либо не изменилось ничего и следующий опрос
        // попробует снова. Счёта «оплачен, а подписки нет» не бывает.
        ensureUser(chatId);
        bool taken = false;  // счёт успели занять раньше нас — не ошибка
        bool granted = false;
        {
            std::lock_guard<std::mutex> l(dbMutex);
            granted = inTransactionLocked("usdt", [&]() {
                sqlite3_stmt* s;
                if (!prepareOrLog(db, &s,
                    "UPDATE ton_invoices SET status='paid', paid_at=?, tx_hash=? "
                    "WHERE memo=? AND status='active' AND kind='usdt'")) return false;
                sqlite3_bind_int64(s, 1, now);
                sqlite3_bind_text(s, 2, pay->hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, memo.c_str(), -1, SQLITE_TRANSIENT);
                const int rc = sqlite3_step(s);
                sqlite3_finalize(s);
                if (rc != SQLITE_DONE) return false;
                if (sqlite3_changes(db) != 1) { taken = true; return false; }
                bool wasActive = false;
                return extendPremiumLocked(chatId, now, days, wasActive);
            });
        }
        if (taken) continue;
        if (!granted) {
            if (firstTime(said, "fail:" + memo)) {
                std::cerr << "[USDT] ОПЛАЧЕНО, НО ПОДПИСКА НЕ ВЫДАНА: chat=" << chatId
                          << " memo=" << memo << " — счёт остался активным, повторяю" << std::endl;
                alertOwner("пришло " + premium::usdText(pay->units) + " USD₮ по счёту " +
                           memo + " (chat " + chatId + "), а Премиум выдать не вышло. Бот повторяет каждые 20 с; "
                           "если не пройдёт — выдать вручную.");
            }
            continue;
        }
        refreshWatchers();
        trackFunnel(chatId, "paid", "usdt");
        std::cout << "[USDT] premium " << days << "d выдан: chat=" << chatId << " memo=" << memo << " "
                  << premium::usdText(pay->units) << " USDT" << std::endl;
        const Lang lang = langFromCode(getUserLanguage(chatId));
        sendMsg(chatId, withDays(tr(lang, "ton_paid_ok"), days), openAppKeyboard(lang));
    }
}

void trackFunnel(const std::string& chatId, const char* ev, const char* src) {
    // Таблица воронки (заводит бот в initDB, пишет и API — whale_api.py,
    // track): событие раз в сутки на человека, вид и источник. Оплату видит
    // только бот — пишем её здесь.
    const long long t = static_cast<long long>(time(nullptr));
    char day[16];
    const time_t tt = static_cast<time_t>(t);
    struct tm g{};
    gmtime_r(&tt, &g);
    std::strftime(day, sizeof day, "%Y-%m-%d", &g);
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "INSERT OR IGNORE INTO funnel_events(chat_id, ev, src, day, at) VALUES(?,?,?,?,?)")) return;
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, ev, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, src, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, day, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 5, t);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

bool grantPremiumDays(const std::string& chatId, int days) {
    if (chatId.empty() || days <= 0 || days > premium::MAX_GRANT_DAYS) return false;
    if (!g_serviceChatId.empty() && chatId == g_serviceChatId) return false;
    if (!g_premiumSchemaOk) return false;

    ensureUser(chatId);
    const long long now = static_cast<long long>(time(nullptr));
    bool ok = false;
    {
        // Чтение срока и запись нового — одной транзакцией. Без неё API, в ту
        // же секунду прибавивший дни за бонус или приглашение (extend_premium),
        // мог оказаться между чтением и записью — и одно из продлений затиралось.
        std::lock_guard<std::mutex> l(dbMutex);
        ok = inTransactionLocked("grant", [&]() {
            bool wasActive = false;
            return extendPremiumLocked(chatId, now, days, wasActive);
        });
    }
    if (ok) refreshWatchers();
    return ok;
}

namespace {

enum class Applied { Activated, Extended, Duplicate, Rejected, Error };
struct ApplyResult { Applied how; int days; };

// Платёж звёздами: проверить, записать в историю и продлить срок — одной
// транзакцией. Повтор того же платежа (Telegram может прислать его ещё раз)
// узнаётся по telegram_payment_charge_id и ничего не продлевает.
ApplyResult applySuccessfulPayment(const std::string& chatId, const json& sp) {
    if (!sp.is_object()) return {Applied::Rejected, 0};

    std::string payload, currency, chargeId, providerChargeId;
    try {
        payload = sp.value("invoice_payload", "");
        currency = sp.value("currency", "");
        chargeId = sp.value("telegram_payment_charge_id", "");
        providerChargeId = sp.value("provider_payment_charge_id", "");
    } catch (const json::exception& e) {
        std::cerr << "[PREMIUM] successful_payment malformed JSON (chat " << chatId << "): " << e.what() << std::endl;
        return {Applied::Rejected, 0};
    }
    const long long amount = premium::intField(sp, "total_amount");
    // Конец оплаченного периода подписки Telegram; у разовой оплаты поля нет — 0.
    const long long subUntil = std::max(0LL, premium::intField(sp, "subscription_expiration_date"));

    const auto* plan = starPlan(payload);
    if (!plan) {
        std::cerr << "[PREMIUM] successful_payment with unknown payload: " << payload << " (chat " << chatId << ")"
                  << std::endl;
        return {Applied::Rejected, 0};
    }
    if (currency != "XTR" || amount != plan->stars) {
        std::cerr << "[PREMIUM] successful_payment amount/currency mismatch: " << amount << " " << currency
                  << " (chat " << chatId << ")" << std::endl;
        return {Applied::Rejected, 0};
    }
    if (chargeId.empty()) {
        std::cerr << "[PREMIUM] successful_payment missing telegram_payment_charge_id (chat " << chatId
                  << ") — rejected, no history record possible" << std::endl;
        return {Applied::Rejected, 0};
    }
    if (!g_premiumSchemaOk) {
        std::cerr << "[PREMIUM] apply: schema not ready, cannot process payment for " << chatId << std::endl;
        return {Applied::Error, plan->days};
    }

    const long long paidAt = static_cast<long long>(time(nullptr));
    ensureUser(chatId);

    bool duplicate = false;
    bool wasActive = false;
    bool ok = false;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        ok = inTransactionLocked("apply", [&]() {
            sqlite3_stmt* s;
            if (!prepareOrLog(db, &s,
                    "INSERT INTO premium_payments(chat_id, telegram_payment_charge_id, provider_payment_charge_id, "
                    "payload, amount, currency, paid_at, sub_until) VALUES(?,?,?,?,?,?,?,?)"))
                return false;
            sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, chargeId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, providerChargeId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, payload.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 5, amount);
            sqlite3_bind_text(s, 6, currency.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 7, paidAt);
            sqlite3_bind_int64(s, 8, subUntil);
            const int rc = sqlite3_step(s);
            const int ext = sqlite3_extended_errcode(db);
            sqlite3_finalize(s);
            if (rc == SQLITE_CONSTRAINT && ext == SQLITE_CONSTRAINT_UNIQUE) {
                duplicate = true;
                return false;
            }
            if (rc != SQLITE_DONE) {
                std::cerr << "[PREMIUM] apply: payment INSERT failed (" << rc << "): " << sqlite3_errmsg(db) << std::endl;
                return false;
            }
            return extendPremiumLocked(chatId, paidAt, plan->days, wasActive);
        });
    }
    if (duplicate) {
        std::cout << "[PREMIUM] Duplicate payment ignored (charge " << chargeId << ", chat " << chatId << ")" << std::endl;
        return {Applied::Duplicate, plan->days};
    }
    if (!ok) return {Applied::Error, plan->days};
    std::cout << "[PREMIUM] " << (wasActive ? "Extended" : "Activated") << " for " << chatId << " (charge "
              << chargeId << ")" << std::endl;
    return {wasActive ? Applied::Extended : Applied::Activated, plan->days};
}

}

size_t premiumMaxWallets(const std::string& chatId) {
    return isPremium(chatId) ? PREMIUM_MAX_WALLETS : FREE_MAX_WALLETS;
}

void handlePreCheckoutQuery(const json& q) {
    if (!q.is_object() || !q.contains("id") || !q["id"].is_string()) return;

    std::string payload, currency;
    bool parseOk = true;
    try {
        payload = q.value("invoice_payload", "");
        currency = q.value("currency", "");
    } catch (const json::exception& e) {
        std::cerr << "[PREMIUM] pre_checkout malformed JSON: " << e.what() << std::endl;
        parseOk = false;
    }
    const long long amount = premium::intField(q, "total_amount");

    Lang lang = Lang::EN;
    std::string buyerChatId;
    if (q.contains("from") && q["from"].is_object() &&
        q["from"].contains("id") && q["from"]["id"].is_number_integer()) {
        buyerChatId = std::to_string(q["from"]["id"].get<long long>());
        lang = langFromCode(getUserLanguage(buyerChatId));
    }

    json a;
    a["pre_checkout_query_id"] = q["id"].get<std::string>();
    const bool schemaReady = g_premiumSchemaOk.load();
    const premium::Checkout verdict = parseOk
        ? premium::checkStars(payload, currency, amount, buyerChatId, static_cast<long long>(time(nullptr)))
        : premium::Checkout::Unknown;
    if (schemaReady && verdict == premium::Checkout::Ok) {
        a["ok"] = true;
    } else {
        a["ok"] = false;
        a["error_message"] = !schemaReady ? tr(lang, "payments_unavailable")
                           : verdict == premium::Checkout::IntroGone ? tr(lang, "intro_gone")
                           : tr(lang, "invoice_unknown_product");
        std::cerr << "[PREMIUM] pre_checkout rejected: schemaReady=" << schemaReady
                  << " payload=" << payload << " currency=" << currency << " amount=" << amount << std::endl;
    }

    // Telegram ждёт ответа 10 секунд — потом отменяет оплату сам.
    std::string why;
    if (!tgAnswerOk(http(tgApi("answerPreCheckoutQuery"), a.dump(), 8), why))
        std::cerr << "[PREMIUM] answerPreCheckoutQuery failed: " << why << std::endl;
}


bool handleSuccessfulPayment(const std::string& chatId, const json& sp) {
    const ApplyResult r = applySuccessfulPayment(chatId, sp);
    if (r.how == Applied::Rejected) return false;
    if (r.how == Applied::Error) {
        // Звёзды Telegram уже списал — значит, выдать нужно руками и сразу.
        const std::string charge = sp.value("telegram_payment_charge_id", std::string("?"));
        std::cerr << "[PREMIUM] ВЫДАЧА НЕ УДАЛАСЬ ПОСЛЕ ОПЛАТЫ: chat=" << chatId << " charge=" << charge
                  << " amount=" << premium::intField(sp, "total_amount") << " — выдать вручную" << std::endl;
        alertOwner("звёзды списаны (" + std::to_string(premium::intField(sp, "total_amount")) + " XTR, chat " + chatId +
                   ", charge " + charge + "), а Премиум на " + std::to_string(r.days) +
                   " дн. выдать не вышло — выдать вручную.");
        return false;
    }
    if (r.how == Applied::Duplicate) return true;
    trackFunnel(chatId, "paid", "stars");
    refreshWatchers();

    const Lang lang = langFromCode(getUserLanguage(chatId));
    // Ежемесячное списание по подписке Telegram — не новая покупка, а
    // продление: человеку достаточно короткого «продлено на 30 дней». Флаги
    // читаем без исключений: подписка уже выдана, падать здесь нельзя.
    auto flag = [&](const char* k) { auto it = sp.find(k); return it != sp.end() && it->is_boolean() && it->get<bool>(); };
    const bool renewal = flag("is_recurring") && !flag("is_first_recurring");
    const std::string text = renewal
        ? withDays(tr(lang, "payment_renewed"), r.days)
        : tr(lang, "payment_success_title") + "\n\n" + tr(lang, "payment_success_activated") + "\n\n" +
          withDays(tr(lang, "payment_success_duration"), r.days);
    if (!sendMsg(chatId, text, openAppKeyboard(lang)).ok)
        std::cerr << "[PREMIUM] success notification failed for " << chatId << std::endl;
    return true;
}

int cancelStarSubscriptions(const std::string& reportTo, bool sayNone) {
    // Действующая подписка — последний платёж человека с sub_until в будущем.
    struct Sub { std::string chat, charge; };
    std::vector<Sub> subs;
    const long long now = static_cast<long long>(time(nullptr));
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s,
                "SELECT p.chat_id, p.telegram_payment_charge_id FROM premium_payments p "
                "WHERE p.sub_until > ? AND p.paid_at = (SELECT MAX(q.paid_at) FROM premium_payments q "
                "WHERE q.chat_id = p.chat_id AND q.sub_until > 0)")) {
            sqlite3_bind_int64(s, 1, now);
            while (sqlite3_step(s) == SQLITE_ROW)
                subs.push_back({safeColumnText(s, 0), safeColumnText(s, 1)});
            sqlite3_finalize(s);
        }
    }
    if (subs.empty()) {
        if (!reportTo.empty() && sayNone) sendMsg(reportTo, "🔁 Автопродлений звёздами нет — ничего не списывается само.");
        return 0;
    }
    int ok = 0, failed = 0;
    for (const auto& sub : subs) {
        long long userId = 0;
        if (!premium::strictInt(sub.chat, userId)) { ++failed; continue; }  // подписки — только у людей
        const json req = {{"user_id", userId}, {"telegram_payment_charge_id", sub.charge}, {"is_canceled", true}};
        std::string why;
        if (!tgAnswerOk(http(tgApi("editUserStarSubscription"), req.dump(), 15), why)) {
            std::cerr << "[PREMIUM] cancel subscription " << sub.chat << ": " << why << std::endl;
            ++failed;
            continue;
        }
        ++ok;
        // Отменённую больше не считаем действующей: второй проход её не тронет.
        {
            std::lock_guard<std::mutex> l(dbMutex);
            sqlite3_stmt* s;
            if (prepareOrLog(db, &s, "UPDATE premium_payments SET sub_until=0 WHERE chat_id=?")) {
                sqlite3_bind_text(s, 1, sub.chat.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
        }
        const Lang lang = langFromCode(getUserLanguage(sub.chat));
        sendMsg(sub.chat, tr(lang, "autorenew_off"), openAppKeyboard(lang, "premium-renew", "btn_extend"));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    std::cout << "[PREMIUM] автопродления отменены: " << ok << ", не вышло: " << failed << std::endl;
    if (!reportTo.empty())
        sendMsg(reportTo, "🔁 <b>Автопродления звёздами отменены</b>: " + std::to_string(ok) +
                          (failed ? "\n⚠️ Не вышло: " + std::to_string(failed) + " — повтор: /autorenew" : "") +
                          "\nКаждому пришло сообщение: Премиум действует до конца оплаченного срока, "
                          "дальше — только если сам продлит.");
    return ok;
}
