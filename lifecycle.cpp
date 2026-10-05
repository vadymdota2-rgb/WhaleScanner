#include "lifecycle.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "premium.h"
#include "ru.h"
#include "telegram.h"
#include "utils.h"
#include "hyperliquid_internal.h"

extern sqlite3* db;
extern std::mutex dbMutex;
extern const std::string SERVICE_CHAT_ID;

namespace {

constexpr long long DAY = 86400;
// Сколько писем за один проход: рассылка идёт прямо из главного цикла, и
// тысяча писем подряд задержала бы чтение блоков.
constexpr int MAX_PER_TICK = 60;

void schema() {
    std::lock_guard<std::mutex> l(dbMutex);
    char* err = nullptr;
    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS lifecycle_sent ("
            " chat_id TEXT NOT NULL, kind TEXT NOT NULL, at INTEGER NOT NULL,"
            " PRIMARY KEY (chat_id, kind));",
            nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[LIFECYCLE] schema failed: " << (err ? err : "") << std::endl;
        sqlite3_free(err);
    }
}

// Занять отметку «письмо отправлено». false — уже было, писать не нужно.
bool claim(const std::string& chat, const std::string& kind) {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, "INSERT OR IGNORE INTO lifecycle_sent(chat_id, kind, at) VALUES(?,?,?)")) return false;
    sqlite3_bind_text(s, 1, chat.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, static_cast<long long>(time(nullptr)));
    const bool ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(s);
    return ok;
}

/* «21 дней» → «21 день»: приветствие пишет и 14, и 21 день (по
   приглашению), а словарь знает одну форму. Склоняем там, где число с
   существительным согласуется по-разному (русский и украинский). */
std::string dayForm(Lang lang, std::string text, long long n) {
    const char* many = lang == Lang::RU ? "дней" : lang == Lang::UK ? "днів" : nullptr;
    if (!many) return text;
    const long long d10 = n % 10, d100 = n % 100;
    const char* w = d10 == 1 && d100 != 11 ? "день"
                  : d10 >= 2 && d10 <= 4 && (d100 < 12 || d100 > 14) ? (lang == Lang::RU ? "дня" : "дні")
                  : many;
    const std::string from = std::to_string(n) + " " + many;
    const size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), std::to_string(n) + " " + w);
    return text;
}

std::string fill(std::string text, long long n) {
    const size_t at = text.find("{n}");
    if (at != std::string::npos) text.replace(at, 3, std::to_string(n));
    return text;
}

// Ни разу не платил: ни звёздами, ни USD₮.
bool neverPaidLocked(const std::string& chat) {
    sqlite3_stmt* s;
    bool paid = false;
    if (prepareOrLog(db, &s, "SELECT 1 FROM premium_payments WHERE chat_id=? LIMIT 1")) {
        sqlite3_bind_text(s, 1, chat.c_str(), -1, SQLITE_TRANSIENT);
        paid = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
    }
    if (!paid && prepareOrLog(db, &s, "SELECT 1 FROM ton_invoices WHERE chat_id=? AND status='paid' LIMIT 1")) {
        sqlite3_bind_text(s, 1, chat.c_str(), -1, SQLITE_TRANSIENT);
        paid = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
    }
    return !paid;
}

long long trialGrantedLocked(const std::string& chat) {
    sqlite3_stmt* s;
    long long at = 0;
    if (prepareOrLog(db, &s, "SELECT granted_at FROM trial_granted WHERE chat_id=?")) {
        sqlite3_bind_text(s, 1, chat.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) at = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return at;
}

struct Watch {
    std::vector<std::string> all;     // кошельки человека в порядке списка
    long long threshold = 0;
};

Watch walletsOfLocked(const std::string& chat) {
    Watch w;
    sqlite3_stmt* s;
    if (prepareOrLog(db, &s,
            "SELECT lower(wa.address) FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id "
            "WHERE uw.user_id=? ORDER BY uw.is_primary DESC, uw.created_at ASC, uw.rowid ASC")) {
        sqlite3_bind_text(s, 1, chat.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(s) == SQLITE_ROW) w.all.push_back(safeColumnText(s, 0));
        sqlite3_finalize(s);
    }
    if (prepareOrLog(db, &s, "SELECT threshold_nanos FROM users WHERE chat_id=?")) {
        sqlite3_bind_text(s, 1, chat.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) w.threshold = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return w;
}

/* Сделки кошельков человека с `since` не меньше его порога — то, о чём
   пришёл бы алерт. `missedOnly` — только то, что бесплатный тариф не
   доставил: спот кошельков сверх первых трёх и весь Hyperliquid. */
long long activity(const std::string& chat, long long since, bool missedOnly) {
    Watch w;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        w = walletsOfLocked(chat);
    }
    if (w.all.empty()) return 0;
    long long n = 0;
    std::vector<std::string> spot;
    for (size_t i = missedOnly ? FREE_ALERT_WALLETS : 0; i < w.all.size(); i++) spot.push_back(w.all[i]);
    auto marks = [](size_t k) {
        std::string m;
        for (size_t i = 0; i < k; i++) m += i ? ",?" : "?";
        return m;
    };
    if (!spot.empty()) {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        const std::string sql = "SELECT COUNT(*) FROM trades WHERE timestamp>=? AND usd_nanos>=? AND wallet IN (" +
                                marks(spot.size()) + ")";
        if (prepareOrLog(db, &s, sql.c_str())) {
            sqlite3_bind_int64(s, 1, since);
            sqlite3_bind_int64(s, 2, w.threshold);
            for (size_t i = 0; i < spot.size(); i++)
                sqlite3_bind_text(s, static_cast<int>(i + 3), spot[i].c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) == SQLITE_ROW) n += sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
    }
    {
        std::lock_guard<std::mutex> l(hl::g_hlDbMutex);
        if (hl::g_hlDb) {
            sqlite3_stmt* s;
            const std::string sql = "SELECT COUNT(*) FROM hl_fills WHERE ts>=? AND notional_nanos>=? AND lower(wallet) IN (" +
                                    marks(w.all.size()) + ")";
            if (prepareOrLog(hl::g_hlDb, &s, sql.c_str())) {
                sqlite3_bind_int64(s, 1, since * 1000);
                sqlite3_bind_int64(s, 2, w.threshold);
                for (size_t i = 0; i < w.all.size(); i++)
                    sqlite3_bind_text(s, static_cast<int>(i + 3), w.all[i].c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(s) == SQLITE_ROW) n += sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
        }
    }
    return n;
}

// Письмо о премиуме открывает приложение сразу на экране Премиума: `go` —
// повод (заголовок экрана и замер воронки), `btn` — подпись кнопки.
void send(const std::string& chat, const std::string& text, const char* go, const char* btn) {
    const Lang lang = langFromCode(getUserLanguage(chat));
    sendMsg(chat, text, openAppKeyboard(lang, go, btn));
}

struct Row {
    std::string chat;
    long long expire = 0;
};

std::vector<Row> select(const char* sql, long long a, long long b) {
    std::vector<Row> out;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s, sql)) return out;
    sqlite3_bind_int64(s, 1, a);
    sqlite3_bind_int64(s, 2, b);
    while (sqlite3_step(s) == SQLITE_ROW && out.size() < 1000)
        out.push_back({safeColumnText(s, 0), sqlite3_column_int64(s, 1)});
    sqlite3_finalize(s);
    return out;
}

/* Новый выпуск дайджеста — тем, кто попросил присылать (digest_subs; кнопка
   в приложении). Выпуск собирает API раз в сутки; бот видит новую строку в
   digests и рассылает отдельным потоком, чтобы не держать чтение блоков.
   Номер последнего разосланного выпуска — в state: после перезапуска старый
   выпуск второй раз не уйдёт, а при первом запуске рассылки нет вовсе. */
void digestTick() {
    long long last = -1, newest = 0;
    std::vector<std::string> subs;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s, "SELECT MAX(id) FROM digests")) {
            if (sqlite3_step(s) == SQLITE_ROW) newest = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
        if (prepareOrLog(db, &s, "SELECT value FROM state WHERE key='digest_notified'")) {
            if (sqlite3_step(s) == SQLITE_ROW) last = std::atoll(safeColumnText(s, 0).c_str());
            sqlite3_finalize(s);
        }
        if (newest <= 0 || newest == last) return;
        if (prepareOrLog(db, &s, "INSERT OR REPLACE INTO state(key, value) VALUES('digest_notified', ?)")) {
            const std::string v = std::to_string(newest);
            sqlite3_bind_text(s, 1, v.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        if (last < 0) return;
        if (prepareOrLog(db, &s, "SELECT chat_id FROM digest_subs")) {
            while (sqlite3_step(s) == SQLITE_ROW) subs.push_back(safeColumnText(s, 0));
            sqlite3_finalize(s);
        }
    }
    if (subs.empty()) return;
    std::cout << "[DIGEST] выпуск " << newest << " — уведомляю " << subs.size() << std::endl;
    std::thread([subs]() {
        for (const auto& chat : subs) {
            const Lang lang = langFromCode(getUserLanguage(chat));
            sendMsg(chat, tr(lang, "dg_ready"), openAppKeyboard(lang, "digest", "dg_open_btn"));
            // Telegram пропускает около 30 сообщений в секунду.
            std::this_thread::sleep_for(std::chrono::milliseconds(45));
        }
    }).detach();
}

}  // namespace

void initLifecycle() { schema(); }

void sendPremiumEnded(const std::string& chat) {
    if (chat.empty() || chat == SERVICE_CHAT_ID) return;
    long long granted = 0;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        granted = trialGrantedLocked(chat);
        fresh = granted > 0 && neverPaidLocked(chat);
    }
    const Lang lang = langFromCode(getUserLanguage(chat));
    std::string text;
    if (fresh) {
        // Конец пробной недели: итог, что остаётся, и вводная цена на 48 часов
        // (её же предлагает экран премиума — intro_until в API).
        text = tr(lang, "lc_trial_end");
        const long long n = activity(chat, granted, false);
        if (n > 0) text += "\n\n" + fill(tr(lang, "lc_stats"), n);
        text += "\n\n" + tr(lang, "lc_after") + "\n\n" + tr(lang, "lc_intro");
        sendMsg(chat, text, openAppKeyboard(lang, "premium-intro", "btn_intro"));
        return;
    }
    text = tr(lang, "lc_prem_end") + "\n\n" + tr(lang, "lc_after");
    sendMsg(chat, text, openAppKeyboard(lang, "premium-ended", "btn_plans"));
}

/* Приветствие: пробный Премиум выдаёт API при первом открытии приложения
   (trial_granted). Только недавним — за последние 3 часа: после обновления
   бота старые пользователи приветствия не получат. Число дней — по сроку
   Премиума: пришедшему по приглашению подарено больше. */
void welcomeTick() {
    const long long now = static_cast<long long>(time(nullptr));
    int sent = 0;
    for (const Row& r : select(
            "SELECT t.chat_id, u.premium_expire FROM trial_granted t JOIN users u ON u.chat_id=t.chat_id "
            "WHERE t.granted_at BETWEEN ? AND ? "
            "AND NOT EXISTS (SELECT 1 FROM lifecycle_sent l WHERE l.chat_id=t.chat_id AND l.kind='welcome')",
            now - 3 * 3600, now)) {
        if (sent >= MAX_PER_TICK) return;
        if (r.chat == SERVICE_CHAT_ID || r.expire <= now || !claim(r.chat, "welcome")) continue;
        const Lang lang = langFromCode(getUserLanguage(r.chat));
        const long long days = (r.expire - now + DAY / 2) / DAY;
        const long long n = days < 1 ? 1 : days;
        sendMsg(r.chat, dayForm(lang, fill(tr(lang, "lc_welcome"), n), n), openAppKeyboard(lang));
        sent++;
    }
}

void lifecycleTick() {
    digestTick();
    const long long now = static_cast<long long>(time(nullptr));
    int sent = 0;

    // 1. Пробная неделя кончается через ~2 дня.
    for (const Row& r : select(
            "SELECT u.chat_id, u.premium_expire FROM users u JOIN trial_granted t ON t.chat_id=u.chat_id "
            "WHERE u.is_premium=1 AND u.premium_expire BETWEEN ? AND ? "
            "AND NOT EXISTS (SELECT 1 FROM premium_payments p WHERE p.chat_id=u.chat_id) "
            "AND NOT EXISTS (SELECT 1 FROM ton_invoices i WHERE i.chat_id=u.chat_id AND i.status='paid')",
            now + 36 * 3600, now + 60 * 3600)) {
        if (sent >= MAX_PER_TICK) return;
        if (r.chat == SERVICE_CHAT_ID || !claim(r.chat, "trial_d5")) continue;
        long long granted = 0;
        {
            std::lock_guard<std::mutex> l(dbMutex);
            granted = trialGrantedLocked(r.chat);
        }
        const Lang lang = langFromCode(getUserLanguage(r.chat));
        std::string text = tr(lang, "lc_trial_d5");
        const long long n = activity(r.chat, granted, false);
        if (n > 0) text += "\n\n" + fill(tr(lang, "lc_stats"), n);
        text += "\n\n" + tr(lang, "lc_after") + "\n\n" + tr(lang, "lc_keep");
        send(r.chat, text, "premium-trial", "btn_keep");
        sent++;
    }

    // 2. Оплаченный срок кончается через 3 дня, а автопродления нет: разовая
    //    оплата (год, USD₮ или месяц до подписок). У подписки Telegram
    //    sub_until последнего платежа покрывает срок — ей не пишем.
    for (const Row& r : select(
            "SELECT u.chat_id, u.premium_expire FROM users u "
            "WHERE u.is_premium=1 AND u.premium_expire BETWEEN ? AND ? "
            "AND (EXISTS (SELECT 1 FROM premium_payments p WHERE p.chat_id=u.chat_id) "
            "  OR EXISTS (SELECT 1 FROM ton_invoices i WHERE i.chat_id=u.chat_id AND i.status='paid')) "
            "AND NOT EXISTS (SELECT 1 FROM premium_payments p WHERE p.chat_id=u.chat_id "
            "  AND p.sub_until >= u.premium_expire - 86400)",
            now, now + 3 * DAY)) {
        if (sent >= MAX_PER_TICK) return;
        if (r.chat == SERVICE_CHAT_ID || !claim(r.chat, "renew:" + std::to_string(r.expire))) continue;
        const Lang lang = langFromCode(getUserLanguage(r.chat));
        const long long days = (r.expire - now + DAY - 1) / DAY;
        send(r.chat, fill(tr(lang, "lc_renew"), days < 1 ? 1 : days), "premium-renew", "btn_extend");
        sent++;
    }

    // 3. Возврат: 14 и 30 дней без премиума. Только если кошельки за это
    //    время сделали то, о чём человек не узнал, — пустое письмо не шлём.
    for (const int after : {14, 30}) {
        for (const Row& r : select(
                "SELECT chat_id, premium_expire FROM users WHERE is_premium=0 AND premium_expire BETWEEN ? AND ?",
                now - (after + 1) * DAY, now - after * DAY)) {
            if (sent >= MAX_PER_TICK) return;
            if (r.chat == SERVICE_CHAT_ID) continue;
            const long long n = activity(r.chat, r.expire, true);
            if (n <= 0 || !claim(r.chat, "wb" + std::to_string(after) + ":" + std::to_string(r.expire))) continue;
            const Lang lang = langFromCode(getUserLanguage(r.chat));
            send(r.chat, fill(tr(lang, "lc_missed"), n) + "\n\n" + tr(lang, "lc_back"), "premium-back", "btn_plans");
            sent++;
        }
    }
}
