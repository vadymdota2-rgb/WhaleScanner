#include "autobase.h"

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

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

}  // namespace

void initAutobase() {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    for (int i = 0; i < NETS; i++) {
        if (prepareOrLog(db, &s, "SELECT value FROM state WHERE key=?")) {
            sqlite3_bind_text(s, 1, KEY[i], -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) g_on[i].store(safeColumnText(s, 0) != "0");
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

int autoLimit(AutoNet n) { return limits()[static_cast<int>(n)]; }

double autoMinUsd(AutoNet n) { return minUsd()[static_cast<int>(n)]; }

void autobaseCommand(const std::string& owner, const std::string& arg) {
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
    std::ostringstream t;
    t << "🐋 <b>Поиск новых китов</b>\n";
    for (int i = 0; i < NETS; i++) {
        const AutoNet n = static_cast<AutoNet>(i);
        t << "\n" << (g_on[i] ? "🟢 " : "⚪️ ") << "<b>" << NAME[i] << "</b> — " << (g_on[i] ? "ищет" : "остановлен")
          << " · сегодня " << autoToday(n) << "/" << limits()[i] << " · порог ";
        if (n == AutoNet::BTC) {
            std::ostringstream b;
            b << btcAutoMinBtc();
            t << b.str() << " BTC";
        } else {
            t << money(minUsd()[i]);
        }
        // Сегодняшнее число берётся из базы при запуске: в нём и то, что
        // добавили до лимита (версии без лимита), поэтому бывает больше.
        if (autoToday(n) >= limits()[i]) t << "\n   лимит на сегодня выбран, новые — с 00:00 UTC";
        if (n == AutoNet::HL && g_on[i]) t << "\n   " << hlAutoStatus();
    }
    t << "\n\nВыключить: <code>/autobase bsc off</code> (или <code>hl</code>, <code>btc</code>, <code>all</code>)"
         "\nВключить: <code>/autobase bsc on</code>"
         "\nУже найденные кошельки остаются в базе; на BSC и Hyperliquid те, кто месяц не торговал, убираются сами.";
    sendMsg(owner, t.str());
}
