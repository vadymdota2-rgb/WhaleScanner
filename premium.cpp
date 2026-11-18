// © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
// Премиум: выдача подписки после оплаты. Спецификация — docs/premium.md,
// правила — premium_logic.h, договор с остальным ботом — premium.h.
//
// Порядок в каждом месте, где появляются деньги: проверить вход чистыми
// правилами → записать оплату и продлить срок одной транзакцией → только
// потом сообщить человеку. Сбой между шагами не оставляет полуготового
// состояния: либо всё, либо ничего, и тогда — сообщение владельцу.

#include "premium.h"
#include "premium_logic.h"
#include "lifecycle.h"
#include "safe_http.h"
#include "ru.h"
#include "telegram.h"
#include "utils.h"

#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;

namespace {

using premium::logSafe;
using premium::STAR_PLANS;
using premium::starPlan;
using premium::withDays;

constexpr size_t FREE_MAX_WALLETS = 3;
constexpr size_t PREMIUM_MAX_WALLETS = 50;

// Задаются один раз при запуске (initPremium), до потоков бота.
std::string g_serviceChatId;
std::string g_ownerChatId;
// Схема готова — оплата включена. До initPremium и после неудачного — нет.
std::atomic<bool> g_schemaOk{false};

long long systemNow() { return static_cast<long long>(std::time(nullptr)); }
std::atomic<premium::Clock> g_clock{&systemNow};
long long nowSec() { return g_clock.load()(); }

void systemPause(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
std::atomic<premium::Pause> g_pause{&systemPause};
void pause(int ms) { g_pause.load()(ms); }

bool isService(const std::string& chatId) { return !g_serviceChatId.empty() && chatId == g_serviceChatId; }

// Пределы ответов чужих серверов (safe_http.h): 50 переводов toncenter —
// ~60 КБ, ответ Bot API — сотни байт.
constexpr size_t TONCENTER_MAX_BYTES = 1U << 20U;
constexpr size_t TELEGRAM_MAX_BYTES = 64U << 10U;

// Строка извне — в сообщение владельцу (HTML Bot API): одной строкой и с
// экранированными < > &, иначе Telegram отклонит всё сообщение.
std::string forOwner(const std::string& s, size_t max) { return escapeHtml(logSafe(s, max)); }

// Деньги пришли, а Премиум не выдан — владельцу сразу, а не только в журнал:
// журнал никто не читает, а человек ждёт.
void alertOwner(const std::string& text) {
    if (!g_ownerChatId.empty()) sendMsg(g_ownerChatId, "⚠️ <b>Оплата</b>: " + text);
}

// ── SQLite: ресурсы закрываются сами ─────────────────────────────────────────

/* Подготовленный запрос. Всё, что открыто, закрывается в деструкторе — ранний
   выход из функции больше не может забыть finalize. Не вышло подготовить —
   ok() ложно, а bind и step безопасно ничего не делают (step — ошибка). */
class Stmt {
public:
    explicit Stmt(const char* sql) : ok_(prepareOrLog(db, &s_, sql)) {}
    ~Stmt() {
        if (s_ != nullptr) sqlite3_finalize(s_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&&) = delete;
    Stmt& operator=(Stmt&&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    Stmt& bind(int i, long long v) {
        if (ok_) sqlite3_bind_int64(s_, i, v);
        return *this;
    }
    Stmt& bind(int i, const std::string& v) {
        if (ok_) sqlite3_bind_text(s_, i, v.c_str(), -1, SQLITE_TRANSIENT);
        return *this;
    }
    // SQLITE_ROW, SQLITE_DONE или код ошибки.
    [[nodiscard]] int step() { return ok_ ? sqlite3_step(s_) : SQLITE_MISUSE; }
    [[nodiscard]] bool row() { return step() == SQLITE_ROW; }
    // Выполнить запрос без строк; true — без ошибки.
    [[nodiscard]] bool run() { return step() == SQLITE_DONE; }
    [[nodiscard]] long long int64(int col) const { return sqlite3_column_int64(s_, col); }
    [[nodiscard]] bool null(int col) const { return sqlite3_column_type(s_, col) == SQLITE_NULL; }
    [[nodiscard]] std::string text(int col) const { return safeColumnText(s_, col); }

private:
    sqlite3_stmt* s_ = nullptr;  // объявлен до ok_: заполняется в его инициализаторе
    bool ok_;
};

/* Транзакция записи. BEGIN IMMEDIATE — блокировка записи сразу: API пишет в
   ту же базу из своего процесса, и пока он держит запись, мы ждём
   (busy_timeout соединения, initDB), а не падаем посреди. Без commit() —
   ROLLBACK в деструкторе: что бы ни случилось внутри, полуготового
   состояния в базе не остаётся. Создавать под dbMutex. */
class Tx {
public:
    explicit Tx(const char* what)
        : what_(what), open_(sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK) {
        if (!open_) std::cerr << "[PREMIUM] " << what_ << ": BEGIN failed: " << logSafe(sqlite3_errmsg(db)) << std::endl;
    }
    ~Tx() {
        if (open_) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;
    Tx(Tx&&) = delete;
    Tx& operator=(Tx&&) = delete;

    [[nodiscard]] bool ok() const { return open_; }
    // Звать только после ok(): без открытой транзакции COMMIT и так ошибка.
    [[nodiscard]] bool commit() {
        if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) {
            open_ = false;
            return true;
        }
        std::cerr << "[PREMIUM] " << what_ << ": COMMIT failed: " << logSafe(sqlite3_errmsg(db)) << std::endl;
        return false;  // ROLLBACK — в деструкторе
    }

private:
    const char* what_;
    bool open_;
};

// ── чтение и продление срока (под dbMutex) ───────────────────────────────────

struct PremiumRow {
    int flag = 0;
    long long start = 0;
    long long expire = 0;
};

// Строка человека; нет строки — nullopt.
std::optional<PremiumRow> readPremiumRowLocked(const std::string& chatId) {
    Stmt s("SELECT is_premium, premium_start, premium_expire FROM users WHERE chat_id=?");
    s.bind(1, chatId);
    if (!s.row()) return std::nullopt;
    return PremiumRow{s.int64(0) != 0 ? 1 : 0, s.int64(1), s.int64(2)};
}

// Продлить срок на days дней. Звать внутри Tx. Нет строки человека или срок
// не продлить (premium::extendTerm) — nullopt, ничего не записано.
std::optional<premium::Term> extendPremiumLocked(const std::string& chatId, long long now, int days) {
    const auto row = readPremiumRowLocked(chatId);
    if (!row) return std::nullopt;
    const auto term = premium::extendTerm(row->flag, row->start, row->expire, now, days);
    if (!term) {
        std::cerr << "[PREMIUM] extend: срок не продлить (chat " << logSafe(chatId) << ", expire=" << row->expire
                  << ", days=" << days << ")" << std::endl;
        return std::nullopt;
    }
    Stmt s("UPDATE users SET is_premium=1, premium_start=?, premium_expire=? WHERE chat_id=?");
    s.bind(1, term->start).bind(2, term->expire).bind(3, chatId);
    // Строка человека прочитана выше в той же транзакции — удачный UPDATE
    // меняет ровно её.
    if (!s.run()) return std::nullopt;
    return term;
}

// ALTER TABLE … ADD COLUMN: «столбец уже есть» — обычное дело на старой
// базе; любая другая ошибка — оплата выключается.
bool addColumnLocked(const char* sql) {
    const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    const std::string e = sqlite3_errmsg(db);
    if (ok) {
        std::cout << "[PREMIUM] migration: " << sql << std::endl;
        return true;
    }
    if (e.find("duplicate column") != std::string::npos) return true;
    std::cerr << "[PREMIUM][FATAL] migration failed: " << logSafe(e) << " (" << sql << ")" << std::endl;
    return false;
}

// Ответ Bot API: ok:true — да; иначе причина для журнала — описание
// Telegram или сбой сети, который httpsCapped уже положил в why. Без
// исключений на пустом и битом ответе.
bool tgAnswerOk(const std::string& resp, std::string& why) {
    const json r = json::parse(resp, nullptr, false);
    if (!r.is_object()) {
        why = why.empty() ? "нет ответа" : logSafe(why);
        return false;
    }
    const auto ok = r.find("ok");
    if (ok != r.end() && ok->is_boolean() && ok->get<bool>()) return true;
    const std::string d = premium::strField(r, "description");
    why = d.empty() ? "без описания" : logSafe(d);
    return false;
}

}  // namespace

void premium::setClock(premium::Clock clock) { g_clock = clock != nullptr ? clock : &systemNow; }
void premium::setPause(premium::Pause p) { g_pause = p != nullptr ? p : &systemPause; }

// ── схема ────────────────────────────────────────────────────────────────────

bool initPremium(const std::string& serviceChatId, const std::string& ownerChatId) {
    g_serviceChatId = serviceChatId;
    g_ownerChatId = ownerChatId;

    const std::lock_guard<std::mutex> l(dbMutex);

    // Таблицу users заводит бот (initDB) — здесь только столбцы Премиума.
    // Без неё оплату включать нельзя: выдать подписку было бы некуда.
    bool ok = true;
    for (const char* sql : {"ALTER TABLE users ADD COLUMN is_premium INTEGER NOT NULL DEFAULT 0",
                            "ALTER TABLE users ADD COLUMN premium_start INTEGER DEFAULT 0",
                            "ALTER TABLE users ADD COLUMN premium_expire INTEGER DEFAULT 0"})
        ok = ok && addColumnLocked(sql);

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
    if (ok && sqlite3_exec(db, tablesSql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[PREMIUM][FATAL] payment tables failed: " << logSafe(sqlite3_errmsg(db)) << std::endl;
        ok = false;
    }
    // Столбцы, которых не было у таблиц из старых версий: подписка Telegram
    // (до какого времени оплачен период — по нему cancelStarSubscriptions
    // находит, что отменять), вид счёта и его срок (старые счета — месяц).
    for (const char* sql : {"ALTER TABLE premium_payments ADD COLUMN sub_until INTEGER NOT NULL DEFAULT 0",
                            "ALTER TABLE ton_invoices ADD COLUMN kind TEXT NOT NULL DEFAULT 'ton'",
                            "ALTER TABLE ton_invoices ADD COLUMN days INTEGER NOT NULL DEFAULT 30"})
        ok = ok && addColumnLocked(sql);

    // Индексы под каждый запрос модуля — на миллионе людей и миллионах
    // счетов ни один запрос не читает таблицу целиком (проверяет
    // tests/premium_policy.py, ScaleTest). Частичные — только нужные строки:
    // подписчики, живые счета USD₮, оплатившие хеши, подписки звёздами.
    const char* indexSql = R"(
        CREATE INDEX IF NOT EXISTS idx_users_premium ON users(premium_expire) WHERE is_premium=1;
        CREATE INDEX IF NOT EXISTS idx_ton_hash ON ton_invoices(tx_hash) WHERE tx_hash!='';
        CREATE INDEX IF NOT EXISTS idx_premium_sub ON premium_payments(sub_until) WHERE sub_until>0;
    )";
    if (ok && sqlite3_exec(db, indexSql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[PREMIUM][FATAL] payment indexes failed: " << logSafe(sqlite3_errmsg(db)) << std::endl;
        ok = false;
    }

    // Сервисному аккаунту Премиума не бывает: старую отметку в базе снимаем,
    // чтобы он не считался подписчиком ни в /stats, ни в API.
    if (ok && !g_serviceChatId.empty()) {
        Stmt s("UPDATE users SET is_premium=0, premium_expire=0 WHERE chat_id=?");
        s.bind(1, g_serviceChatId);
        ok = s.run();
    }

    g_schemaOk = ok;
    if (ok)
        std::cout << "[PREMIUM] Module initialized (plans: " << STAR_PLANS[0].stars << " Stars / "
                  << STAR_PLANS[0].days << " days, " << STAR_PLANS[1].stars << " / " << STAR_PLANS[1].days << ")"
                  << std::endl;
    else
        std::cerr << "[PREMIUM][FATAL] Module init failed — payments are DISABLED until fixed" << std::endl;
    return ok;
}

// ── кто подписчик ────────────────────────────────────────────────────────────

std::set<std::string> premiumSubsetOf(const std::vector<std::string>& chatIds) {
    std::set<std::string> out;
    if (chatIds.empty() || !g_schemaOk) return out;

    const long long now = nowSec();
    // Кусками: у SQLite предел числа параметров в запросе (в старых сборках —
    // 999), и алерт большому кругу людей иначе ушёл бы всем как бесплатным.
    constexpr size_t CHUNK = 500;

    for (size_t from = 0; from < chatIds.size(); from += CHUNK) {
        // Замок — на кусок, а не на весь список: рассылка на миллион человек
        // не держит базу бота секунду, остальные потоки идут между кусками.
        const std::lock_guard<std::mutex> l(dbMutex);
        const size_t n = std::min(CHUNK, chatIds.size() - from);
        std::string sql = "SELECT chat_id FROM users WHERE is_premium=1 AND premium_expire>? AND chat_id IN (";
        for (size_t i = 0; i < n; i++) sql += i != 0 ? ",?" : "?";
        sql += ")";
        Stmt s(sql.c_str());
        if (!s.ok()) return {};
        s.bind(1, now);
        for (size_t i = 0; i < n; i++) s.bind(static_cast<int>(i + 2), chatIds[from + i]);
        while (s.row()) out.insert(s.text(0));
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
    if (isService(chatId)) return false;
    const long long now = nowSec();
    const std::lock_guard<std::mutex> l(dbMutex);
    const auto row = readPremiumRowLocked(chatId);
    return row && row->flag != 0 && row->expire > now;
}

size_t premiumMaxWallets(const std::string& chatId) {
    return isPremium(chatId) ? PREMIUM_MAX_WALLETS : FREE_MAX_WALLETS;
}

void cleanupExpiredPremium() {
    // Проход может длиться долго (письма — с паузами); второй, пока идёт
    // первый, ничего не делает — двух писем одному человеку не будет.
    // Отметка «идёт» снимается при любом выходе (и при исключении из
    // письма): иначе снятие истёкших остановилось бы навсегда.
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return;
    const auto release = [](std::atomic<bool>* flag) { *flag = false; };
    const std::unique_ptr<std::atomic<bool>, decltype(release)> done(&busy, release);

    // Пачками: транзакция записи короткая (API пишет в ту же базу и ждёт
    // её), память — на пачку, а не на всех истёкших сразу.
    constexpr int BATCH = 1000;
    // Письмо — не чаще 4 в секунду: лимит Telegram (~30 в секунду) — на
    // весь бот, львиная доля — алертам. 1000 писем — около 4 минут.
    constexpr int MAIL_GAP_MS = 250;
    for (;;) {
        const long long now = nowSec();
        std::vector<std::string> expired;
        {
            // Снятие отметки и список снятых — один запрос (RETURNING) в
            // транзакции: продление от API в ту же секунду не даст письма
            // «Премиум закончился» тому, у кого он есть. Ошибку SQLite
            // (триггер, ограничение) step возвращает до первой строки.
            const std::lock_guard<std::mutex> l(dbMutex);
            Tx tx("cleanup");
            if (!tx.ok()) return;
            Stmt upd(
                "UPDATE users SET is_premium=0 WHERE chat_id IN (SELECT chat_id FROM users "
                "WHERE is_premium=1 AND premium_expire<=? LIMIT ?) RETURNING chat_id");
            upd.bind(1, now).bind(2, BATCH);
            int rc = upd.step();
            for (; rc == SQLITE_ROW; rc = upd.step()) expired.push_back(upd.text(0));
            if (rc != SQLITE_DONE || expired.empty() || !tx.commit()) return;
        }
        std::cout << "[PREMIUM] Expired -> Free: " << expired.size() << " user(s)" << std::endl;
        refreshWatchers();
        // Письмо об окончании: после пробной недели — с итогом и вводной
        // ценой, после оплаченного срока — что остаётся бесплатно.
        for (const std::string& cid : expired) {
            if (cid.empty() || isService(cid)) continue;
            sendPremiumEnded(cid);
            pause(MAIL_GAP_MS);
        }
        if (expired.size() < static_cast<size_t>(BATCH)) return;
    }
}

// ── выдача дней ──────────────────────────────────────────────────────────────

namespace {

// Прибавить дни; новый конец срока или nullopt — не выдано.
std::optional<long long> grantDays(const std::string& chatId, int days) {
    if (chatId.empty() || days <= 0 || days > premium::MAX_GRANT_DAYS) return std::nullopt;
    if (isService(chatId) || !g_schemaOk) return std::nullopt;

    ensureUser(chatId);
    const long long now = nowSec();
    std::optional<premium::Term> term;
    {
        // Чтение срока и запись нового — одной транзакцией. Без неё API, в ту
        // же секунду прибавивший дни за бонус или приглашение (extend_premium),
        // мог оказаться между чтением и записью — и одно из продлений затиралось.
        const std::lock_guard<std::mutex> l(dbMutex);
        Tx tx("grant");
        if (tx.ok()) term = extendPremiumLocked(chatId, now, days);
        if (term && !tx.commit()) term.reset();
    }
    if (!term) return std::nullopt;
    refreshWatchers();
    return term->expire;
}

}  // namespace

bool grantPremiumDays(const std::string& chatId, int days) { return grantDays(chatId, days).has_value(); }

namespace {

void trackFunnel(const std::string& chatId, const char* ev, const char* src) {
    // Таблица воронки (заводит бот в initDB, пишет и API — whale_api.py,
    // track): событие раз в сутки на человека, вид и источник. Оплату видит
    // только бот — пишем её здесь.
    const long long t = nowSec();
    const std::lock_guard<std::mutex> l(dbMutex);
    Stmt s("INSERT OR IGNORE INTO funnel_events(chat_id, ev, src, day, at) VALUES(?,?,?,?,?)");
    s.bind(1, chatId).bind(2, std::string(ev)).bind(3, std::string(src)).bind(4, premium::utcDate(t)).bind(5, t);
    if (!s.run()) std::cerr << "[PREMIUM] funnel: " << logSafe(sqlite3_errmsg(db)) << std::endl;
}

}  // namespace

std::string grantCommand(const std::string& args) {
    const auto g = premium::parseGrant(args);
    if (!g) return "Использование: /grant &lt;chat_id&gt; &lt;дней 1–3650&gt;";
    const auto until = grantDays(g->chat, g->days);
    if (!until)
        return "⚠️ Не выдано: " + g->chat + " — служебный аккаунт, оплата выключена или сбой базы (журнал [PREMIUM]).";
    std::cout << "[PREMIUM] выдано вручную: chat=" << g->chat << " +" << g->days << "d" << std::endl;
    return "✅ Премиум +" + std::to_string(g->days) + " дн.: <code>" + g->chat + "</code>, до " +
           premium::utcDateTime(*until);
}


// ── USD₮ ─────────────────────────────────────────────────────────────────────

namespace {

// Переводы жетонов отдаёт только третья версия обозревателя: во второй
// пришлось бы разбирать тело сообщения вручную.
constexpr const char* TON_API_V3 = "https://toncenter.com/api/v3/";

// Кошелёк приёма — тот же, что показывает API (whale_api.py, TON_WALLET):
// одна переменная окружения и одно значение по умолчанию у обоих.
std::string tonWallet() {
    const char* a = std::getenv("TON_WALLET_ADDRESS");  // NOLINT(concurrency-mt-unsafe): окружение не меняется
    if (a != nullptr && *a != '\0') return a;
    return "UQDAiNYvy2KUIwjEcgD1ZxPVw-CPwdk4WbBQwpVsQQ5jsO6o";
}

// Опоздавшую оплату принимаем сутки: API показывает счёт час (PAY_TTL), но
// человек, заплативший позже, не должен терять деньги.
constexpr long long USDT_INVOICE_LIVE_SEC = premium::DAY_SEC;

struct Invoice {
    std::string chatId;
    long long need = 0;
    int days = 30;  // месяц; год — только если так записано в счёте
    long long created = 0;
};

}  // namespace

void pollUsdtPayments() {
    // Счёт в USD₮ выставляет API, платит человек из своего кошелька с памяткой
    // счёта. Здесь: найти перевод с памяткой живого счёта, занять счёт и
    // продлить срок одной транзакцией, потом сообщить человеку. Зовётся из
    // главного цикла бота, всегда из одного потока.
    if (!g_schemaOk) return;
    static premium::Once said;
    static long long lastPoll = 0;  // когда спрашивали обозреватель
    const std::string wallet = tonWallet();
    const std::string walletRaw = premium::tonRawAddress(wallet);
    if (walletRaw.empty()) {
        if (said.first("wallet"))
            std::cerr << "[USDT] адрес кошелька приёма не разобран — оплату USD₮ не проверяю" << std::endl;
        return;
    }

    // Живой счёт — активный и моложе суток. Счёт старше гасим, но живость
    // проверяем и сами: не погасился (сбой базы) — всё равно не засчитываем.
    const long long now = nowSec();
    const long long cutoff = now - USDT_INVOICE_LIVE_SEC;
    long long newest = 0;  // самый молодой активный счёт
    {
        // Сначала только чтение: в WAL оно не ждёт пишущего. Блокировку
        // записи (а с ней — ожидание, пока API допишет своё, под общим
        // замком бота) берём, только когда есть что гасить.
        const std::lock_guard<std::mutex> l(dbMutex);
        long long oldest = 0;
        {
            Stmt live("SELECT MIN(created_at), MAX(created_at) FROM ton_invoices WHERE status='active' AND kind='usdt'");
            if (!live.row() || live.null(0)) return;
            oldest = live.int64(0);
            newest = live.int64(1);
        }
        if (oldest <= cutoff) {
            Stmt expire("UPDATE ton_invoices SET status='expired' WHERE status='active' AND kind='usdt' AND created_at <= ?");
            expire.bind(1, cutoff);
            if (!expire.run()) std::cerr << "[USDT] истечение счетов: " << logSafe(sqlite3_errmsg(db)) << std::endl;
        }
    }
    // Живых счетов нет — обозреватель не спрашиваем вовсе; есть только
    // старые — реже (premium::pollDue).
    if (newest <= cutoff || !premium::pollDue(now, newest, lastPoll)) return;
    lastPoll = now;

    // Только переводы USD₮ (jetton_master): рассылки чужих жетонов на кошелёк
    // иначе вытеснили бы настоящую оплату из полусотни последних. Адреса — в
    // сыром виде: в нём нет «+» и «/», которые в адресе запроса пришлось бы
    // кодировать. Сверка мастера и получателя всё равно идёт ниже, у нас.
    const std::string url = std::string(TON_API_V3) + "jetton/transfers?owner_address=" + walletRaw +
                            "&jetton_master=" + premium::USDT_MASTER + "&direction=in&limit=50";
    std::string err;
    const json j = json::parse(httpsCapped(url, "", 15, TONCENTER_MAX_BYTES, &err), nullptr, false);
    if (!err.empty() && said.first("net:" + err)) std::cerr << "[USDT] toncenter: " << logSafe(err) << std::endl;
    const auto list = j.is_object() ? j.find("jetton_transfers") : j.end();
    if (!j.is_object() || list == j.end() || !list->is_array()) return;

    // Переводы, похожие на оплату, и живые счета с их памятками — одним
    // запросом на весь список, а не запросом на каждый перевод.
    std::vector<premium::UsdtTransfer> pays;
    json memos = json::array();
    for (const auto& item : *list) {
        if (auto pay = premium::usdtTransfer(item, walletRaw)) {
            memos.push_back(pay->memo);
            pays.push_back(std::move(*pay));
        }
    }
    // Поиск — по памятке (первичный ключ): «+» перед status и created_at
    // не даёт SQLite взять вместо него индекс по статусу — тот прошёл бы
    // все живые счета, а не полсотни нужных.
    std::map<std::string, Invoice> invoices;
    {
        const std::lock_guard<std::mutex> l(dbMutex);
        Stmt s(
            "SELECT memo, chat_id, nano_amount, days, created_at FROM ton_invoices "
            "WHERE +status='active' AND kind='usdt' AND +created_at > ? AND memo IN (SELECT value FROM json_each(?))");
        s.bind(1, cutoff).bind(2, memos.dump(-1, ' ', false, json::error_handler_t::replace));
        while (s.row()) invoices[s.text(0)] = Invoice{s.text(1), s.int64(2), s.int64(3) == 365 ? 365 : 30, s.int64(4)};
    }

    for (const auto& pay : pays) {
        const std::string& memo = pay.memo;

        // Нет живого счёта с такой памяткой (чужой перевод, уже оплаченный
        // или истёкший счёт) или сумма счёта ниже доллара — не наш случай.
        const auto found = invoices.find(memo);
        if (found == invoices.end()) continue;
        const Invoice inv = found->second;
        if (inv.chatId.empty() || inv.need < premium::USDT_MIN_UNITS) continue;

        if (!premium::transferFitsInvoice(pay.at, inv.created)) {
            if (said.first("old:" + pay.hash))
                std::cerr << "[USDT] перевод " << logSafe(pay.hash, 64) << " сделан раньше счёта " << logSafe(memo)
                          << " — не засчитываю" << std::endl;
            continue;
        }
        if (!premium::paidEnough(pay.units, inv.need)) {
            if (said.first("under:" + pay.hash))
                std::cerr << "[USDT] недоплата по " << logSafe(memo) << ": пришло " << pay.units << ", ждали "
                          << inv.need << std::endl;
            continue;
        }
        if (isService(inv.chatId)) {
            if (said.first("service:" + memo))
                std::cerr << "[USDT] счёт " << logSafe(memo) << " выставлен сервисному аккаунту — Премиум ему не выдаётся"
                          << std::endl;
            continue;
        }

        // Занять счёт и продлить срок — одной транзакцией: либо счёт оплачен
        // и подписка выдана, либо не изменилось ничего и следующий опрос
        // попробует снова. Счёта «оплачен, а подписки нет» не бывает.
        ensureUser(inv.chatId);
        bool taken = false;  // счёт успели занять раньше нас — не ошибка
        bool reused = false;  // этот перевод уже оплатил другой счёт
        bool granted = false;
        {
            const std::lock_guard<std::mutex> l(dbMutex);
            Tx tx("usdt");
            Stmt used("SELECT 1 FROM ton_invoices WHERE tx_hash=? AND tx_hash!='' AND status='paid' LIMIT 1");
            used.bind(1, pay.hash);
            reused = used.row();
            Stmt claim(
                "UPDATE ton_invoices SET status='paid', paid_at=?, tx_hash=? "
                "WHERE memo=? AND status='active' AND kind='usdt'");
            claim.bind(1, now).bind(2, pay.hash).bind(3, memo);
            if (tx.ok() && !reused && claim.run()) {
                taken = sqlite3_changes(db) != 1;
                granted = !taken && extendPremiumLocked(inv.chatId, now, inv.days) && tx.commit();
            }
        }
        // Счёт закрыт — второй перевод с той же памяткой его уже не ищет.
        if (taken || granted) invoices.erase(found);
        if (taken) continue;
        if (reused) {
            if (said.first("reused:" + pay.hash))
                std::cerr << "[USDT] перевод " << logSafe(pay.hash, 64) << " уже оплатил другой счёт — " << logSafe(memo)
                          << " не засчитываю" << std::endl;
            continue;
        }
        if (!granted) {
            if (said.first("fail:" + memo)) {
                std::cerr << "[USDT] ОПЛАЧЕНО, НО ПОДПИСКА НЕ ВЫДАНА: chat=" << logSafe(inv.chatId)
                          << " memo=" << logSafe(memo) << " — счёт остался активным, повторяю" << std::endl;
                alertOwner("пришло " + premium::usdText(pay.units) + " USD₮ по счёту " + forOwner(memo, 64) + " (chat " +
                           forOwner(inv.chatId, 32) +
                           "), а Премиум выдать не вышло. Бот повторяет каждые 20 с; если не пройдёт — выдать вручную.");
            }
            continue;
        }
        refreshWatchers();
        trackFunnel(inv.chatId, "paid", "usdt");
        std::cout << "[USDT] premium " << inv.days << "d выдан: chat=" << logSafe(inv.chatId)
                  << " memo=" << logSafe(memo) << " " << premium::usdText(pay.units) << " USDT" << std::endl;
        const Lang lang = langFromCode(getUserLanguage(inv.chatId));
        sendMsg(inv.chatId, withDays(tr(lang, "ton_paid_ok"), inv.days), openAppKeyboard(lang));
    }
}

// ── звёзды ───────────────────────────────────────────────────────────────────

namespace {

enum class Applied : std::uint8_t { Activated, Extended, Duplicate, Rejected, Error };
struct ApplyResult {
    Applied how;
    int days;
};

// Поле есть, но не строка — ответ Telegram испорчен, такой платёж не берём.
bool badString(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && !it->is_string();
}

// Платёж звёздами: проверить, записать в историю и продлить срок — одной
// транзакцией. Повтор того же платежа (Telegram может прислать его ещё раз)
// узнаётся по telegram_payment_charge_id и ничего не продлевает.
ApplyResult applySuccessfulPayment(const std::string& chatId, const json& sp) {
    if (!sp.is_object()) return {Applied::Rejected, 0};
    if (badString(sp, "invoice_payload") || badString(sp, "currency") ||
        badString(sp, "telegram_payment_charge_id") || badString(sp, "provider_payment_charge_id")) {
        std::cerr << "[PREMIUM] successful_payment malformed (chat " << logSafe(chatId) << ")" << std::endl;
        return {Applied::Rejected, 0};
    }
    const std::string payload = premium::strField(sp, "invoice_payload");
    const std::string currency = premium::strField(sp, "currency");
    const std::string chargeId = premium::strField(sp, "telegram_payment_charge_id");
    const std::string providerChargeId = premium::strField(sp, "provider_payment_charge_id");
    const long long amount = premium::intField(sp, "total_amount");
    // Конец оплаченного периода подписки Telegram; у разовой оплаты поля нет — 0.
    const long long subUntil = std::max(0LL, premium::intField(sp, "subscription_expiration_date"));

    const auto* plan = starPlan(payload);
    if (plan == nullptr) {
        std::cerr << "[PREMIUM] successful_payment with unknown payload: " << logSafe(payload) << " (chat "
                  << logSafe(chatId) << ")" << std::endl;
        return {Applied::Rejected, 0};
    }
    if (currency != "XTR" || amount != plan->stars) {
        std::cerr << "[PREMIUM] successful_payment amount/currency mismatch: " << amount << " " << logSafe(currency)
                  << " (chat " << logSafe(chatId) << ")" << std::endl;
        return {Applied::Rejected, 0};
    }
    if (chargeId.empty()) {
        std::cerr << "[PREMIUM] successful_payment missing telegram_payment_charge_id (chat " << logSafe(chatId)
                  << ") — rejected, no history record possible" << std::endl;
        return {Applied::Rejected, 0};
    }
    if (!g_schemaOk) {
        std::cerr << "[PREMIUM] apply: schema not ready, cannot process payment for " << logSafe(chatId) << std::endl;
        return {Applied::Error, plan->days};
    }

    const long long paidAt = nowSec();
    ensureUser(chatId);

    bool duplicate = false;
    bool wasActive = false;
    bool ok = false;
    {
        const std::lock_guard<std::mutex> l(dbMutex);
        Tx tx("apply");
        Stmt ins(
            "INSERT INTO premium_payments(chat_id, telegram_payment_charge_id, provider_payment_charge_id, "
            "payload, amount, currency, paid_at, sub_until) VALUES(?,?,?,?,?,?,?,?)");
        ins.bind(1, chatId).bind(2, chargeId).bind(3, providerChargeId).bind(4, payload);
        ins.bind(5, amount).bind(6, currency).bind(7, paidAt).bind(8, subUntil);
        if (tx.ok()) {
            const int rc = ins.step();
            duplicate = rc == SQLITE_CONSTRAINT && sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_UNIQUE;
            if (rc != SQLITE_DONE && !duplicate)
                std::cerr << "[PREMIUM] apply: payment INSERT failed (" << rc << "): " << logSafe(sqlite3_errmsg(db))
                          << std::endl;
            const auto term = rc == SQLITE_DONE ? extendPremiumLocked(chatId, paidAt, plan->days) : std::nullopt;
            wasActive = term && term->wasActive;
            ok = term && tx.commit();
        }
    }
    if (duplicate) {
        std::cout << "[PREMIUM] Duplicate payment ignored (charge " << logSafe(chargeId) << ", chat " << logSafe(chatId)
                  << ")" << std::endl;
        return {Applied::Duplicate, plan->days};
    }
    if (!ok) return {Applied::Error, plan->days};
    std::cout << "[PREMIUM] " << (wasActive ? "Extended" : "Activated") << " for " << logSafe(chatId) << " (charge "
              << logSafe(chargeId) << ")" << std::endl;
    return {wasActive ? Applied::Extended : Applied::Activated, plan->days};
}

}  // namespace

void handlePreCheckoutQuery(const json& preCheckoutQuery) {
    const json& q = preCheckoutQuery;
    const std::string id = premium::strField(q, "id");
    if (id.empty()) return;  // отвечать нечем

    std::string buyerChatId;
    Lang lang = Lang::EN;
    if (const auto from = q.find("from"); from != q.end() && from->is_object()) {
        const auto uid = from->find("id");
        if (uid != from->end() && uid->is_number_integer()) {
            buyerChatId = std::to_string(uid->get<long long>());
            lang = langFromCode(getUserLanguage(buyerChatId));
        }
    }
    const std::string payload = premium::strField(q, "invoice_payload");
    const std::string currency = premium::strField(q, "currency");
    const long long amount = premium::intField(q, "total_amount");
    const bool parseOk = !badString(q, "invoice_payload") && !badString(q, "currency");

    json a;
    a["pre_checkout_query_id"] = id;
    const bool schemaReady = g_schemaOk.load();
    const premium::Checkout verdict =
        parseOk ? premium::checkStars(payload, currency, amount, buyerChatId, nowSec()) : premium::Checkout::Unknown;
    if (schemaReady && verdict == premium::Checkout::Ok) {
        a["ok"] = true;
    } else {
        a["ok"] = false;
        const char* why = "invoice_unknown_product";
        if (!schemaReady) why = "payments_unavailable";
        else if (verdict == premium::Checkout::IntroGone) why = "intro_gone";
        a["error_message"] = tr(lang, why);
        std::cerr << "[PREMIUM] pre_checkout rejected: schemaReady=" << schemaReady << " payload=" << logSafe(payload)
                  << " currency=" << logSafe(currency) << " amount=" << amount << std::endl;
    }
    // Telegram ждёт ответа 10 секунд — потом отменяет оплату сам.
    std::string why;
    if (!tgAnswerOk(httpsCapped(tgApi("answerPreCheckoutQuery"), a.dump(), 8, TELEGRAM_MAX_BYTES, &why), why))
        std::cerr << "[PREMIUM] answerPreCheckoutQuery failed: " << why << std::endl;
}

bool handleSuccessfulPayment(const std::string& chatId, const json& successfulPayment) {
    const json& sp = successfulPayment;
    const ApplyResult r = applySuccessfulPayment(chatId, sp);
    if (r.how == Applied::Rejected) return false;
    if (r.how == Applied::Error) {
        // Звёзды Telegram уже списал — значит, выдать нужно руками и сразу.
        const std::string charge = forOwner(premium::strField(sp, "telegram_payment_charge_id"), 80);
        const long long amount = premium::intField(sp, "total_amount");
        std::cerr << "[PREMIUM] ВЫДАЧА НЕ УДАЛАСЬ ПОСЛЕ ОПЛАТЫ: chat=" << logSafe(chatId) << " charge=" << charge
                  << " amount=" << amount << " — выдать вручную" << std::endl;
        alertOwner("звёзды списаны (" + std::to_string(amount) + " XTR, chat " + forOwner(chatId, 32) + ", charge " +
                   charge + "), а Премиум на " + std::to_string(r.days) + " дн. выдать не вышло — выдать вручную.");
        return false;
    }
    if (r.how == Applied::Duplicate) return true;
    trackFunnel(chatId, "paid", "stars");
    refreshWatchers();

    const Lang lang = langFromCode(getUserLanguage(chatId));
    // Ежемесячное списание по подписке Telegram — не новая покупка, а
    // продление: человеку достаточно короткого «продлено на 30 дней». Флаги
    // читаем без исключений: подписка уже выдана, падать здесь нельзя.
    const auto flag = [&](const char* k) {
        const auto it = sp.find(k);
        return it != sp.end() && it->is_boolean() && it->get<bool>();
    };
    const bool renewal = flag("is_recurring") && !flag("is_first_recurring");
    const std::string text = renewal ? withDays(tr(lang, "payment_renewed"), r.days)
                                     : tr(lang, "payment_success_title") + "\n\n" + tr(lang, "payment_success_activated") +
                                           "\n\n" + withDays(tr(lang, "payment_success_duration"), r.days);
    if (!sendMsg(chatId, text, openAppKeyboard(lang)).ok)
        std::cerr << "[PREMIUM] success notification failed for " << logSafe(chatId) << std::endl;
    return true;
}

int cancelStarSubscriptions(const std::string& reportTo, bool sayNone) {
    // Действующая подписка — последний платёж человека с sub_until в будущем.
    struct Sub {
        std::string chat;
        std::string charge;
    };
    std::vector<Sub> subs;
    {
        const std::lock_guard<std::mutex> l(dbMutex);
        Stmt s(
            "SELECT p.chat_id, p.telegram_payment_charge_id FROM premium_payments p "
            "WHERE p.sub_until > ? AND p.sub_until>0 AND p.paid_at = (SELECT MAX(q.paid_at) FROM premium_payments q "
            "WHERE q.chat_id = p.chat_id AND q.sub_until > 0)");
        s.bind(1, nowSec());
        while (s.row()) subs.push_back({s.text(0), s.text(1)});
    }
    if (subs.empty()) {
        if (!reportTo.empty() && sayNone) sendMsg(reportTo, "🔁 Автопродлений звёздами нет — ничего не списывается само.");
        return 0;
    }
    int ok = 0;
    int failed = 0;
    for (const auto& sub : subs) {
        long long userId = 0;
        if (!premium::strictInt(sub.chat, userId)) {  // подписки — только у людей
            ++failed;
            continue;
        }
        const json req = {{"user_id", userId}, {"telegram_payment_charge_id", sub.charge}, {"is_canceled", true}};
        std::string why;
        if (!tgAnswerOk(httpsCapped(tgApi("editUserStarSubscription"), req.dump(), 15, TELEGRAM_MAX_BYTES, &why), why)) {
            std::cerr << "[PREMIUM] cancel subscription " << logSafe(sub.chat) << ": " << why << std::endl;
            ++failed;
            continue;
        }
        ++ok;
        // Отменённую больше не считаем действующей: второй проход её не тронет.
        {
            const std::lock_guard<std::mutex> l(dbMutex);
            Stmt s("UPDATE premium_payments SET sub_until=0 WHERE chat_id=?");
            s.bind(1, sub.chat);
            if (!s.run()) std::cerr << "[PREMIUM] cancel: " << logSafe(sqlite3_errmsg(db)) << std::endl;
        }
        const Lang lang = langFromCode(getUserLanguage(sub.chat));
        sendMsg(sub.chat, tr(lang, "autorenew_off"), openAppKeyboard(lang, "premium-renew", "btn_extend"));
        pause(60);
    }
    std::cout << "[PREMIUM] автопродления отменены: " << ok << ", не вышло: " << failed << std::endl;
    if (!reportTo.empty())
        sendMsg(reportTo, "🔁 <b>Автопродления звёздами отменены</b>: " + std::to_string(ok) +
                              (failed != 0 ? "\n⚠️ Не вышло: " + std::to_string(failed) + " — повтор: /autorenew" : "") +
                              "\nКаждому пришло сообщение: Премиум действует до конца оплаченного срока, "
                              "дальше — только если сам продлит.");
    return ok;
}
