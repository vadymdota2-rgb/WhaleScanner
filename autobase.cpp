#include "autobase.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <sqlite3.h>

#include "telegram.h"
#include "hyperliquid.h"
#include "utils.h"

extern sqlite3* db;
extern std::mutex dbMutex;
extern const std::string SERVICE_CHAT_ID;

namespace {

constexpr int NETS = 3;
const char* const KEY[NETS] = {"auto_bsc", "auto_hl", "auto_btc"};
const char* const NAME[NETS] = {"BSC", "Hyperliquid", "Bitcoin"};
const char* const ARG[NETS] = {"bsc", "hl", "btc"};

int envInt(const char* key, int def) {
    const char* v = std::getenv(key);
    const int n = (v && *v) ? std::atoi(v) : def;
    return n > 0 ? n : def;
}
double envUsd(const char* key, double def) {
    const char* v = std::getenv(key);
    const double n = (v && *v) ? std::atof(v) : def;
    return n > 0 ? n : def;
}

// Лимиты и пороги — внутри функций, а не глобальными массивами: их читают
// глобальные константы других файлов (HL_AUTO_MIN_NANOS в
// hyperliquid_core.cpp) ещё до main, а порядок инициализации глобальных
// переменных между файлами не определён — массив мог оказаться нулями.
// Статическая переменная функции заполняется при первом вызове.
const int* limits() {
    static const int v[NETS] = {envInt("WHALE_BSC_AUTO_DAILY", 500), envInt("WHALE_HL_AUTO_DAILY", 500),
                                envInt("WHALE_BTC_AUTO_DAILY", 500)};
    return v;
}
const double* minUsd() {
    static const double v[NETS] = {envUsd("WHALE_BSC_AUTO_MIN", 10000.0), envUsd("WHALE_HL_AUTO_MIN", 10000.0), 0};
    return v;
}

std::atomic<bool> g_on[NETS] = {true, true, true};
std::mutex g_mx;

// Счётчик событий «сегодня (UTC) и всего». В state хранится как
// "<день>:<сегодня>:<всего>", поэтому переживает перезапуск.
struct Tally {
    const char* key[NETS];
    long long day[NETS] = {-1, -1, -1};
    long long today[NETS] = {0, 0, 0};
    long long total[NETS] = {0, 0, 0};
};
Tally g_pruned{{"pruned_bsc", "pruned_hl", "pruned_btc"}};  // убраны за бездействие
Tally g_banned{{"banned_bsc", "banned_hl", "banned_btc"}};  // забанены как боты
long long g_day = -1;
int g_today[NETS] = {0, 0, 0};

long long utcDay() { return static_cast<long long>(time(nullptr)) / 86400; }

// Новые сутки — счёт с нуля. Зовётся под g_mx.
void rollDay() {
    const long long d = utcDay();
    if (d != g_day) {
        g_day = d;
        for (int& t : g_today) t = 0;
    }
}

void saveFlag(int i) {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (prepareOrLog(db, &s, "INSERT OR REPLACE INTO state(key, value) VALUES(?, ?)")) {
        sqlite3_bind_text(s, 1, KEY[i], -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, g_on[i].load() ? "1" : "0", -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

std::string money(double usd) {
    std::ostringstream o;
    if (usd >= 1e6) o << "$" << std::setprecision(3) << usd / 1e6 << "M";
    else if (usd >= 1e3) o << "$" << std::setprecision(3) << usd / 1e3 << "k";
    else o << "$" << usd;
    return o.str();
}


std::string num(long long n) {
    std::string d = std::to_string(n < 0 ? -n : n), out = n < 0 ? "-" : "";
    for (size_t i = 0; i < d.size(); i++) {
        if (i && (d.size() - i) % 3 == 0) out += ' ';
        out += d[i];
    }
    return out;
}

// Длина в знаках, а не байтах: кириллица в UTF-8 — два байта на букву.
size_t chars(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// Строка таблицы: подпись слева (9 знаков) и три числа справа (по 8).
std::string row(const std::string& label, const std::string& a, const std::string& b, const std::string& c) {
    std::string out = label;
    out.append(chars(label) < 9 ? 9 - chars(label) : 0, ' ');
    for (const std::string* v : {&a, &b, &c}) {
        out.append(chars(*v) < 8 ? 8 - chars(*v) : 1, ' ');
        out += *v;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
}  // namespace

void initAutobase() {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS auto_woke (net TEXT NOT NULL, address TEXT NOT NULL, "
                 "at INTEGER NOT NULL, PRIMARY KEY (net, address))",
                 nullptr, nullptr, nullptr);
    for (int i = 0; i < NETS; i++) {
        if (prepareOrLog(db, &s, "SELECT value FROM state WHERE key=?")) {
            sqlite3_bind_text(s, 1, KEY[i], -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) g_on[i].store(safeColumnText(s, 0) != "0");
            sqlite3_finalize(s);
        }
    }
    for (Tally* t : {&g_pruned, &g_banned}) {
        for (int i = 0; i < NETS; i++) {
            if (!prepareOrLog(db, &s, "SELECT value FROM state WHERE key=?")) continue;
            sqlite3_bind_text(s, 1, t->key[i], -1, SQLITE_STATIC);
            long long d = -1, today = 0, all = 0;
            if (sqlite3_step(s) == SQLITE_ROW &&
                std::sscanf(safeColumnText(s, 0).c_str(), "%lld:%lld:%lld", &d, &today, &all) == 3) {
                t->day[i] = d;
                t->today[i] = today;
                t->total[i] = all;
            }
            sqlite3_finalize(s);
        }
    }
    std::lock_guard<std::mutex> m(g_mx);
    rollDay();
    const long long dayStart = g_day * 86400;
    if (prepareOrLog(db, &s, "SELECT SUM(label='auto-bsc'), SUM(label='auto-hl') FROM user_whales "
                             "WHERE user_id=? AND created_at>=?")) {
        sqlite3_bind_text(s, 1, SERVICE_CHAT_ID.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, dayStart);
        if (sqlite3_step(s) == SQLITE_ROW) {
            g_today[0] = sqlite3_column_int(s, 0);
            g_today[1] = sqlite3_column_int(s, 1);
        }
        sqlite3_finalize(s);
    }
    std::cout << "[AUTO] поиск китов: BSC " << (g_on[0] ? "вкл" : "выкл") << ", Hyperliquid "
              << (g_on[1] ? "вкл" : "выкл") << ", Bitcoin " << (g_on[2] ? "вкл" : "выкл") << std::endl;
}

void autoSeed(AutoNet n, int addedToday) {
    std::lock_guard<std::mutex> m(g_mx);
    rollDay();
    g_today[static_cast<int>(n)] = addedToday;
}

bool autoEnabled(AutoNet n) { return g_on[static_cast<int>(n)].load(std::memory_order_relaxed); }

bool autoRoom(AutoNet n) {
    const int i = static_cast<int>(n);
    if (!g_on[i].load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> m(g_mx);
    rollDay();
    return g_today[i] < limits()[i];
}

void autoCounted(AutoNet n) {
    std::lock_guard<std::mutex> m(g_mx);
    rollDay();
    ++g_today[static_cast<int>(n)];
}

int autoToday(AutoNet n) {
    std::lock_guard<std::mutex> m(g_mx);
    rollDay();
    return g_today[static_cast<int>(n)];
}

namespace {
// Не под dbMutex: пишет в state сама.
void bump(Tally& t, AutoNet n, int count) {
    if (count <= 0) return;
    const int i = static_cast<int>(n);
    std::string v;
    {
        std::lock_guard<std::mutex> m(g_mx);
        const long long d = utcDay();
        if (t.day[i] != d) { t.day[i] = d; t.today[i] = 0; }
        t.today[i] += count;
        t.total[i] += count;
        v = std::to_string(t.day[i]) + ":" + std::to_string(t.today[i]) + ":" + std::to_string(t.total[i]);
    }
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (prepareOrLog(db, &s, "INSERT OR REPLACE INTO state(key, value) VALUES(?, ?)")) {
        sqlite3_bind_text(s, 1, t.key[i], -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, v.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

// Сегодня и всего — под g_mx.
std::pair<long long, long long> tallyOf(const Tally& t, int i) {
    return {t.day[i] == utcDay() ? t.today[i] : 0, t.total[i]};
}
}  // namespace

void autoPruned(AutoNet n, int count) { bump(g_pruned, n, count); }

void autoBanned(AutoNet n, int count) { bump(g_banned, n, count); }

void autoWoke(AutoNet n, const std::string& addr) {
    // До пересборки списка (раз в час) проснувшийся на BSC ещё числится
    // спящим, и каждая его сделка зовёт сюда — в базу идём раз в 30 дней.
    constexpr long long COLD_SEC = 30LL * 86400LL;  // как AUTO_IDLE_SEC в main.cpp
    const long long now = static_cast<long long>(time(nullptr));
    static std::mutex mx;
    static std::unordered_map<std::string, long long> seen;
    {
        std::lock_guard<std::mutex> m(mx);
        long long& at = seen[ARG[static_cast<int>(n)] + std::string(":") + addr];
        if (at > now - COLD_SEC) return;
        at = now;
    }
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    bool cold = false;
    if (prepareOrLog(db, &s, "SELECT 1 FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id "
                             "WHERE uw.user_id=? AND wa.address=? AND uw.created_at>0 AND uw.created_at<? LIMIT 1")) {
        sqlite3_bind_text(s, 1, SERVICE_CHAT_ID.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, addr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, now - COLD_SEC);
        cold = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
    }
    if (!cold) return;
    if (prepareOrLog(db, &s, "INSERT INTO auto_woke(net, address, at) VALUES(?,?,?) "
                             "ON CONFLICT(net, address) DO UPDATE SET at=excluded.at "
                             "WHERE auto_woke.at < excluded.at - ?")) {
        sqlite3_bind_text(s, 1, ARG[static_cast<int>(n)], -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, addr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, now);
        sqlite3_bind_int64(s, 4, COLD_SEC);
        if (sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) > 0)
            std::cout << "[AUTO] спящий кошелёк проснулся (" << NAME[static_cast<int>(n)] << "): " << addr << std::endl;
        sqlite3_finalize(s);
    }
}

int autoLimit(AutoNet n) { return limits()[static_cast<int>(n)]; }

double autoMinUsd(AutoNet n) { return minUsd()[static_cast<int>(n)]; }

void autobaseCommand(const std::string& owner, const std::string& arg) {
    // Сейчас в базе: найденные поиском и ещё не убранные. Всего добавлено =
    // в базе + удалено + забанено (повторно найденный считается дважды).
    long long inBase[NETS] = {0, 0, btcAutoCount()};
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (prepareOrLog(db, &s, "SELECT SUM(label='auto-bsc'), SUM(label='auto-hl') FROM user_whales WHERE user_id=?")) {
            sqlite3_bind_text(s, 1, SERVICE_CHAT_ID.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) == SQLITE_ROW) {
                inBase[0] = sqlite3_column_int64(s, 0);
                inBase[1] = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
    }
    std::istringstream in(arg);
    std::string net, act;
    in >> net >> act;
    if (!net.empty()) {
        const bool on = act == "on", off = act == "off";
        int idx = -2;
        if (net == "all") idx = -1;
        for (int i = 0; i < NETS; i++) if (net == ARG[i]) idx = i;
        if (idx == -2 || (!on && !off)) {
            sendMsg(owner, "Использование: <code>/autobase</code> — состояние\n"
                           "<code>/autobase bsc off</code> · <code>/autobase hl on</code> · "
                           "<code>/autobase btc off</code> · <code>/autobase all on</code>");
            return;
        }
        for (int i = 0; i < NETS; i++) {
            if (idx != -1 && idx != i) continue;
            g_on[i].store(on);
            saveFlag(i);
        }
    }
    // Числа — таблицами в <pre>: моноширинный шрифт держит столбцы ровно.
    // Ширина строки 33 знака — влезает в экран телефона без переноса.
    std::pair<long long, long long> pr[NETS], bn[NETS];
    {
        std::lock_guard<std::mutex> m(g_mx);
        for (int i = 0; i < NETS; i++) {
            pr[i] = tallyOf(g_pruned, i);
            bn[i] = tallyOf(g_banned, i);
        }
    }
    const ServiceBaseStats sb = serviceBaseStats();

    std::ostringstream t;
    t << "🐋 <b>Автопоиск китов</b>\n";
    for (int i = 0; i < NETS; i++) {
        const AutoNet n = static_cast<AutoNet>(i);
        t << "\n" << (g_on[i] ? "🟢 " : "⚪️ ") << "<b>" << NAME[i] << "</b> — " << (g_on[i] ? "ищет" : "остановлен")
          << ", от ";
        if (n == AutoNet::BTC) {
            std::ostringstream b;
            b << btcAutoMinBtc();
            t << b.str() << " BTC";
        } else {
            t << money(minUsd()[i]);
        }
    }

    auto today = [&](int i) {
        return std::to_string(autoToday(static_cast<AutoNet>(i))) + "/" + std::to_string(limits()[i]);
    };
    t << "\n\n<b>Найдено поиском</b>\n<pre>" << row("", "BSC", "HL", "BTC") << "\n" << row("Сегодня", "", "", "")
      << "\n" << row(" найдено", today(0), today(1), today(2))
      << "\n" << row(" удалено", "—", "—", num(pr[2].first))
      << "\n" << row(" бан", num(bn[0].first), num(bn[1].first), num(bn[2].first))
      << "\n" << row("Всего", "", "", "");
    std::string found[NETS], pruned[NETS], banned[NETS], base[NETS];
    for (int i = 0; i < NETS; i++) {
        found[i] = num(inBase[i] + pr[i].second + bn[i].second);
        // BSC и Hyperliquid за бездействие не удаляют — усыпляют.
        pruned[i] = i == 2 ? num(pr[i].second) : "—";
        banned[i] = num(bn[i].second);
        base[i] = num(inBase[i]);
    }
    t << "\n" << row(" найдено", found[0], found[1], found[2]) << "\n" << row(" удалено", pruned[0], pruned[1], pruned[2])
      << "\n" << row(" бан", banned[0], banned[1], banned[2]) << "\n" << row(" в базе", base[0], base[1], base[2])
      << "</pre>";
    // Сегодняшнее число берётся из базы при запуске: в нём и то, что
    // добавили до лимита (версии без лимита), поэтому бывает больше.
    std::string full;
    for (int i = 0; i < NETS; i++)
        if (autoToday(static_cast<AutoNet>(i)) >= limits()[i]) full += std::string(full.empty() ? "" : ", ") + NAME[i];
    if (!full.empty()) t << "\n⛔ Лимит на сегодня выбран: " << full << ". Новые — с 00:00 UTC.";
    if (g_on[1]) t << "\n📈 Hyperliquid: " << hlAutoStatus() << ".";

    t << "\n\n📦 <b>Вся база сервисного аккаунта</b>\n<pre>" << row("", "BSC", "HL", "BTC")
      << "\n" << row("Кошельков", num(sb.evm), num(sb.evm), num(sb.btc))
      << "\n" << row(" импорт", num(sb.evm - sb.evmAuto), num(sb.evm - sb.evmAuto), num(sb.btc - sb.btcAuto))
      << "\n" << row(" поиском", num(sb.evmAuto), num(sb.evmAuto), num(sb.btcAuto))
      << "\n" << row("Спят", num(sb.bscSleep), num(sb.hlSleep), "—")
      << "\n" << row("Проснулись", "", "", "")
      << "\n" << row(" сегодня", num(sb.wokeToday[0]), num(sb.wokeToday[1]), "—")
      << "\n" << row(" всего", num(sb.wokeAll[0]), num(sb.wokeAll[1]), "—") << "</pre>"
      << "\n😴 Спят на обеих сетях: <b>" << num(sb.bothSleep) << "</b> из " << num(sb.evm)
      << "\nИтого кошельков: <b>" << num(sb.evm + sb.btc) << "</b>"
      << "\n<i>Адрес 0x один на BSC и Hyperliquid — в итог входит один раз.</i>";

    t << "\n\nℹ️ <b>Что значат строки</b>"
         "\n• <b>спят</b> — 30 дней без сделок в этой сети. Кошелёк не удаляется и просыпается "
         "на первой же сделке. Спит отдельно в каждой сети: торгует на BSC, но не на Hyperliquid — "
         "спит только на Hyperliquid. Касается и импорта, и найденных поиском."
         "\n• <b>проснулись</b> — спавшие, которые снова торгуют. На BSC будит и крупный вывод "
         "с биржи себе (от $10k): кит готовится покупать."
         "\n• <b>удалено</b> — только Bitcoin: найденный поиском кошелёк месяц без движений и меньше 0,2 BTC. "
         "Без бана — снова крупно выведет с биржи, найдётся снова."
         "\n• <b>бан</b> — навсегда: боты на BSC и Hyperliquid, сервисы и биржи в Bitcoin (1000+ транзакций)."
         "\n\n⚙️ <b>Управление</b>"
         "\n<code>/autobase bsc off</code> — остановить (или <code>hl</code>, <code>btc</code>, <code>all</code>)"
         "\n<code>/autobase bsc on</code> — включить";
    sendMsg(owner, t.str());
}
