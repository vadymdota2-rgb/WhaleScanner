// Оплата Премиума без сети и без настоящего Telegram (tests/run.sh).
//
// Правила (premium_logic.h): тарифы, вводная цена, срок, адреса TON, приём
// перевода USD₮ — примерами и тысячами случайных входов. Потом premium.cpp
// целиком на базе SQLite в памяти: обозреватель и Telegram подменены, их
// ответы и сообщения человеку записываются.
//
// Каждое правило отвечает на вопрос «выдать ли подписку»: ошибка в одну
// сторону — человек заплатил и ничего не получил, в другую — получил даром.
#include <array>
#include <cctype>
#include <climits>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sqlite3.h>
#include "../premium.h"
#include "../premium_logic.h"
#include "../telegram.h"
#include "../lifecycle.h"
#include "../ru.h"

using nlohmann::json;
using namespace premium;

static int g_fail = 0;
// Мутационная проверка (tools/mutate.py) гоняет тесты сотни раз: там тяжёлые
// циклы короче в 10 раз (PREMIUM_QUICK=1) — проверки те же, повторов меньше.
static const int QUICK = std::getenv("PREMIUM_QUICK") != nullptr ? 10 : 1;  // NOLINT(concurrency-mt-unsafe)
#define CHECK(cond) do { if (!(cond)) { ++g_fail; std::cerr << __FILE__ << ":" << __LINE__ << ": " #cond << std::endl; } } while (0)

// ── подмены того, что в боте живёт в main.cpp и lifecycle.cpp ────────────────
sqlite3* db = nullptr;
std::mutex dbMutex;
static std::string g_explorer;                  // что ответит toncenter
static std::vector<std::string> g_urls;         // куда ходил бот
static std::vector<json> g_tgPosts;             // что ушло в Bot API
static std::string g_tgAnswer = R"({"ok":true,"result":true})";
static std::vector<std::pair<std::string, std::string>> g_sent;  // сообщения людям
static std::vector<std::string> g_ended;
static int g_refreshed = 0;
static std::set<std::string> g_ghosts;          // ensureUser «не смог» завести строку

static std::vector<size_t> g_caps;             // предел ответа каждого запроса
static std::string g_netError;                  // «сбой сети» для следующих запросов
std::string httpsCapped(const std::string& url, const std::string& post, int, size_t maxBytes, std::string* error) {
    g_urls.push_back(url);
    g_caps.push_back(maxBytes);
    if (!g_netError.empty()) {
        if (error != nullptr) *error = g_netError;
        return "";
    }
    if (url.find("toncenter.com") != std::string::npos) return g_explorer;
    g_tgPosts.push_back(json::parse(post, nullptr, false));
    return g_tgAnswer;
}
std::string tgApi(const std::string& method) { return "https://tg.test/bot/" + method; }
SendResult sendMsg(const std::string& chatId, const std::string& text, const std::string&) {
    g_sent.emplace_back(chatId, text);
    return {chatId != "mute", false, 0};  // «mute» — Telegram не доставил
}
std::string openAppKeyboard(Lang, const std::string&, const char*) { return ""; }
std::string getUserLanguage(const std::string&) { return "ru"; }
void ensureUser(const std::string& chatId, const std::string&) {
    if (g_ghosts.count(chatId)) return;
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO users(chat_id, created_at) VALUES(?, 1)", -1, &s, nullptr);
    sqlite3_bind_text(s, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);
}
void refreshWatchers() { ++g_refreshed; }
void sendPremiumEnded(const std::string& chatId) { g_ended.push_back(chatId); }

// ── помощники ────────────────────────────────────────────────────────────────
static const std::string WALLET = "UQDAiNYvy2KUIwjEcgD1ZxPVw-CPwdk4WbBQwpVsQQ5jsO6o";
static const std::string WALLET_RAW = "0:C088D62FCB62942308C47200F56713D5C3E08FC1D93859B050C2956C410E63B0";
static const std::string OTHER_RAW = "0:7784B24A1684BBD9F141D055902C46F08890352B89D0566B822175D548B0058E";
static const std::string FAKE_MASTER = "0:78532B87EC99F6227A0010E9CF59F5AAE07E083F229347DFEA6FE38C0599278B";
static long long now() { return static_cast<long long>(time(nullptr)); }

static void exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "SQL: " << (err ? err : "?") << " in " << sql << std::endl;
        ++g_fail;
    }
    sqlite3_free(err);
}
static long long num(const std::string& sql) {
    sqlite3_stmt* s;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr);
    long long v = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : LLONG_MIN;
    sqlite3_finalize(s);
    return v;
}
static std::string text(const std::string& sql) {
    sqlite3_stmt* s;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr);
    std::string v = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_text(s, 0)
        ? reinterpret_cast<const char*>(sqlite3_column_text(s, 0)) : "";
    sqlite3_finalize(s);
    return v;
}
static long long expireOf(const std::string& chat) {
    return num("SELECT premium_expire FROM users WHERE chat_id='" + chat + "'");
}
// Каждый запрос модуля закрыт (Stmt закрывает сам): незакрытый держал бы
// базу и память. Проверяется перед каждой новой базой и в конце.
static void closeDb() {
    if (db == nullptr) return;
    CHECK(sqlite3_next_stmt(db, nullptr) == nullptr);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = nullptr;
}

static void freshDb() {
    closeDb();
    sqlite3_open(":memory:", &db);
    exec("CREATE TABLE users(chat_id TEXT PRIMARY KEY, language TEXT NOT NULL DEFAULT 'en', "
         "threshold_nanos INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL)");
    exec("CREATE TABLE funnel_events(chat_id TEXT NOT NULL, ev TEXT NOT NULL, src TEXT NOT NULL DEFAULT '', "
         "day TEXT NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (chat_id, ev, src, day))");
    CHECK(initPremium("777", "owner"));
    g_urls.clear(); g_tgPosts.clear(); g_sent.clear(); g_ended.clear(); g_refreshed = 0;
    g_tgAnswer = R"({"ok":true,"result":true})";
}
// Время перевода по умолчанию — заведомо после любого счёта в проверках,
// и через годы тоже (проверки не должны зависеть от сегодняшней даты).
static json transfer(const std::string& memo, long long units, const std::string& hash = "h1",
                     long long at = 4'000'000'000'000) {
    return {{"jetton_master", USDT_MASTER}, {"destination", WALLET_RAW}, {"amount", std::to_string(units)},
            {"transaction_aborted", false}, {"transaction_hash", hash}, {"transaction_now", at},
            {"decoded_forward_payload", {{"@type", "text_comment"}, {"comment", memo}}}};
}
static void invoice(const std::string& memo, const std::string& chat, long long units, long long made, int days = 30) {
    exec("INSERT INTO ton_invoices(memo, chat_id, nano_amount, status, created_at, kind, days) VALUES('" + memo +
         "','" + chat + "'," + std::to_string(units) + ",'active'," + std::to_string(made) + ",'usdt'," +
         std::to_string(days) + ")");
}
static json successful(const std::string& payload, long long amount, const std::string& charge) {
    return {{"currency", "XTR"}, {"total_amount", amount}, {"invoice_payload", payload},
            {"telegram_payment_charge_id", charge}, {"provider_payment_charge_id", ""}};
}
static json preCheckout(const std::string& payload, json amount, long long from, const std::string& cur = "XTR") {
    return {{"id", "q1"}, {"from", {{"id", from}}}, {"currency", cur}, {"total_amount", amount},
            {"invoice_payload", payload}};
}
static bool lastAnswerOk() { return !g_tgPosts.empty() && g_tgPosts.back().value("ok", false); }

// Человеческий вид адреса TON — для проверки разбора в обратную сторону.
static std::string friendlyOf(const std::array<uint8_t, 34>& body, bool urlSafe) {
    std::array<uint8_t, 36> b{};
    for (size_t i = 0; i < 34; ++i) b[i] = body[i];
    uint16_t crc = 0;
    for (size_t i = 0; i < 34; ++i) {
        crc ^= static_cast<uint16_t>(b[i] << 8);
        for (int k = 0; k < 8; ++k) crc = static_cast<uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    b[34] = static_cast<uint8_t>(crc >> 8);
    b[35] = static_cast<uint8_t>(crc);
    const std::string abc = std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") + (urlSafe ? "-_" : "+/");
    std::string out;
    for (size_t i = 0; i < 36; i += 3) {
        const uint32_t v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        for (int k = 18; k >= 0; k -= 6) out += abc[(v >> k) & 63];
    }
    return out;
}

// ── правила ──────────────────────────────────────────────────────────────────
static void rules() {
    // Тарифы: начало нагрузки определяет тариф, хвост после «:» — нет.
    CHECK(starPlan("premium_30_days")->stars == 250 && starPlan("premium_30_days")->days == 30);
    CHECK(starPlan("premium_365_days")->stars == 1990 && starPlan("premium_365_days")->days == 365);
    CHECK(starPlan("premium_30_intro:1:2")->stars == 150 && starPlan("premium_30_intro:1:2")->days == 30);
    for (const char* bad : {"", ":", "premium", "premium_30_days_", "PREMIUM_30_DAYS", " premium_30_days", "premium_30"})
        CHECK(starPlan(bad) == nullptr);
    CHECK(withDays("на {n} дней", 365) == "на 365 дней");
    CHECK(withDays("без числа", 30) == "без числа");

    long long v = 0;
    CHECK(strictInt("123", v) && v == 123);
    CHECK(strictInt("-5", v, true) && v == -5);
    for (const char* bad : {"", "-", "-5", "+5", " 5", "5 ", "12a", "1e3", "0x10", "1234567890123456789"})
        CHECK(!strictInt(bad, v));
    CHECK(strictInt("999999999999999999", v) && v == 999999999999999999LL);

    // Вводная цена: только своему покупателю и в окно.
    const long long t = 1'800'000'000;
    const std::string intro = "premium_30_intro:42:" + std::to_string(t);
    CHECK(checkStars(intro, "XTR", 150, "42", t) == Checkout::Ok);
    CHECK(checkStars(intro, "XTR", 150, "42", t + INTRO_GRACE_SEC) == Checkout::Ok);
    CHECK(checkStars(intro, "XTR", 150, "42", t + INTRO_GRACE_SEC + 1) == Checkout::IntroGone);
    CHECK(checkStars(intro, "XTR", 150, "43", t) == Checkout::IntroGone);
    CHECK(checkStars(intro, "XTR", 150, "", t) == Checkout::IntroGone);
    for (const std::string& bad : {std::string("premium_30_intro"), std::string("premium_30_intro:42"),
                                   std::string("premium_30_intro::") + std::to_string(t),
                                   std::string("premium_30_intro:42:") + std::to_string(t) + "x",
                                   std::string("premium_30_intro:42: ") + std::to_string(t),
                                   std::string("premium_30_intro:4 2:") + std::to_string(t),
                                   std::string("premium_30_intro:42:0"), std::string("premium_30_intro:42:-5")})
        CHECK(checkStars(bad, "XTR", 150, "42", t) == Checkout::IntroGone);
    CHECK(checkStars(intro, "XTR", 150, "42", t + 900) == Checkout::Ok);
    CHECK(checkStars(intro, "XTR", 150, "42", t + 901) == Checkout::IntroGone);
    {
        std::string c;
        long long u = 0;
        CHECK(introBinding("premium_30_intro:42:1", c, u) && c == "42" && u == 1);
        for (const char* bad : {"premium_30_intro", "premium_30_intro:42", "premium_30_intro:x:5", "premium_30_intro:42:0",
                                "premium_30_intro:42:", "premium_30_intro::5"})
            CHECK(!introBinding(bad, c, u));
    }
    CHECK(checkStars("premium_30_intro:-1001:" + std::to_string(t), "XTR", 150, "-1001", t) == Checkout::Ok);
    // Цена и валюта — ровно тарифа; вводная за полную — тоже нет.
    CHECK(checkStars("premium_30_days", "XTR", 250, "1", t) == Checkout::Ok);
    CHECK(checkStars("premium_30_days:x", "XTR", 250, "1", t) == Checkout::Ok);
    for (long long a : {0LL, 249LL, 251LL, 150LL, 1990LL, -250LL, 250LL + (1LL << 32)})
        CHECK(checkStars("premium_30_days", "XTR", a, "1", t) == Checkout::Unknown);
    CHECK(checkStars("premium_30_days", "USD", 250, "1", t) == Checkout::Unknown);
    CHECK(checkStars("premium_30_days", "xtr", 250, "1", t) == Checkout::Unknown);
    CHECK(checkStars(intro, "XTR", 250, "42", t) == Checkout::Unknown);
    CHECK(checkStars("premium_365_days", "XTR", 1990, "1", t) == Checkout::Ok);

    // Целые поля Telegram: дробь, строка и огромное — не цена.
    json j = {{"a", 250}, {"b", 250.9}, {"c", "250"}, {"d", 4294967546LL}, {"e", 18446744073709551615ULL}, {"f", -1}};
    CHECK(intField(j, "a") == 250);
    CHECK(intField(j, "b") == -1 && intField(j, "c") == -1 && intField(j, "x") == -1);
    CHECK(intField(j, "d") == 4294967546LL);
    CHECK(intField(j, "e") == -1);
    CHECK(intField(j, "f") == -1);
    json big = {{"max", 9223372036854775807ULL}, {"over", 9223372036854775808ULL}, {"neg", -5}};
    CHECK(intField(big, "max") == LLONG_MAX && intField(big, "over") == -1 && intField(big, "neg") == -5);
    CHECK(intField(json::array(), "a") == -1);

    // Срок: продление с конца действующего, кончившийся — с сейчас.
    const long long n = 1'800'000'000;
    auto a = extendTerm(1, n - 100, n + 1000, n, 30);
    CHECK(a && a->start == n - 100 && a->expire == n + 1000 + 30 * 86400 && a->wasActive);
    auto b = extendTerm(1, n - 100, n, n, 30);  // кончился ровно сейчас
    CHECK(b && b->start == n && b->expire == n + 30 * 86400 && !b->wasActive);
    auto c = extendTerm(0, n - 100, n + 1000, n, 30);  // флаг снят — срок не в счёт
    CHECK(c && c->start == n && c->expire == n + 30 * 86400 && !c->wasActive);
    auto d = extendTerm(1, 0, n + 5, n, 365);  // без начала — начало сейчас
    CHECK(d && d->start == n && d->expire == n + 5 + 365 * 86400LL);
    CHECK(extendTerm(1, 1, LLONG_MAX - 10, n, 1) == std::nullopt);
    CHECK(extendTerm(1, 1, LLONG_MAX - 86400, n, 1).has_value());
    for (int bad : {0, -1, MAX_GRANT_DAYS + 1, INT_MAX, INT_MIN}) CHECK(extendTerm(0, 0, 0, n, bad) == std::nullopt);
    CHECK(extendTerm(0, 0, 0, n, MAX_GRANT_DAYS).has_value());
    CHECK(MAX_GRANT_DAYS == 3650 && extendTerm(0, 0, 0, n, 3651) == std::nullopt);
    CHECK(extendTerm(0, 0, 0, 1, 1).has_value());
    auto e1 = extendTerm(1, 1, n + 5, n, 1);
    CHECK(e1 && e1->start == 1);
    CHECK(extendTerm(0, 0, 0, 0, 30) == std::nullopt);

    // Хватает ли денег: ровно 98 % — да, на единицу меньше — нет.
    CHECK(paidEnough(4990000, 4990000) && paidEnough(4890200, 4990000) && !paidEnough(4890199, 4990000));
    CHECK(!paidEnough(LLONG_MAX, 0) && !paidEnough(LLONG_MAX, -5) && paidEnough(LLONG_MAX, LLONG_MAX));
    CHECK(paidEnough(1, 1) && !paidEnough(0, 1));
    CHECK(usdText(3990000) == "3.99" && usdText(4100000) == "4.10" && usdText(4090000) == "4.09" &&
          usdText(4099999) == "4.09" && usdText(101000000) == "101.00" && usdText(0) == "0.00" && usdText(-5) == "0.00" &&
          usdText(-50000) == "0.00" && usdText(-1990000) == "0.00");
    std::mt19937_64 r(0x5eed);
    for (int i = 0; i < 100000 / QUICK; ++i) {
        const long long need = static_cast<long long>(r() % 100'000'000'000ULL) + 1;
        const long long got = static_cast<long long>(r() % 200'000'000'000ULL);
        CHECK(paidEnough(got, need) == (static_cast<__int128>(got) * 100 >= static_cast<__int128>(need) * 98));
    }

    CHECK(memoKey(" wt-ab cd\n7 ") == "WT-ABCD7");
    CHECK(memoKey("WT-ÄB") == "WT-ÄB");  // не латиница — как есть, просто не совпадёт
    CHECK(memoKey(std::string(65, 'A')).empty() && memoKey(std::string(64, 'a')) == std::string(64, 'A'));
    CHECK(memoKey("").empty() && memoKey(" \t\r\n").empty());
    CHECK(memoKey("az{`@[") == "AZ{`@[");

    // Адреса: человеческий вид → сырой, с проверкой контрольной суммы.
    CHECK(tonRawAddress(WALLET) == WALLET_RAW);
    CHECK(tonRawAddress("EQCxE6mUtQJKFnGfaROTKOt1lZbDiiX1kCixRv7Nw2Id_sDs") == USDT_MASTER);
    CHECK(tonRawAddress("EQCxE6mUtQJKFnGfaROTKOt1lZbDiiX1kCixRv7Nw2Id/sDs") == USDT_MASTER);  // base64 без url
    CHECK(tonRawAddress(WALLET_RAW) == WALLET_RAW);
    std::string low = WALLET_RAW;
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    CHECK(tonRawAddress(low) == WALLET_RAW);
    CHECK(tonRawAddress("-1:" + std::string(64, 'a')) == "-1:" + std::string(64, 'A'));
    CHECK(tonRawAddress("-128:" + std::string(64, 'A')) == "-128:" + std::string(64, 'A'));
    CHECK(tonRawAddress("127:" + std::string(64, 'A')) == "127:" + std::string(64, 'A'));
    CHECK(tonRawAddress("-129:" + std::string(64, 'A')).empty() && tonRawAddress("128:" + std::string(64, 'A')).empty());
    // Туда и обратно: случайный сырой адрес → человеческий (кодировщик ниже)
    // → сырой. Так в адресах встречается каждая буква алфавита base64.
    {
        std::mt19937_64 g(0xADD);
        std::set<char> letters;
        for (int i = 0; i < 20000 / QUICK; ++i) {
            std::array<uint8_t, 34> body{};
            body[0] = (g() & 1) ? 0x11 : 0x51;
            if (g() & 1) body[0] |= 0x80;  // тестовая сеть — тот же адрес
            body[1] = static_cast<uint8_t>(g() % 3 == 0 ? 0xFF : 0);
            for (size_t k = 2; k < 34; ++k) body[k] = static_cast<uint8_t>(g());
            const std::string nice = friendlyOf(body, g() & 1);
            for (char ch : nice) letters.insert(ch);
            std::string raw = (body[1] ? "-1:" : "0:");
            static const char* H = "0123456789ABCDEF";
            for (size_t k = 2; k < 34; ++k) { raw += H[body[k] >> 4]; raw += H[body[k] & 15]; }
            CHECK(tonRawAddress(nice) == raw);
        }
        CHECK(letters.size() == 66);  // 62 буквы и цифры, «-_» и «+/»
        std::array<uint8_t, 34> odd{};
        odd[0] = 0x12;  // неизвестный флаг
        CHECK(tonRawAddress(friendlyOf(odd, true)).empty());
    }
    for (const std::string& bad : {std::string(""), WALLET.substr(1), WALLET + "A", std::string(48, '='),
                                   std::string("UQDAiNYvy2KUIwjEcgD1ZxPVw-CPwdk4WbBQwpVsQQ5jsO6p"),  // контрольная сумма
                                   "0:" + std::string(63, 'A'), "0:" + std::string(64, 'G'), "x:" + std::string(64, 'A'),
                                   "999:" + std::string(64, 'A'), "0:" + std::string(64, 'A') + " ", WALLET_RAW + "0"})
        CHECK(tonRawAddress(bad).empty());
    // Любая порча одного знака человеческого адреса ловится контрольной суммой.
    const std::string abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int caught = 0, tried = 0;
    for (size_t i = 0; i < WALLET.size(); ++i)
        for (char ch : abc) {
            if (ch == WALLET[i]) continue;
            std::string w = WALLET;
            w[i] = ch;
            ++tried;
            if (tonRawAddress(w) != WALLET_RAW) ++caught;
        }
    // 48 знаков × 6 бит = 36 байт ровно: значим каждый бит, и CRC-16 ловит
    // любую одиночную порчу.
    CHECK(tried == 48 * 63 && caught == tried);

    // Перевод: годный проходит, у каждой порчи — отказ.
    auto ok = usdtTransfer(transfer(" wt-abcde ", 4990000, "H"), WALLET_RAW);
    CHECK(ok && ok->memo == "WT-ABCDE" && ok->units == 4990000 && ok->hash == "H");
    auto spoil = [&](const char* key, const json& v) { json t = transfer("WT-ABCDE", 4990000); t[key] = v; return t; };
    CHECK(!usdtTransfer(spoil("transaction_aborted", true), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("transaction_aborted", "false"), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("transaction_aborted", nullptr), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("jetton_master", FAKE_MASTER), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("destination", OTHER_RAW), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("amount", "999999"), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("amount", 4990000), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("amount", "+4990000"), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("amount", "4990000.5"), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("amount", "99999999999999999999"), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("decoded_forward_payload", nullptr), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("transaction_hash", ""), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("transaction_hash", std::string(129, 'h')), WALLET_RAW));
    CHECK(usdtTransfer(spoil("transaction_hash", std::string(128, 'h')), WALLET_RAW).has_value());
    CHECK(!usdtTransfer(spoil("transaction_hash", 5), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("transaction_now", 0), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("transaction_now", "1795000000"), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("transaction_now", 1795000000.5), WALLET_RAW));
    CHECK(usdtTransfer(spoil("transaction_now", 1), WALLET_RAW)->at == 1);
    CHECK(transferFitsInvoice(1000 - 600, 1000) && !transferFitsInvoice(1000 - 601, 1000) && transferFitsInvoice(5000, 1000));
    // Опрос: счёт моложе часа — всегда; все старше — раз в 2 минуты.
    CHECK(FRESH_INVOICE_SEC == 3600 && SLOW_POLL_SEC == 120);
    CHECK(pollDue(10000, 10000 - 3599, 9999) && !pollDue(10000, 10000 - 3600, 9999));
    CHECK(pollDue(10000, 0, 10000 - 120) && !pollDue(10000, 0, 10000 - 119) && pollDue(10000, 0, 0));
    CHECK(!usdtTransfer(spoil("decoded_forward_payload", {{"comment", 5}}), WALLET_RAW));
    CHECK(!usdtTransfer(spoil("decoded_forward_payload", {{"comment", "   "}}), WALLET_RAW));
    CHECK(!usdtTransfer(transfer("WT-ABCDE", 4990000), ""));
    json noDest = transfer("WT-ABCDE", 4990000);
    noDest.erase("destination");
    CHECK(!usdtTransfer(noDest, "") && !usdtTransfer(noDest, WALLET_RAW));
    CHECK(USDT_MIN_UNITS == 1000000 && usdtTransfer(transfer("WT-A", 1000000), WALLET_RAW).has_value());
    CHECK(!usdtTransfer(transfer("WT-A", 999999), WALLET_RAW));
    json noAbort = transfer("WT-ABCDE", 4990000);
    noAbort.erase("transaction_aborted");
    CHECK(!usdtTransfer(noAbort, WALLET_RAW));
    // Мастер и получатель в человеческом виде — тоже годятся.
    json friendly = spoil("jetton_master", "EQCxE6mUtQJKFnGfaROTKOt1lZbDiiX1kCixRv7Nw2Id_sDs");
    friendly["destination"] = WALLET;
    CHECK(usdtTransfer(friendly, WALLET_RAW).has_value());
}

// Случайные ответы обозревателя: ни одного исключения, проходит только то,
// что годно по каждому правилу сразу.
static void fuzz() {
    std::mt19937_64 r(0xF0220);
    const std::vector<json> junk = {nullptr, true, false, 0, -1, 1.5, "", "x", "nan", json::array(), json::object(),
                                    "0:", std::string(5000, 'A'), "\xff\xfe", "WT-ABCDE", USDT_MASTER, WALLET_RAW};
    auto pick = [&](const std::vector<json>& v) -> const json& { return v[r() % v.size()]; };
    int passed = 0;
    for (int i = 0; i < 50000 / QUICK; ++i) {
        json t = transfer(r() % 2 ? "WT-ABCDE" : " wt-xyz ", static_cast<long long>(r() % 10'000'000));
        for (const char* key : {"jetton_master", "destination", "amount", "transaction_aborted", "transaction_hash",
                                "decoded_forward_payload"}) {
            const auto roll = r() % 20;
            if (roll == 0) t.erase(key);
            else if (roll == 1) t[key] = pick(junk);
        }
        if (r() % 50 == 0) t = pick(junk);
        std::optional<UsdtTransfer> got;
        try {
            got = usdtTransfer(t, WALLET_RAW);
        } catch (...) {
            ++g_fail;
            std::cerr << "fuzz: исключение на " << t.dump().substr(0, 200) << std::endl;
            continue;
        }
        if (!got) continue;
        ++passed;
        CHECK(t.is_object() && t["transaction_aborted"] == false);
        CHECK(tonRawAddress(t["jetton_master"].get<std::string>()) == USDT_MASTER);
        CHECK(tonRawAddress(t["destination"].get<std::string>()) == WALLET_RAW);
        CHECK(got->units >= USDT_MIN_UNITS && std::to_string(got->units) == t["amount"].get<std::string>());
        CHECK(!got->memo.empty() && got->memo == memoKey(t["decoded_forward_payload"]["comment"].get<std::string>()));
    }
    CHECK(passed > 10000 / QUICK);
}

// ── premium.cpp на базе ──────────────────────────────────────────────────────
static void openBare() {
    closeDb();
    sqlite3_open(":memory:", &db);
    exec("CREATE TABLE users(chat_id TEXT PRIMARY KEY, language TEXT NOT NULL DEFAULT 'en', "
         "threshold_nanos INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL)");
    exec("CREATE TABLE funnel_events(chat_id TEXT NOT NULL, ev TEXT NOT NULL, src TEXT NOT NULL DEFAULT '', "
         "day TEXT NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (chat_id, ev, src, day))");
    g_urls.clear(); g_tgPosts.clear(); g_sent.clear(); g_ended.clear(); g_refreshed = 0;
}

// До initPremium (или после неудачного) оплата выключена целиком: ни выдачи,
// ни опроса, а на счёт — «оплата временно недоступна».
static void disabled() {
    g_urls.clear();
    g_tgPosts.clear();
    CHECK(!grantPremiumDays("1", 30));
    CHECK(!handleSuccessfulPayment("1", successful("premium_30_days", 250, "z1")));
    CHECK(premiumSubsetOf({"1"}).empty());
    pollUsdtPayments();
    CHECK(g_urls.empty());
    handlePreCheckoutQuery(preCheckout("premium_30_days", 250, 1));
    CHECK(!lastAnswerOk() && g_tgPosts.back()["error_message"] == tr(Lang::RU, "payments_unavailable"));
    CHECK(num("SELECT COUNT(*) FROM users") <= 0 || num("SELECT COUNT(*) FROM users WHERE chat_id='1'") == 0);
}

static void schema() {
    openBare();
    disabled();
    // Повторный запуск на той же базе — не ошибка; отметку служебного
    // аккаунта запуск снимает.
    CHECK(initPremium("777"));
    exec("INSERT INTO users(chat_id, created_at, is_premium, premium_expire) VALUES('777', 1, 1, 9999999999)");
    CHECK(initPremium("777"));
    CHECK(num("SELECT is_premium + premium_expire FROM users WHERE chat_id='777'") == 0);
    CHECK(num("SELECT sub_until FROM premium_payments") == LLONG_MIN);  // столбец есть, строк нет
    // Миграция, которая не удалась по-настоящему, — оплата выключена.
    closeDb();
    sqlite3_open(":memory:", &db);
    exec("CREATE TABLE real_users(chat_id TEXT, created_at INTEGER)");
    exec("CREATE VIEW users AS SELECT chat_id, created_at FROM real_users");
    CHECK(!initPremium("777"));
    disabled();
    // Столбцы добавились, а таблица платежей не создалась (на её месте
    // представление, индекс на него не встаёт) — тоже выключена.
    openBare();
    exec("CREATE TABLE x(chat_id TEXT)");
    exec("CREATE VIEW premium_payments AS SELECT chat_id FROM x");
    CHECK(!initPremium("777"));
    disabled();
    // Не добавился только столбец (users — представление), а всё прочее
    // встало и служебного аккаунта нет — всё равно выключена.
    closeDb();
    sqlite3_open(":memory:", &db);
    exec("CREATE TABLE real_users(chat_id TEXT, created_at INTEGER)");
    exec("CREATE VIEW users AS SELECT chat_id, created_at FROM real_users");
    CHECK(!initPremium(""));
    disabled();
    // Таблицы и столбцы на месте, а индекс не встал (имя занято таблицей) —
    // схема не та, что ждёт код: выключена.
    openBare();
    exec("CREATE TABLE idx_ton_chat(x)");
    CHECK(!initPremium("777"));
    disabled();
    // Старая таблица платежей без столбца sub_until, а база только на чтение:
    // столбцы людей уже есть, таблицы тоже — не добавился один столбец, и
    // оплата всё равно выключена.
    openBare();
    exec("ALTER TABLE users ADD COLUMN is_premium INTEGER NOT NULL DEFAULT 0");
    exec("ALTER TABLE users ADD COLUMN premium_start INTEGER DEFAULT 0");
    exec("ALTER TABLE users ADD COLUMN premium_expire INTEGER DEFAULT 0");
    exec("CREATE TABLE premium_payments(id INTEGER PRIMARY KEY AUTOINCREMENT, chat_id TEXT NOT NULL, "
         "telegram_payment_charge_id TEXT NOT NULL UNIQUE, provider_payment_charge_id TEXT, payload TEXT NOT NULL, "
         "amount INTEGER NOT NULL, currency TEXT NOT NULL, paid_at INTEGER NOT NULL)");
    exec("CREATE INDEX idx_premium_chat ON premium_payments(chat_id)");
    exec("CREATE INDEX idx_premium_paid ON premium_payments(paid_at)");
    exec("CREATE TABLE ton_invoices(memo TEXT PRIMARY KEY, chat_id TEXT NOT NULL, nano_amount INTEGER NOT NULL, "
         "status TEXT NOT NULL DEFAULT 'active', created_at INTEGER NOT NULL, paid_at INTEGER NOT NULL DEFAULT 0, "
         "tx_hash TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL DEFAULT 'ton', days INTEGER NOT NULL DEFAULT 30)");
    exec("CREATE INDEX idx_ton_active ON ton_invoices(status, created_at)");
    exec("CREATE INDEX idx_ton_chat ON ton_invoices(chat_id)");
    exec("PRAGMA query_only=1");
    CHECK(!initPremium(""));
    exec("PRAGMA query_only=0");
    disabled();
    // Нет таблицы людей — выдавать некуда, оплата выключена.
    closeDb();
    sqlite3_open(":memory:", &db);
    CHECK(!initPremium("777"));
    disabled();
}

static void usdt() {
    freshDb();
    const long long t0 = now();
    invoice("WT-ABCDE", "100", 4990000, t0 - 60);
    invoice("WT-YEAR2", "200", 49900000, t0 - 60, 365);
    invoice("WT-OLDXX", "300", 4990000, t0 - 86401);
    invoice("WT-UNDER", "400", 4990000, t0 - 60);

    // Чужой жетон, чужой получатель, недоплата и сорванный перевод — мимо.
    json fake = transfer("WT-ABCDE", 4990000, "f1");
    fake["jetton_master"] = FAKE_MASTER;
    json elsewhere = transfer("WT-ABCDE", 4990000, "f2");
    elsewhere["destination"] = OTHER_RAW;
    json aborted = transfer("WT-ABCDE", 4990000, "f3");
    aborted["transaction_aborted"] = true;
    g_explorer = json{{"jetton_transfers", {fake, elsewhere, aborted, transfer("wt-under", 4890199, "u1"),
                                            transfer("WT-OLDXX", 4990000, "o1"), "junk", 5}}}.dump();
    pollUsdtPayments();
    // Спрашиваем только переводы USD₮ на наш кошелёк, адреса — в сыром виде.
    CHECK(g_urls.size() == 1 && g_urls[0].find("owner_address=" + WALLET_RAW + "&") != std::string::npos);
    CHECK(g_urls[0].find(std::string("jetton_master=") + USDT_MASTER + "&") != std::string::npos);
    CHECK(g_urls[0].find("direction=in") != std::string::npos && g_urls[0].find("limit=50") != std::string::npos);
    CHECK(num("SELECT COUNT(*) FROM ton_invoices WHERE status='paid'") == 0);
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-OLDXX'") == "expired");
    CHECK(expireOf("100") == LLONG_MIN && g_sent.empty());

    // Годные: месяц и год, памятка перебита руками, второй круг — без повтора.
    g_explorer = json{{"jetton_transfers", {transfer(" wt-abcde\n", 4990000, "p1"), transfer("WT-YEAR2", 49900000, "p2"),
                                            transfer("WT-UNDER", 4890200, "p3")}}}.dump();
    for (int round = 0; round < 2; ++round) pollUsdtPayments();
    CHECK(text("SELECT status || tx_hash FROM ton_invoices WHERE memo='WT-ABCDE'") == "paidp1");
    CHECK(text("SELECT status || tx_hash FROM ton_invoices WHERE memo='WT-UNDER'") == "paidp3");
    const long long e100 = expireOf("100"), e200 = expireOf("200");
    CHECK(e100 >= t0 + 30 * 86400 && e100 <= now() + 30 * 86400);
    CHECK(e200 >= t0 + 365 * 86400LL && e200 <= now() + 365 * 86400LL);
    CHECK(num("SELECT is_premium FROM users WHERE chat_id='100'") == 1);
    CHECK(g_sent.size() == 3 && g_sent[0].first == "100");
    CHECK(g_sent[0].second.find("30") != std::string::npos && g_sent[0].second.find("{n}") == std::string::npos);
    CHECK(g_sent[1].first == "200" && g_sent[1].second.find("365") != std::string::npos);
    CHECK(num("SELECT COUNT(*) FROM funnel_events WHERE ev='paid' AND src='usdt'") == 3);
    CHECK(g_refreshed >= 3);

    // Срок счёта — только месяц или год; что-то другое в базе — месяц.
    // Сумма счёта ниже доллара — не наш счёт; ровно доллар — наш. Человека
    // нет в базе — счёт не трогаем.
    g_ghosts = {"ghost"};
    invoice("WT-WEEK7", "601", 4990000, now(), 7);
    invoice("WT-DAY31", "602", 4990000, now(), 31);
    invoice("WT-CHEAP", "603", 500000, now());
    invoice("WT-ONEUS", "604", 1000000, now());
    invoice("WT-GHOST", "ghost", 4990000, now());
    g_explorer = json{{"jetton_transfers", {transfer("WT-WEEK7", 4990000, "w7"), transfer("WT-DAY31", 4990000, "d31"),
                                            transfer("WT-CHEAP", 1000000, "ch"), transfer("WT-ONEUS", 1000000, "one"),
                                            transfer("WT-GHOST", 4990000, "gh")}}}.dump();
    const long long t1 = now();
    pollUsdtPayments();
    auto ownerAlerts = [&]() {
        int n = 0;
        for (const auto& [to, msg] : g_sent)
            if (to == "owner") {
                ++n;
                CHECK(msg.find("4.99 USD₮") != std::string::npos && msg.find("WT-GHOST") != std::string::npos);
            }
        return n;
    };
    CHECK(ownerAlerts() == 1);  // сразу, с первого круга
    pollUsdtPayments();  // второй круг — владельцу не пишем повторно
    CHECK(ownerAlerts() == 1);
    for (const char* c : {"601", "602", "604"}) {
        const long long e = expireOf(c);
        CHECK(e >= t1 + 30 * 86400 && e <= now() + 30 * 86400);
    }
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-CHEAP'") == "active" && expireOf("603") == LLONG_MIN);
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-GHOST'") == "active");
    g_ghosts.clear();
    pollUsdtPayments();  // человек появился — следующий круг выдаёт
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-GHOST'") == "paid");

    // Нет живых счетов — к обозревателю не ходим вовсе.
    exec("UPDATE ton_invoices SET status='paid' WHERE status='active'");
    g_urls.clear();
    pollUsdtPayments();
    CHECK(g_urls.empty());

    // Битый ответ обозревателя — ничего не меняется и ничего не падает.
    invoice("WT-LATER", "500", 4990000, now());
    for (const char* bad : {"", "{", "[]", "null", R"({"jetton_transfers":5})", R"({"jetton_transfers":[null,[],{}]})"}) {
        g_explorer = bad;
        pollUsdtPayments();
    }
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-LATER'") == "active");

    // Выдать не удалось (служебный чат) — счёт остаётся активным: деньги не
    // теряются молча, следующий круг попробует снова.
    invoice("WT-SERVC", "777", 4990000, now());
    g_explorer = json{{"jetton_transfers", {transfer("WT-SERVC", 4990000, "s1")}}}.dump();
    pollUsdtPayments();
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-SERVC'") == "active");
    CHECK(num("SELECT COUNT(*) FROM users WHERE chat_id='777' AND is_premium=1") == 0);
    // Счёт старше суток — истёк, даже если перевод по нему пришёл.
    exec("UPDATE ton_invoices SET created_at=created_at-86400 WHERE memo='WT-SERVC'");
    invoice("WT-OLD24", "800", 4990000, now() - 86400 - 5);
    g_explorer = json{{"jetton_transfers", {transfer("WT-OLD24", 4990000, "o2")}}}.dump();
    g_urls.clear();
    pollUsdtPayments();
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-OLD24'") == "expired" && expireOf("800") == LLONG_MIN);
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-SERVC'") == "expired");
}

static void stars() {
    freshDb();
    // Подтверждение перед оплатой.
    handlePreCheckoutQuery(preCheckout("premium_30_days", 250, 42));
    CHECK(lastAnswerOk() && g_tgPosts.back()["pre_checkout_query_id"] == "q1");
    for (const json& bad : {preCheckout("premium_30_days", 249, 42), preCheckout("premium_30_days", 250.0, 42),
                            preCheckout("premium_30_days", "250", 42), preCheckout("premium_30_days", 250LL + (1LL << 32), 42),
                            preCheckout("premium_30_days", 250, 42, "USD"), preCheckout("hack", 250, 42),
                            preCheckout("premium_30_intro:43:" + std::to_string(now() + 99), 150, 42),
                            preCheckout("premium_30_intro:42:" + std::to_string(now() - INTRO_GRACE_SEC - 5), 150, 42)}) {
        handlePreCheckoutQuery(bad);
        CHECK(!lastAnswerOk());
        CHECK(!g_tgPosts.back().value("error_message", "").empty());
    }
    handlePreCheckoutQuery(preCheckout("premium_30_intro:42:" + std::to_string(now() + 99), 150, 42));
    CHECK(lastAnswerOk());
    // Сообщения отказа — по причине.
    handlePreCheckoutQuery(preCheckout("premium_30_intro:43:" + std::to_string(now() + 99), 150, 42));
    CHECK(g_tgPosts.back()["error_message"] == tr(Lang::RU, "intro_gone"));
    handlePreCheckoutQuery(preCheckout("hack", 250, 42));
    CHECK(g_tgPosts.back()["error_message"] == tr(Lang::RU, "invoice_unknown_product"));
    // Без покупателя (или с номером-строкой) обычный тариф — можно, вводная — нет.
    json noFrom = preCheckout("premium_30_days", 250, 1);
    noFrom.erase("from");
    handlePreCheckoutQuery(noFrom);
    CHECK(lastAnswerOk());
    json emptyFrom = preCheckout("premium_30_days", 250, 1);
    emptyFrom["from"] = json::object();
    handlePreCheckoutQuery(emptyFrom);
    CHECK(lastAnswerOk());
    json strFrom = preCheckout("premium_30_intro:42:" + std::to_string(now() + 99), 150, 1);
    strFrom["from"]["id"] = "42";
    handlePreCheckoutQuery(strFrom);
    CHECK(!lastAnswerOk());
    handlePreCheckoutQuery(json{{"id", "q2"}, {"invoice_payload", 5}, {"currency", "XTR"}, {"total_amount", 250}});
    CHECK(!lastAnswerOk() && g_tgPosts.back()["pre_checkout_query_id"] == "q2");
    const size_t posts = g_tgPosts.size();
    handlePreCheckoutQuery(json{{"from", {{"id", 1}}}});  // без id — отвечать нечем
    handlePreCheckoutQuery(json::array());
    CHECK(g_tgPosts.size() == posts);

    // Оплата: месяц, повтор того же платежа, год сверху.
    const long long t0 = now();
    CHECK(handleSuccessfulPayment("42", successful("premium_30_days", 250, "c1")));
    const long long e1 = expireOf("42");
    CHECK(e1 >= t0 + 30 * 86400 && e1 <= now() + 30 * 86400);
    CHECK(handleSuccessfulPayment("42", successful("premium_30_days", 250, "c1")));  // повтор — не продлевает
    CHECK(expireOf("42") == e1);
    CHECK(num("SELECT COUNT(*) FROM premium_payments") == 1);
    CHECK(text("SELECT chat_id || '|' || telegram_payment_charge_id || '|' || provider_payment_charge_id || '|' || payload || "
               "'|' || amount || '|' || currency || '|' || sub_until FROM premium_payments") == "42|c1||premium_30_days|250|XTR|0");
    CHECK(num("SELECT paid_at FROM premium_payments") >= t0);
    CHECK(handleSuccessfulPayment("42", successful("premium_365_days", 1990, "c2")));
    CHECK(expireOf("42") == e1 + 365 * 86400LL);
    CHECK(isPremium("42") && premiumMaxWallets("42") == 50 && premiumMaxWallets("43") == 3);
    CHECK(num("SELECT COUNT(*) FROM funnel_events WHERE ev='paid' AND src='stars'") == 1);  // раз в сутки

    // Не та цена, валюта, тариф, нет номера платежа, мусор — отказ без следа.
    const size_t sent = g_sent.size();
    for (const json& bad : {successful("premium_30_days", 249, "b1"), successful("premium_30_days", 250LL + (1LL << 32), "b2"),
                            successful("nope", 250, "b3"), successful("premium_30_days", 250, ""),
                            json{{"total_amount", 250.0}, {"currency", "XTR"}, {"invoice_payload", "premium_30_days"},
                                 {"telegram_payment_charge_id", "b4"}},
                            json{{"invoice_payload", 5}}, json::array(), json(nullptr),
                            json{{"currency", "XTR"}, {"total_amount", 250}, {"invoice_payload", "premium_30_days"},
                                 {"telegram_payment_charge_id", "b5"}, {"provider_payment_charge_id", 5}}})
        CHECK(!handleSuccessfulPayment("43", bad));
    CHECK(num("SELECT COUNT(*) FROM premium_payments") == 2 && !isPremium("43") && g_sent.size() == sent);

    // Продление подписки Telegram — короткое сообщение; странный флаг — не падает.
    json renew = successful("premium_30_days", 250, "c3");
    renew["is_recurring"] = true;
    CHECK(handleSuccessfulPayment("44", renew));
    CHECK(num("SELECT COUNT(*) FROM premium_payments WHERE chat_id='44'") == 1);
    json sub = successful("premium_30_days", 250, "c5");
    sub["subscription_expiration_date"] = 1900000000;
    CHECK(handleSuccessfulPayment("46", sub));
    CHECK(num("SELECT sub_until FROM premium_payments WHERE telegram_payment_charge_id='c5'") == 1900000000);
    json odd = successful("premium_30_days", 250, "c4");
    odd["is_recurring"] = "yes";
    CHECK(handleSuccessfulPayment("45", odd));
    CHECK(g_sent.size() == sent + 3 && g_sent[sent].second != g_sent[sent + 2].second);
    CHECK(g_sent[sent].second == withDays(tr(Lang::RU, "payment_renewed"), 30));

    // Звёзды списаны, а база занята — Премиум не выдан, владельцу сообщение;
    // тот же платёж потом проходит (запись откатилась целиком).
    g_sent.clear();
    exec("BEGIN");
    CHECK(!handleSuccessfulPayment("47", successful("premium_365_days", 1990, "c9")));
    exec("ROLLBACK");
    CHECK(g_sent.size() == 1 && g_sent[0].first == "owner");
    CHECK(g_sent[0].second.find("1990 XTR") != std::string::npos && g_sent[0].second.find("c9") != std::string::npos &&
          g_sent[0].second.find("365") != std::string::npos);
    CHECK(num("SELECT COUNT(*) FROM premium_payments WHERE telegram_payment_charge_id='c9'") == 0);
    CHECK(handleSuccessfulPayment("47", successful("premium_365_days", 1990, "c9")) && isPremium("47"));

    // Служебный аккаунт — никогда не подписчик.
    CHECK(!grantPremiumDays("777", 30) && !isPremium("777"));
    exec("UPDATE users SET is_premium=1, premium_expire=9999999999 WHERE chat_id='42'");
    exec("INSERT INTO users(chat_id, created_at, is_premium, premium_expire) VALUES('777', 1, 1, 9999999999)");
    CHECK(!isPremium("777"));
    CHECK(premiumSubsetOf({"42", "43", "777"}) == std::set<std::string>{"42"});
    // Флаг снят — срок в будущем не в счёт.
    exec("UPDATE users SET is_premium=0 WHERE chat_id='45'");
    CHECK(!isPremium("45") && premiumSubsetOf({"45"}).empty());
    // Больше предела параметров SQLite — кусками, подписчики не теряются.
    std::vector<std::string> many;
    for (int i = 0; i < 2500; ++i) many.push_back("n" + std::to_string(i));
    many.push_back("42");
    exec("UPDATE users SET is_premium=1, premium_expire=9999999999 WHERE chat_id='44'");
    many.insert(many.begin() + 1000, "44");
    CHECK(premiumSubsetOf(many) == (std::set<std::string>{"42", "44"}));
    CHECK(premiumSubsetOf({}).empty());
}

static void grantAndExpire() {
    freshDb();
    CHECK(!grantPremiumDays("", 30) && !grantPremiumDays("9", 0) && !grantPremiumDays("9", MAX_GRANT_DAYS + 1));
    const long long t0 = now();
    CHECK(grantPremiumDays("9", 7) && g_refreshed == 1);
    const long long e = expireOf("9");
    const long long st = num("SELECT premium_start FROM users WHERE chat_id='9'");
    CHECK(st >= t0 && st <= now() && e == st + 7 * 86400);
    CHECK(grantPremiumDays("9", 1) && expireOf("9") == e + 86400 && g_refreshed == 2);
    CHECK(num("SELECT premium_start FROM users WHERE chat_id='9'") == st);  // начало не сдвигается
    CHECK(grantPremiumDays("10", MAX_GRANT_DAYS));
    // Нет строки человека — нечего продлевать.
    g_ghosts = {"ghost"};
    CHECK(!grantPremiumDays("ghost", 30) && g_refreshed == 3);
    g_ghosts.clear();
    // База занята чужой транзакцией — отказ, а не запись мимо неё.
    exec("BEGIN");
    CHECK(!grantPremiumDays("9", 1));
    exec("ROLLBACK");
    CHECK(expireOf("9") == e + 86400);
    exec("DELETE FROM users WHERE chat_id='10'");
    // Срок у края числа — отказ, база не тронута (транзакция откатилась).
    exec("UPDATE users SET premium_expire=" + std::to_string(LLONG_MAX - 10) + " WHERE chat_id='9'");
    CHECK(!grantPremiumDays("9", 1) && expireOf("9") == LLONG_MAX - 10);
    CHECK(num("SELECT COUNT(*) FROM users") == 1);

    // Истёкших нет — ни писем, ни пересборки наблюдателей.
    const int refreshed = g_refreshed;
    cleanupExpiredPremium();
    CHECK(g_refreshed == refreshed && g_ended.empty());
    // Истёкшие — в бесплатные, каждому письмо, кроме служебного.
    exec("UPDATE users SET is_premium=1, premium_expire=5 WHERE chat_id='9'");
    exec("INSERT INTO users(chat_id, created_at, is_premium, premium_expire) VALUES('777', 1, 1, 5), ('8', 1, 1, 9999999999)");
    cleanupExpiredPremium();
    CHECK(num("SELECT COUNT(*) FROM users WHERE is_premium=1") == 1);
    CHECK(g_ended == std::vector<std::string>{"9"});
    cleanupExpiredPremium();
    CHECK(g_ended.size() == 1);
    // Ровно один истёкший — тоже письмо.
    exec("UPDATE users SET is_premium=1, premium_expire=5 WHERE chat_id='8'");
    cleanupExpiredPremium();
    CHECK((g_ended == std::vector<std::string>{"9", "8"}));
    // База занята чужой записью — проход ничего не меняет и писем не шлёт;
    // следующий проход сделает всё.
    exec("UPDATE users SET is_premium=1, premium_expire=5 WHERE chat_id='8'");
    exec("BEGIN");
    cleanupExpiredPremium();
    exec("ROLLBACK");
    CHECK(g_ended.size() == 2 && num("SELECT is_premium FROM users WHERE chat_id='8'") == 1);
    cleanupExpiredPremium();
    CHECK(g_ended.size() == 3 && num("SELECT is_premium FROM users WHERE chat_id='8'") == 0);
}

static void autorenew() {
    freshDb();
    const long long t = now();
    exec("INSERT INTO premium_payments(chat_id, telegram_payment_charge_id, payload, amount, currency, paid_at, sub_until) "
         "VALUES('1','a','premium_30_days',250,'XTR'," + std::to_string(t - 10) + "," + std::to_string(t + 99) + "),"
         "('2','b','premium_30_days',250,'XTR'," + std::to_string(t - 10) + "," + std::to_string(t + 99) + "),"
         "('3','c','premium_30_days',250,'XTR'," + std::to_string(t - 10) + ",0),"
         "('-100','d','premium_30_days',250,'XTR'," + std::to_string(t - 10) + "," + std::to_string(t + 99) + ")");
    for (const char* bad : {R"({"ok":false,"description":"x"})", "{}", "", "garbage"}) {
        g_tgAnswer = bad;
        CHECK(cancelStarSubscriptions("", false) == 0);
        CHECK(num("SELECT COUNT(*) FROM premium_payments WHERE sub_until>0") == 3);  // не вышло — не трогаем
    }
    g_sent.clear();
    CHECK(cancelStarSubscriptions("owner", false) == 0);
    CHECK(g_sent.size() == 1 && g_sent[0].first == "owner" && g_sent[0].second.find("Не вышло: 3") != std::string::npos);
    g_tgAnswer = R"({"ok":true,"result":true})";
    g_tgPosts.clear();
    CHECK(cancelStarSubscriptions("", false) == 2);
    // Номер «-100» — не человек: в Telegram не уходит, считается неудачей.
    CHECK(g_tgPosts.size() == 2 && g_tgPosts[0]["is_canceled"] == true);
    CHECK(g_tgPosts[0]["telegram_payment_charge_id"] == "a" && g_tgPosts[1]["telegram_payment_charge_id"] == "b");
    CHECK(g_tgPosts[0]["user_id"] == 1 && g_tgPosts[1]["user_id"] == 2);
    CHECK(num("SELECT COUNT(*) FROM premium_payments WHERE sub_until>0") == 1);
    exec("DELETE FROM premium_payments WHERE chat_id='-100'");
    g_sent.clear();
    CHECK(cancelStarSubscriptions("", false) == 0 && cancelStarSubscriptions("owner", false) == 0 && g_sent.empty());
    CHECK(cancelStarSubscriptions("owner", true) == 0 && g_sent.size() == 1 && g_sent[0].first == "owner");
    // Ровно одна неудача — тоже в отчёте.
    exec("INSERT INTO premium_payments(chat_id, telegram_payment_charge_id, payload, amount, currency, paid_at, sub_until) "
         "VALUES('-5','e','premium_30_days',250,'XTR',1," + std::to_string(t + 99) + ")");
    g_sent.clear();
    CHECK(cancelStarSubscriptions("owner", false) == 0);
    CHECK(g_sent.size() == 1 && g_sent[0].second.find("Не вышло: 1") != std::string::npos);
}

// Образцы ответов — договор с toncenter и Telegram: форма полей как в
// настоящих ответах (значения выдуманы). Поменяется формат у них — упадёт
// проверка, а не оплата у людей.
// Договор с API (Wallet-Tracker, whale_api.py): тот же файл лежит у API, и
// там свой тест сверяет с ним его константы. Разойдутся цена, срок, формат
// памятки или вводной цены, кошелёк, столбцы счёта — упадёт проверка, а не
// оплата у людей.
static void contract() {
    std::ifstream f("tests/fixtures/premium_contract.json");
    std::stringstream ss;
    ss << f.rdbuf();
    const json c = json::parse(ss.str(), nullptr, false);
    CHECK(c.is_object() && c.contains("star_plans"));
    if (!c.is_object() || !c.contains("star_plans")) return;
    CHECK(c["star_plans"].size() == STAR_PLANS.size());
    for (const auto& p : c["star_plans"]) {
        const auto* plan = starPlan(p["payload"].get<std::string>());
        CHECK(plan != nullptr && plan->stars == p["stars"].get<long long>() && plan->days == p["days"].get<int>());
    }
    CHECK(c["currency"] == "XTR" && checkStars("premium_30_days", c["currency"].get<std::string>(), 250, "1", 1) == Checkout::Ok);
    CHECK(c["intro_grace_sec"].get<long long>() == INTRO_GRACE_SEC);
    // Вводная цена — «<нагрузка>:<chat>:<до>», как её собирает API.
    std::string chat;
    long long until = 0;
    CHECK(c["intro_payload"] == "premium_30_intro:<chat_id>:<until_unix>");
    CHECK(introBinding("premium_30_intro:123:1800000000", chat, until) && chat == "123" && until == 1800000000);
    const auto& u = c["usdt"];
    CHECK(u["min_units"].get<long long>() == USDT_MIN_UNITS && u["decimals"] == 6);
    CHECK(u["master_raw"] == USDT_MASTER && tonRawAddress(u["master_ui"].get<std::string>()) == USDT_MASTER);
    CHECK(u["wallet_default"] == WALLET && tonRawAddress(WALLET) == u["wallet_default_raw"].get<std::string>());
    CHECK(u["invoice_shown_sec"].get<long long>() == FRESH_INVOICE_SEC);
    CHECK(u["invoice_accepted_sec"].get<long long>() == DAY_SEC);
    for (const auto& p : u["plans"]) CHECK(p["days"] == 30 || p["days"] == 365);
    // Памятка из алфавита API проходит memoKey без изменений.
    const std::regex memoRx(u["memo_regex"].get<std::string>());
    const std::string abc = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
    std::mt19937 r(7);
    for (int i = 0; i < 2000; ++i) {
        std::string memo = "WT-";
        for (int k = 0; k < 5; ++k) memo += abc[r() % abc.size()];
        CHECK(std::regex_match(memo, memoRx) && memoKey(memo) == memo);
    }
    // Столбцы счёта, которые пишут API и бот, — в таблице бота.
    freshDb();
    std::set<std::string> cols;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT name FROM pragma_table_info('ton_invoices')", -1, &st, nullptr);
    while (sqlite3_step(st) == SQLITE_ROW) cols.insert(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
    sqlite3_finalize(st);
    for (const auto* part : {"api_writes", "bot_writes"})
        for (const auto& col : c["ton_invoices"][part]) CHECK(cols.count(col.get<std::string>()) == 1);
    for (const auto& status : c["ton_invoices"]["statuses"])
        CHECK(status == "active" || status == "paid" || status == "expired");
    CHECK(u["kind"] == "usdt");
}

// Подменённые часы: границы сроков — до секунды.
static long long g_now = 0;
static long long fakeNow() { return g_now; }

static json fixture(const char* name) {
    // Запуск — из корня репозитория (tests/run.sh); FIXTURES — если иначе.
    const char* dir = std::getenv("FIXTURES");  // NOLINT(concurrency-mt-unsafe)
    std::ifstream f(std::string(dir != nullptr ? dir : "tests/fixtures") + "/" + name);
    std::stringstream ss;
    ss << f.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    CHECK(j.is_object());
    return j;
}

static void fixtures() {
    const json ton = fixture("toncenter_jetton_transfers.json");
    if (!ton.is_object() || !ton.contains("jetton_transfers")) return;  // провал уже засчитан
    const auto& list = ton["jetton_transfers"];
    CHECK(list.is_array() && list.size() == 2);
    const auto pay = usdtTransfer(list[0], WALLET_RAW);
    CHECK(pay && pay->memo == "WT-ABCDE" && pay->units == 3990000 && !pay->hash.empty());
    CHECK(!usdtTransfer(list[1], WALLET_RAW));  // без памятки (decoded_forward_payload: null)

    // Часы — на минуту позже перевода из образца: проверка не зависит от
    // сегодняшней даты (иначе через сутки счёт в ней «истёк» бы).
    freshDb();
    premium::setClock(&fakeNow);
    g_now = list[0]["transaction_now"].get<long long>() + 60;
    invoice("WT-ABCDE", "900", 3990000, g_now - 120);
    g_explorer = ton.dump();
    pollUsdtPayments();
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-ABCDE'") == "paid");
    CHECK(expireOf("900") == g_now + 30 * 86400);
    premium::setClock(nullptr);

    CHECK(handleSuccessfulPayment("901", fixture("telegram_successful_payment.json")));
    CHECK(expireOf("901") > now() + 29 * 86400);
    handlePreCheckoutQuery(fixture("telegram_pre_checkout_query.json"));
    CHECK(lastAnswerOk() && g_tgPosts.back()["pre_checkout_query_id"] == "4382bfdwdsb323b2d9");
}


static void clockEdges() {
    freshDb();
    premium::setClock(&fakeNow);
    g_now = 1'800'000'000;
    const std::string n = std::to_string(g_now);
    exec("INSERT INTO users(chat_id, created_at, is_premium, premium_expire) VALUES"
         "('at', 1, 1, " + n + "), ('next', 1, 1, " + std::to_string(g_now + 1) + ")");
    // Срок, равный «сейчас», — уже кончился; на секунду позже — ещё идёт.
    CHECK(!isPremium("at") && isPremium("next"));
    CHECK(premiumSubsetOf({"at", "next"}) == std::set<std::string>{"next"});
    cleanupExpiredPremium();
    CHECK(g_ended == std::vector<std::string>{"at"});
    g_now += 1;  // теперь и второй
    CHECK(!isPremium("next"));
    // Выдача — ровно от «сейчас», продление — ровно от конца.
    CHECK(grantPremiumDays("fresh", 1) && expireOf("fresh") == g_now + 86400);
    CHECK(num("SELECT premium_start FROM users WHERE chat_id='fresh'") == g_now);
    CHECK(grantPremiumDays("fresh", 2) && expireOf("fresh") == g_now + 3 * 86400);
    // Счёт USD₮ живёт ровно сутки: ровно сутки назад — истёк, на секунду моложе — жив.
    invoice("WT-EDGE0", "e0", 4990000, g_now - 86400);
    invoice("WT-EDGE1", "e1", 4990000, g_now - 86399);
    g_explorer = json{{"jetton_transfers", {transfer("WT-EDGE0", 4990000, "x0"), transfer("WT-EDGE1", 4990000, "x1")}}}.dump();
    pollUsdtPayments();
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-EDGE0'") == "expired");
    CHECK(text("SELECT status || paid_at FROM ton_invoices WHERE memo='WT-EDGE1'") == "paid" + std::to_string(g_now));
    CHECK(expireOf("e1") == g_now + 30 * 86400);
    // Воронка пишет день по часам модуля (UTC).
    CHECK(text("SELECT day FROM funnel_events WHERE chat_id='e1'") == "2027-01-15");
    // Звёзды: время оплаты и срок — по тем же часам.
    CHECK(handleSuccessfulPayment("s1", successful("premium_30_days", 250, "edge")));
    CHECK(num("SELECT paid_at FROM premium_payments WHERE telegram_payment_charge_id='edge'") == g_now);
    CHECK(expireOf("s1") == g_now + 30 * 86400);
    // Вводная цена — до конца окна плюс 15 минут включительно.
    handlePreCheckoutQuery(preCheckout("premium_30_intro:42:" + std::to_string(g_now - 900), 150, 42));
    CHECK(lastAnswerOk());
    handlePreCheckoutQuery(preCheckout("premium_30_intro:42:" + std::to_string(g_now - 901), 150, 42));
    CHECK(!lastAnswerOk());
    // Автопродление: действующее — sub_until строго в будущем.
    exec("INSERT INTO premium_payments(chat_id, telegram_payment_charge_id, payload, amount, currency, paid_at, sub_until) "
         "VALUES('71','p1','premium_30_days',250,'XTR',1," + n + "),('72','p2','premium_30_days',250,'XTR',1," +
         std::to_string(g_now + 1) + ")");
    g_tgPosts.clear();
    CHECK(cancelStarSubscriptions("", false) == 1 && g_tgPosts.size() == 1 && g_tgPosts[0]["user_id"] == 72);
    premium::setClock(nullptr);
    CHECK(isPremium("fresh") == (expireOf("fresh") > static_cast<long long>(time(nullptr))));
}

// Журнал: строка из чужих рук — одной строкой, без управляющих знаков, с
// пределом длины. Подделать в журнале «выдан Премиум» нельзя.
static void logging() {
    CHECK(premium::logSafe("ok") == "ok");
    CHECK(premium::logSafe("a\nb\r\x1b[31mc\x7f\td") == "a b  [31mc  d");
    CHECK(premium::logSafe(std::string(300, 'x')) == std::string(300, 'x'));
    CHECK(premium::logSafe(std::string(301, 'x')) == std::string(300, 'x') + "…");
    // Обрезка — по границе знака UTF-8: «ж» — два байта.
    std::string cyr;
    for (int i = 0; i < 200; ++i) cyr += "ж";
    const std::string cut = premium::logSafe(cyr, 5);
    CHECK(cut == "жж…");
    CHECK(premium::logSafe("", 0).empty() && premium::logSafe("x", 0) == "…");
    // Битый UTF-8 из одних байтов-продолжений — обрезка не уходит за начало.
    CHECK(premium::logSafe(std::string(10, '\x80'), 5) == "…");
    CHECK(premium::logSafe("ab" + std::string(10, '\x80'), 5) == "a…");  // «b»+продолжения — один знак

    premium::Once once(3);
    CHECK(once.first("a") && !once.first("a") && once.first("b") && once.first("c") && once.size() == 3);
    CHECK(once.first("d") && once.size() == 1);  // полон — начинается заново, а не растёт
    CHECK(once.first("a"));
    premium::Once big;
    for (int i = 0; i < 1000; ++i) (void)big.first("k" + std::to_string(i));
    CHECK(big.size() == 1000);
    CHECK(big.first("k1000") && big.size() == 1);  // по умолчанию — ровно 1000
    for (int i = 0; i < 20000; ++i) (void)big.first("k" + std::to_string(i));
    CHECK(big.size() <= 1000);

    // Подделка строки журнала через нагрузку счёта и ответ Telegram.
    freshDb();
    std::ostringstream err;
    std::streambuf* old = std::cerr.rdbuf(err.rdbuf());
    (void)handleSuccessfulPayment("1", successful("evil\n[PREMIUM] Activated for 666 (charge x)", 250, "z"));
    g_tgAnswer = "{\"ok\":false,\"description\":\"bad\\n[PREMIUM] Activated for 777\"}";
    handlePreCheckoutQuery(preCheckout("premium_30_days", 250, 1));
    std::cerr.rdbuf(old);
    const std::string log = err.str();
    CHECK(log.find("\n[PREMIUM] Activated") == std::string::npos);
    CHECK(log.find("evil [PREMIUM] Activated for 666") != std::string::npos);
    CHECK(log.find("bad [PREMIUM] Activated for 777") != std::string::npos);
    for (const char c : log) CHECK(c == '\n' || static_cast<unsigned char>(c) >= 0x20);
    g_tgAnswer = R"({"ok":true,"result":true})";
}

// Атаки на опрос USD₮: старый перевод с совпавшей памяткой, повтор хеша,
// HTML в ответах, сбой сети, пределы ответа.
static void attacks() {
    freshDb();
    premium::setClock(&fakeNow);
    g_now = 1'800'000'000;
    // Перевод сделан раньше счёта (больше чем на 10 минут) — не оплачивает его.
    invoice("WT-OLDTX", "a1", 4990000, g_now);
    invoice("WT-SKEWD", "a2", 4990000, g_now);
    g_explorer = json{{"jetton_transfers", {transfer("WT-OLDTX", 4990000, "old", g_now - 601),
                                            transfer("WT-SKEWD", 4990000, "skew", g_now - 600)}}}.dump();
    pollUsdtPayments();
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-OLDTX'") == "active");
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-SKEWD'") == "paid");
    // Один перевод (хеш) оплачивает один счёт: подделанный ответ с тем же
    // хешем и другой памяткой второй счёт не оплатит.
    invoice("WT-TWICE", "a3", 4990000, g_now);
    g_explorer = json{{"jetton_transfers", {transfer("WT-TWICE", 4990000, "skew", g_now)}}}.dump();
    pollUsdtPayments();
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-TWICE'") == "active" && !isPremium("a3"));
    // Вложенность в сотни тысяч уровней (ответ под предел в 1 МБ) — ни
    // переполнения стека при разборе и удалении, ни выдачи.
    for (const std::string& deep : {std::string(500'000, '[') + std::string(500'000, ']'),
                                    "{\"jetton_transfers\":" + std::string(400'000, '[') + std::string(400'000, ']') + "}",
                                    std::string(500'000, '[')}) {
        g_explorer = deep;
        pollUsdtPayments();
    }
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-OLDTX'") == "active");
    // Пределы ответа: toncenter — 1 МБ, Bot API — 64 КБ; адрес — только https.
    CHECK(!g_caps.empty() && g_caps.back() == (1U << 20U));
    g_caps.clear();
    handlePreCheckoutQuery(preCheckout("premium_30_days", 250, 1));
    CHECK(g_caps.size() == 1 && g_caps[0] == (64U << 10U) && g_urls.back().rfind("https://", 0) == 0);
    for (const auto& u : g_urls) CHECK(u.rfind("https://", 0) == 0);
    // Сбой сети — ничего не меняется, в журнал одна строка на причину.
    g_netError = "Timeout was reached";
    std::ostringstream err;
    std::streambuf* old = std::cerr.rdbuf(err.rdbuf());
    pollUsdtPayments();
    pollUsdtPayments();
    handlePreCheckoutQuery(preCheckout("premium_30_days", 250, 1));
    std::cerr.rdbuf(old);
    g_netError.clear();
    const std::string log = err.str();
    size_t n = 0;
    for (size_t at = log.find("[USDT] toncenter: Timeout"); at != std::string::npos;
         at = log.find("[USDT] toncenter: Timeout", at + 1))
        ++n;
    CHECK(n == 1);
    CHECK(log.find("answerPreCheckoutQuery failed: Timeout was reached") != std::string::npos);
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-OLDTX'") == "active");
    // HTML из ответа Telegram не ломает сообщение владельцу (parse_mode HTML).
    g_sent.clear();
    exec("CREATE TRIGGER html_boom BEFORE INSERT ON premium_payments WHEN NEW.chat_id='h1' BEGIN SELECT RAISE(ABORT,'x'); END");
    CHECK(!handleSuccessfulPayment("h1", successful("premium_30_days", 250, "<b>x</b>&\n")));
    CHECK(g_sent.size() == 1 && g_sent[0].second.find("&lt;b&gt;x&lt;/b&gt;&amp; ") != std::string::npos);
    CHECK(g_sent[0].second.find("<b>x") == std::string::npos);
    premium::setClock(nullptr);
}

// Редкий опрос старых счетов: toncenter спрашиваем реже, но спрашиваем.
static void slowPoll() {
    freshDb();
    premium::setClock(&fakeNow);
    g_now = 1'900'000'000;
    invoice("WT-OLD2H", "p1", 4990000, g_now - 7200);
    g_explorer = "{}";
    g_urls.clear();
    pollUsdtPayments();  // давно не спрашивали — сейчас
    CHECK(g_urls.size() == 1);
    for (int i = 0; i < 5; ++i) {  // 100 секунд по 20 — тишина
        g_now += 20;
        pollUsdtPayments();
    }
    CHECK(g_urls.size() == 1);
    g_now += 20;  // 120 секунд с прошлого запроса
    pollUsdtPayments();
    CHECK(g_urls.size() == 2);
    // Новый счёт — снова каждый проход.
    invoice("WT-NEW00", "p2", 4990000, g_now);
    g_now += 20;
    pollUsdtPayments();
    g_now += 20;
    pollUsdtPayments();
    CHECK(g_urls.size() == 4);
    // Оплата старого счёта находится и в редком режиме.
    exec("UPDATE ton_invoices SET status='paid' WHERE memo='WT-NEW00'");
    g_explorer = json{{"jetton_transfers", {transfer("WT-OLD2H", 4990000, "late", g_now)}}}.dump();
    g_now += 120;
    pollUsdtPayments();
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-OLD2H'") == "paid");
    // Живых счетов не осталось — сколько бы ни прошло, toncenter не спрашиваем.
    const size_t asked = g_urls.size();
    for (int i = 0; i < 3; ++i) {
        g_now += 1000;
        pollUsdtPayments();
    }
    CHECK(g_urls.size() == asked);
    premium::setClock(nullptr);
}

// Ручная выдача владельцем: строгий разбор, ответ с датой.
static void grantByOwner() {
    for (const char* ok : {"42 30", " 42   30 ", "42 3650", "42 1"}) CHECK(parseGrant(ok).has_value());
    const auto g = parseGrant("  777123  365 ");
    CHECK(g && g->chat == "777123" && g->days == 365);
    for (const char* bad : {"", " ", "42", "42 ", "42 0", "42 3651", "42 -1", "-42 30", "0 30", "x 30", "42 30 1",
                            "42 30x", "42\t30", "4 2 30", "042 30x", "99999999999999999999 30"})
        CHECK(!parseGrant(bad));
    CHECK(parseGrant("042 30")->chat == "42");  // номер — как число, без ведущих нулей
    CHECK(parseGrant("1 30")->chat == "1");

    freshDb();
    premium::setClock(&fakeNow);
    g_now = 1'800'000'000;
    const std::string r = grantCommand(" 555 30");
    CHECK(r.find("✅") == 0 && r.find("<code>555</code>") != std::string::npos);
    CHECK(r.find("2027-02-14 08:00 UTC") != std::string::npos);
    CHECK(expireOf("555") == g_now + 30 * 86400);
    CHECK(grantCommand("777 30").find("⚠️ Не выдано: 777") == 0);  // служебный аккаунт
    CHECK(grantCommand("abc").find("Использование") == 0);
    CHECK(grantCommand("").find("Использование") == 0);
    premium::setClock(nullptr);
}

// Сбои, которые в жизни редки, но обязаны кончаться ничем, а не полуделом.
static void failures() {
    // COMMIT не прошёл (отложенный внешний ключ, сработавший на фиксации) —
    // откат целиком: срок прежний, выдача — отказ.
    freshDb();
    exec("PRAGMA foreign_keys=ON");
    exec("CREATE TABLE fk_parent(id INTEGER PRIMARY KEY)");
    exec("CREATE TABLE fk_child(p INTEGER REFERENCES fk_parent(id) DEFERRABLE INITIALLY DEFERRED)");
    exec("CREATE TRIGGER fk_boom AFTER UPDATE ON users WHEN NEW.chat_id='boom' BEGIN INSERT INTO fk_child VALUES(42); END");
    CHECK(!grantPremiumDays("boom", 30));
    CHECK(num("SELECT premium_expire FROM users WHERE chat_id='boom'") == 0);
    CHECK(num("SELECT COUNT(*) FROM fk_child") == 0);
    CHECK(grantPremiumDays("fine", 30));  // база после отката живая

    // Запись платежа отклонена базой не как повтор — Премиум не выдан,
    // владельцу сообщение, звёзды выдать вручную.
    exec("CREATE TRIGGER pay_boom BEFORE INSERT ON premium_payments WHEN NEW.chat_id='payboom' "
         "BEGIN SELECT RAISE(ABORT, 'boom'); END");
    g_sent.clear();
    CHECK(!handleSuccessfulPayment("payboom", successful("premium_30_days", 250, "pb1")));
    CHECK(g_sent.size() == 1 && g_sent[0].first == "owner" && g_sent[0].second.find("pb1") != std::string::npos);
    CHECK(!isPremium("payboom") && num("SELECT COUNT(*) FROM premium_payments WHERE chat_id='payboom'") == 0);

    // Сообщение «оплата прошла» не доставлено — оплата всё равно записана.
    CHECK(handleSuccessfulPayment("mute", successful("premium_30_days", 250, "m1")) && isPremium("mute"));

    // Кошелёк приёма в окружении испорчен — опрос молчит и в сеть не ходит.
    invoice("WT-WALLT", "w1", 4990000, now());
    setenv("TON_WALLET_ADDRESS", "garbage", 1);  // NOLINT(concurrency-mt-unsafe)
    g_urls.clear();
    pollUsdtPayments();
    pollUsdtPayments();
    CHECK(g_urls.empty() && text("SELECT status FROM ton_invoices WHERE memo='WT-WALLT'") == "active");
    // Свой кошелёк в окружении — запрос идёт на него, в сыром виде.
    setenv("TON_WALLET_ADDRESS", "EQCxE6mUtQJKFnGfaROTKOt1lZbDiiX1kCixRv7Nw2Id_sDs", 1);  // NOLINT(concurrency-mt-unsafe)
    g_explorer = "{}";
    pollUsdtPayments();
    CHECK(g_urls.size() == 1 && g_urls[0].find(std::string("owner_address=") + USDT_MASTER) != std::string::npos);
    unsetenv("TON_WALLET_ADDRESS");  // NOLINT(concurrency-mt-unsafe)
}

// Пути сбоев, которых в обычной жизни нет, — вызываем настоящими средствами
// SQLite (триггер с RAISE, отложенный внешний ключ на COMMIT, удалённая
// таблица, чужая транзакция). Каждый кончается ничем, а не полуделом.
static void faults() {
    // Запрос не подготовился (таблицы нет) — Stmt ничего не делает и не падает.
    freshDb();
    exec("DROP TABLE funnel_events");
    CHECK(handleSuccessfulPayment("f1", successful("premium_30_days", 250, "fz1")) && isPremium("f1"));
    // Таблицы людей нет — выборка подписчиков пустая, а не падение.
    exec("DROP TABLE users");
    CHECK(premiumSubsetOf({"f1"}).empty());
    // Таблицы счетов нет — опрос молчит.
    exec("DROP TABLE ton_invoices");
    g_urls.clear();
    pollUsdtPayments();
    CHECK(g_urls.empty());

    // Без служебного аккаунта — всё работает, выборка не трогает пустое имя.
    closeDb();
    sqlite3_open(":memory:", &db);
    exec("CREATE TABLE users(chat_id TEXT PRIMARY KEY, language TEXT NOT NULL DEFAULT 'en', "
         "threshold_nanos INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL)");
    exec("CREATE TABLE funnel_events(chat_id TEXT NOT NULL, ev TEXT NOT NULL, src TEXT NOT NULL DEFAULT '', "
         "day TEXT NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (chat_id, ev, src, day))");
    CHECK(initPremium("", "owner"));
    exec("INSERT INTO users(chat_id, created_at, is_premium, premium_expire) VALUES('', 1, 1, 9999999999), ('x', 1, 1, 5)");
    CHECK(premiumSubsetOf({"", "x"}) == std::set<std::string>{""});
    // Истёкший с пустым номером чата — отметку снимаем, письмо не пишем никому.
    exec("UPDATE users SET premium_expire=5 WHERE chat_id=''");
    g_ended.clear();
    cleanupExpiredPremium();
    CHECK(g_ended == std::vector<std::string>{"x"});

    // Снятие истёкших: UPDATE отклонён или COMMIT не прошёл — откат, писем нет.
    freshDb();
    exec("INSERT INTO users(chat_id, created_at, is_premium, premium_expire) VALUES('c1', 1, 1, 5)");
    exec("CREATE TRIGGER c_boom BEFORE UPDATE OF is_premium ON users WHEN NEW.chat_id='c1' BEGIN SELECT RAISE(ABORT,'x'); END");
    g_ended.clear();
    cleanupExpiredPremium();
    CHECK(g_ended.empty() && num("SELECT is_premium FROM users WHERE chat_id='c1'") == 1);
    exec("DROP TRIGGER c_boom");
    exec("PRAGMA foreign_keys=ON");
    exec("CREATE TABLE fk_parent(id INTEGER PRIMARY KEY)");
    exec("CREATE TABLE fk_child(p INTEGER REFERENCES fk_parent(id) DEFERRABLE INITIALLY DEFERRED)");
    exec("CREATE TRIGGER c_fk AFTER UPDATE OF is_premium ON users WHEN NEW.chat_id='c1' BEGIN INSERT INTO fk_child VALUES(1); END");
    cleanupExpiredPremium();
    CHECK(g_ended.empty() && num("SELECT is_premium FROM users WHERE chat_id='c1'") == 1);

    // Звёзды: продление отклонено базой или COMMIT не прошёл — платёж не
    // записан, владельцу сообщение.
    exec("CREATE TRIGGER s_boom BEFORE UPDATE ON users WHEN NEW.chat_id IN ('s1','91') BEGIN SELECT RAISE(ABORT,'x'); END");
    exec("CREATE TRIGGER s_fk AFTER UPDATE ON users WHEN NEW.chat_id='s2' BEGIN INSERT INTO fk_child VALUES(2); END");
    for (const char* who : {"s1", "s2"}) {
        g_sent.clear();
        CHECK(!handleSuccessfulPayment(who, successful("premium_30_days", 250, std::string("ch-") + who)));
        CHECK(g_sent.size() == 1 && g_sent[0].first == "owner");
        CHECK(num("SELECT COUNT(*) FROM premium_payments WHERE chat_id='" + std::string(who) + "'") == 0);
    }
    // Ручная выдача упёрлась в отказ базы — «не выдано».
    CHECK(grantCommand("91 30").find("⚠️ Не выдано: 91") == 0);

    // USD₮: занять счёт не дали (BEGIN, UPDATE, «счёт занят», COMMIT) —
    // счёт активен, Премиума нет; «занят» — молча, остальное — владельцу.
    exec("CREATE TRIGGER u_boom BEFORE UPDATE ON ton_invoices WHEN OLD.memo='WT-UBOOM' BEGIN SELECT RAISE(ABORT,'x'); END");
    exec("CREATE TRIGGER u_skip BEFORE UPDATE ON ton_invoices WHEN OLD.memo='WT-USKIP' BEGIN SELECT RAISE(IGNORE); END");
    exec("CREATE TRIGGER u_fk AFTER UPDATE ON ton_invoices WHEN OLD.memo='WT-UFKEY' BEGIN INSERT INTO fk_child VALUES(3); END");
    for (const char* memo : {"WT-UBOOM", "WT-USKIP", "WT-UFKEY", "WT-UBUSY"}) invoice(memo, std::string("u-") + memo, 4990000, now());
    g_explorer = json{{"jetton_transfers", {transfer("WT-UBOOM", 4990000, "b1"), transfer("WT-USKIP", 4990000, "b2"),
                                            transfer("WT-UFKEY", 4990000, "b3")}}}.dump();
    g_sent.clear();
    pollUsdtPayments();
    for (const char* memo : {"WT-UBOOM", "WT-USKIP", "WT-UFKEY"})
        CHECK(text("SELECT status FROM ton_invoices WHERE memo='" + std::string(memo) + "'") == "active");
    CHECK(g_sent.size() == 2);  // UBOOM и UFKEY; «занят» (USKIP) — без сообщения
    exec("BEGIN");  // чужая транзакция — BEGIN IMMEDIATE не начнётся
    g_explorer = json{{"jetton_transfers", {transfer("WT-UBUSY", 4990000, "b4")}}}.dump();
    pollUsdtPayments();
    exec("ROLLBACK");
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-UBUSY'") == "active");

    // Отмена автопродления прошла в Telegram, а отметка в базе — нет: в журнал.
    exec("INSERT INTO premium_payments(chat_id, telegram_payment_charge_id, payload, amount, currency, paid_at, sub_until) "
         "VALUES('77','pp','premium_30_days',250,'XTR',1," + std::to_string(now() + 99) + ")");
    exec("CREATE TRIGGER p_boom BEFORE UPDATE ON premium_payments BEGIN SELECT RAISE(ABORT,'x'); END");
    g_sent.clear();
    CHECK(cancelStarSubscriptions("owner", false) == 1);
    CHECK(g_sent.back().first == "owner" && g_sent.back().second.find("Не вышло") == std::string::npos);

    // Ответ Telegram с «ok» не логическим — не успех.
    g_tgAnswer = R"({"ok":"true"})";
    handlePreCheckoutQuery(preCheckout("premium_30_days", 250, 1));
    g_tgAnswer = R"({"ok":true,"result":true})";
}

// Остальные развилки разбора — по одной на ветку.
static void edges() {
    freshDb();
    // Звёзды: испорчено одно поле — отказ; валюта не XTR при верной сумме — отказ.
    for (const json& bad : {json{{"currency", 5}, {"total_amount", 250}, {"invoice_payload", "premium_30_days"},
                                 {"telegram_payment_charge_id", "e1"}},
                            json{{"currency", "XTR"}, {"total_amount", 250}, {"invoice_payload", "premium_30_days"},
                                 {"telegram_payment_charge_id", 5}},
                            json{{"currency", "USD"}, {"total_amount", 250}, {"invoice_payload", "premium_30_days"},
                                 {"telegram_payment_charge_id", "e2"}}})
        CHECK(!handleSuccessfulPayment("e", bad));
    // Флаги продления: не продление и первое списание подписки — полное сообщение.
    g_sent.clear();
    json first = successful("premium_30_days", 250, "e3");
    first["is_recurring"] = true;
    first["is_first_recurring"] = true;
    json once = successful("premium_30_days", 250, "e4");
    once["is_recurring"] = false;
    CHECK(handleSuccessfulPayment("e5", first) && handleSuccessfulPayment("e6", once));
    CHECK(g_sent.size() == 2 && g_sent[0].second == g_sent[1].second);
    CHECK(g_sent[0].second.find(tr(Lang::RU, "payment_success_title")) == 0);
    // Подтверждение: покупатель — не объект, валюта — не строка.
    json q = preCheckout("premium_30_days", 250, 1);
    q["from"] = "42";
    handlePreCheckoutQuery(q);
    CHECK(lastAnswerOk());
    q = preCheckout("premium_30_days", 250, 1);
    q["currency"] = 5;
    handlePreCheckoutQuery(q);
    CHECK(!lastAnswerOk());
    // Кошелёк в окружении пустой — по умолчанию.
    setenv("TON_WALLET_ADDRESS", "", 1);  // NOLINT(concurrency-mt-unsafe)
    invoice("WT-EMPTY", "e7", 4990000, now());
    g_explorer = "{}";
    g_urls.clear();
    premium::setClock(&fakeNow);
    g_now = now() + 100000;  // давно не спрашивали
    exec("UPDATE ton_invoices SET created_at=" + std::to_string(g_now) + " WHERE memo='WT-EMPTY'");
    pollUsdtPayments();
    CHECK(g_urls.size() == 1 && g_urls[0].find("owner_address=" + WALLET_RAW) != std::string::npos);
    unsetenv("TON_WALLET_ADDRESS");  // NOLINT(concurrency-mt-unsafe)
    // Счёт без номера чата — не наш случай.
    invoice("WT-NOCHT", "", 4990000, g_now);
    g_explorer = json{{"jetton_transfers", {transfer("WT-NOCHT", 4990000, "n1")}}}.dump();
    pollUsdtPayments();
    CHECK(text("SELECT status FROM ton_invoices WHERE memo='WT-NOCHT'") == "active");
    // Повторные случаи — журнал молчит (второй круг тех же переводов).
    invoice("WT-OLD99", "o9", 4990000, g_now);
    invoice("WT-UND99", "u9", 4990000, g_now);
    invoice("WT-SRV99", "777", 4990000, g_now);
    invoice("WT-RE099", "r9", 4990000, g_now);
    invoice("WT-RE199", "r8", 4990000, g_now);
    g_explorer = json{{"jetton_transfers", {transfer("WT-OLD99", 4990000, "o", g_now - 10000), transfer("WT-UND99", 100, "u"),
                                            transfer("WT-UND99", 1000000, "u2"), transfer("WT-SRV99", 4990000, "s"),
                                            transfer("WT-RE099", 4990000, "same"), transfer("WT-RE199", 4990000, "same")}}}.dump();
    std::ostringstream err;
    std::streambuf* old = std::cerr.rdbuf(err.rdbuf());
    for (int i = 0; i < 3; ++i) {
        g_now += 20;
        pollUsdtPayments();
    }
    std::cerr.rdbuf(old);
    const std::string log = err.str();
    for (const char* once1 : {"сделан раньше счёта WT-OLD99", "недоплата по WT-UND99", "WT-SRV99 выставлен сервисному",
                              "уже оплатил другой счёт — WT-RE199"}) {
        const size_t at = log.find(once1);
        CHECK(at != std::string::npos && log.find(once1, at + 1) == std::string::npos);
    }
    premium::setClock(nullptr);
    // Сырой адрес: строчная буква после «f», знак ниже «0»; человеческий — знак после «z».
    CHECK(tonRawAddress("0:" + std::string(63, 'a') + "g").empty());
    CHECK(tonRawAddress("0:" + std::string(63, 'A') + "-").empty());
    CHECK(tonRawAddress(std::string(47, 'A') + "{").empty());
    // Дата UTC — как у gmtime, на любом времени, и в далёком будущем.
    std::mt19937_64 r(0xDA7E);
    for (int i = 0; i < 200000 / QUICK; ++i) {
        const long long t = static_cast<long long>(r() % 400'000'000'000ULL) - 100'000'000'000LL;
        const auto tt = static_cast<time_t>(t);
        struct tm g {};
        gmtime_r(&tt, &g);
        const UtcTime u = utcTime(t);
        CHECK(u.year == g.tm_year + 1900LL && u.month == g.tm_mon + 1 && u.day == g.tm_mday && u.hour == g.tm_hour &&
              u.minute == g.tm_min);
    }
    CHECK(utcDate(0) == "1970-01-01" && utcDateTime(-1) == "1969-12-31 23:59 UTC" && utcDate(951782400) == "2000-02-29");
    CHECK(utcDateTime(600) == "1970-01-01 00:10 UTC" && utcDateTime(540) == "1970-01-01 00:09 UTC");
    CHECK(utcDate(9 * 86400) == "1970-01-10" && utcDate(10 * 86400) == "1970-01-11");
    // Границы, на которые случайные даты почти не попадают: начало и конец
    // 400-летней эры (и в минус), переходы веков внутри эры — как у gmtime.
    for (long long k = -3; k <= 3; ++k)
        for (const long long doe : {0LL, 1LL, 1459LL, 1460LL, 36523LL, 36524LL, 36525LL, 73048LL, 73049LL, 109572LL,
                                    109573LL, 146095LL, 146096LL})
            for (const long long sec : {0LL, 86399LL}) {
                const long long t = (k * 146097 + doe - 719468) * 86400 + sec;
                const auto tt = static_cast<time_t>(t);
                struct tm g {};
                gmtime_r(&tt, &g);
                const UtcTime u = utcTime(t);
                CHECK(u.year == g.tm_year + 1900LL && u.month == g.tm_mon + 1 && u.day == g.tm_mday && u.hour == g.tm_hour);
            }
    // Далёкое будущее — без переполнения; сверено независимым расчётом на Python.
    CHECK(utcDateTime(LLONG_MAX - 86400) == "292277026596-12-03 15:30 UTC");
}

// Тексты оплаты есть на всех 16 языках, «{n}» — там, где подставляется срок.
// ASan у gcc и у clang объявляется по-разному.
#if defined(__SANITIZE_ADDRESS__)
#define PREMIUM_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PREMIUM_TEST_ASAN 1
#endif
#endif

// Нагрузка и память (как у модуля халвинга): 20 000 человек и тысячи проходов
// опроса с новыми переводами — время в пределах, а память, незакрытые
// запросы и «сказано один раз» не растут. Под ASan память не меряем: он
// держит освобождённое в карантине.
static long rssKb() {
    std::ifstream f("/proc/self/statm");
    long pages = 0;
    long rss = 0;
    f >> pages >> rss;
    return rss * (sysconf(_SC_PAGESIZE) / 1024);
}
static void load() {
    using clk = std::chrono::steady_clock;
    const auto ms = [](clk::time_point t0) { return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); };
    freshDb();
    premium::setClock(&fakeNow);
    g_now = 1'900'000'000;
    exec("BEGIN");
    std::vector<std::string> ids;
    for (int i = 0; i < 20000; ++i) {
        ids.push_back("L" + std::to_string(i));
        exec("INSERT INTO users(chat_id, created_at, is_premium, premium_expire) VALUES('" + ids.back() + "',1," +
             (i % 10 == 0 ? "1, 9999999999)" : "0, 0)"));
    }
    exec("COMMIT");
    auto t0 = clk::now();
    CHECK(premiumSubsetOf(ids).size() == 2000);
    CHECK(ms(t0) < 2000);
    t0 = clk::now();
    int n = 0;
    for (const auto& id : ids) n += isPremium(id) ? 1 : 0;
    CHECK(n == 2000 && ms(t0) < 4000);

    // 50 живых счетов, в каждом проходе — 50 новых переводов-недоплат.
    for (int i = 0; i < 50; ++i) invoice("WT-L" + std::to_string(1000 + i), "L" + std::to_string(i), 4990000, g_now);
    std::ostream sink(nullptr);
    std::streambuf* old = std::cerr.rdbuf(sink.rdbuf());
    const auto pass = [&](int round) {
        json list = json::array();
        for (int i = 0; i < 50; ++i)
            list.push_back(transfer("WT-L" + std::to_string(1000 + i), 1000000, "h" + std::to_string(round) + "-" + std::to_string(i)));
        g_explorer = json{{"jetton_transfers", list}}.dump();
        g_now += 20;
        pollUsdtPayments();
    };
    for (int r = 0; r < 200; ++r) pass(r);
    const long rss0 = rssKb();
    t0 = clk::now();
    const int passes = QUICK == 1 ? 2000 : 100;
    for (int r = 200; r < 200 + passes; ++r) pass(r);
    const double perPass = ms(t0) / passes;
    const long grown = rssKb() - rss0;
    std::cerr.rdbuf(old);
    CHECK(perPass < 50);
    CHECK(sqlite3_next_stmt(db, nullptr) == nullptr);
    CHECK(num("SELECT COUNT(*) FROM ton_invoices WHERE status='active'") == 50);
#ifdef PREMIUM_TEST_ASAN
    (void)grown;
#else
    if (QUICK == 1) CHECK(grown < 512);  // 100 000 новых хешей: ~64 КБ; без предела у Once — ~1,4 МБ
#endif
    premium::setClock(nullptr);
}

static void texts() {
    // Правила текстов проекта (как в приложении, tools/i18n.json): число и
    // слово после него не разрываются переносом — между ними неразрывный
    // пробел; в китайском и японском пробелов вокруг числа нет; во
    // французском перед «: ; ! ? »» — неразрывный пробел. И каждый язык —
    // свой перевод, а не молча подставленный английский.
    const std::string NBSP = "\xC2\xA0";
    const auto letterAfter = [](const std::string& s, size_t at) {
        return at < s.size() && (static_cast<unsigned char>(s[at]) >= 0x80 || std::isalpha(static_cast<unsigned char>(s[at])));
    };
    const char* withN[] = {"ton_paid_ok", "payment_renewed", "payment_success_duration"};
    const char* plain[] = {"intro_gone", "invoice_unknown_product", "payments_unavailable", "payment_success_title",
                           "payment_success_activated", "autorenew_off"};
    for (int l = 0; l <= static_cast<int>(Lang::JA); ++l) {
        const auto lang = static_cast<Lang>(l);
        const bool cjk = lang == Lang::ZH || lang == Lang::JA;
        std::vector<std::string> all;
        for (const char* key : withN) {
            const std::string s = tr(lang, key);
            CHECK(s != key);
            const size_t at = s.find("{n}");
            CHECK(at != std::string::npos);
            if (at == std::string::npos) continue;
            if (cjk) {
                CHECK(at == 0 || (s[at - 1] != ' ' && s.compare(at - 2, 2, NBSP) != 0));
                CHECK(s.compare(at + 3, 1, " ") != 0 && s.compare(at + 3, 2, NBSP) != 0);
            } else if (s.compare(at + 3, 1, " ") == 0) {
                CHECK(!letterAfter(s, at + 4));  // «{n} дней» — только через неразрывный
            }
            all.push_back(s);
        }
        for (const char* key : plain) {
            CHECK(tr(lang, key) != key && !tr(lang, key).empty());
            all.push_back(tr(lang, key));
        }
        if (lang != Lang::EN) {
            size_t k = 0;
            for (const char* key : withN) CHECK(all[k++] != tr(Lang::EN, key));
            for (const char* key : plain) CHECK(all[k++] != tr(Lang::EN, key));
        }
        if (lang == Lang::FR)
            for (const auto& s : all)
                for (size_t i = 1; i < s.size(); ++i)
                    if (std::string(":;!?").find(s[i]) != std::string::npos) CHECK(s[i - 1] != ' ');
    }
}

int main() {
    unsetenv("TON_WALLET_ADDRESS");  // кошелёк по умолчанию — тот же, что у API
    rules();
    fuzz();
    schema();
    usdt();
    stars();
    grantAndExpire();
    autorenew();
    texts();
    fixtures();
    contract();
    grantByOwner();
    attacks();
    slowPoll();
    failures();
    clockEdges();
    logging();
    faults();
    edges();
    load();
    closeDb();
    if (g_fail) {
        std::cerr << "premium: ПРОВАЛОВ " << g_fail << std::endl;
        return 1;
    }
    std::cout << "premium: ok" << std::endl;
    return 0;
}
