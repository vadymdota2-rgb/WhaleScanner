#include "hyperliquid.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <curl/websockets.h>

#include "json.hpp"
#include "utils.h"
#include "ru.h"
#include "message_queue.h"
#include "premium.h"        // isPremium, premiumTopTradersLimit
#include "alert_settings.h" // replyInPlace

using json = nlohmann::json;

// --- Сервисы из main.cpp / rpc_client ---
extern std::atomic<bool> running;
extern const std::string SERVICE_CHAT_ID;
std::string http(const std::string& url, const std::string& post, int timeout);
void logCritical(const std::string& msg);
std::string getUserLanguage(const std::string& chatId);
void rememberView(const std::string& chatId, const std::string& data);
// Из wallet_menu.cpp - тот же формат адреса, что на остальных экранах.
std::string shortAddress(const std::string& a);

namespace {

// ============================== Константы ==============================

const char* const HL_WS_URL   = "wss://api.hyperliquid.xyz/ws";
const char* const HL_INFO_URL = "https://api.hyperliquid.xyz/info";

// Площадка закрывает молчащее соединение, а libcurl сам служебных пингов не
// шлёт (он лишь отвечает на чужие). Поэтому пинг прикладного уровня свой.
constexpr long long HL_PING_INTERVAL_SEC = 50;

// Лимит площадки - 2000 сообщений в минуту на все соединения. Подписка на
// каждый рынок отправляется отдельным сообщением (пакетных площадка не
// принимает), поэтому при ~250 рынках их надо разложить во времени.
constexpr int HL_SUBSCRIBE_PACE_MS = 50;

// Список рынков меняется на ходу: HIP-3 позволяет заводить новые без участия
// площадки, поэтому статический перечень протухнет.
constexpr long long HL_COINS_REFRESH_SEC = 3600;
constexpr long long HL_WALLETS_REFRESH_SEC = 60;

// Обрывы бывают без уведомления: сокет открыт, а данные не идут.
constexpr long long HL_SILENCE_TIMEOUT_SEC = 120;
constexpr int HL_RECONNECT_MIN_SEC = 1;
constexpr int HL_RECONNECT_MAX_SEC = 60;

// Бюджет REST: площадка даёт 1200 весов в минуту на IP. Держимся ниже потолка,
// чтобы случайный всплеск не выбил нас из лимита целиком.
// Потолок площадки - 1200 весов в минуту. Держим 1150: запаса меньше, чем
// раньше, но всплески гасит очередь, а не резерв - непрошедшая дозагрузка
// возвращается в очередь и разбирается следующей минутой.
constexpr int HL_BUDGET_PER_MINUTE = 1150;
constexpr int HL_WEIGHT_USER_FILLS = 20;
constexpr int HL_WEIGHT_CLEARINGHOUSE = 2;
constexpr int HL_WEIGHT_META = 20;

// Кит в разгоне торгует пачками. Дозагружать на каждую сделку незачем: один
// запрос отдаёт диапазон, поэтому ждём паузу и забираем всё скопом.
//
// Поднято с 30 до 60 секунд: при 1800 кошельках дозагрузок стало 43 в минуту
// при потолке 45, и бюджет начал кончаться. Удвоение паузы вдвое укрупняет
// пачки и вдвое сокращает запросы. Цена - худшая задержка алерта растёт
// примерно с 33 секунд до минуты; типичная не меняется, она зависит от того,
// торговал ли кит только что, а не от длины паузы.
constexpr long long HL_ENRICH_DEBOUNCE_SEC = 60;

// При первой встрече кошелька дозагрузка вернёт его недавнюю историю. Слать по
// ней алерты нельзя - человек получил бы пачку сообщений о сделках, которые
// давно закрыты. Поэтому первый проход только наполняет базу.
constexpr long long HL_SEED_LOOKBACK_MS = 3600LL * 1000LL;

// Защита от лавины: у гиперактивного адреса (маркет-мейкер, HFT-бот) сделок
// сотни в минуту, и он один съел бы весь бюджет. Такие переводятся на редкий
// опрос.
constexpr int HL_HYPERACTIVE_FILLS = 200;
constexpr long long HL_HYPERACTIVE_DEBOUNCE_SEC = 600;

constexpr long long NANOS_PER_UNIT = 1000000000LL;

// Срок хранения сделок. Рейтинг считает за 30 дней, остальное - мёртвый груз.
constexpr long long HL_FILL_TTL_SEC = 45LL * 86400LL;
constexpr long long HL_CLEANUP_INTERVAL_SEC = 3600;
constexpr size_t HL_POSITION_CACHE_CAP = 20000;

constexpr long long HL_RANK_WINDOW_SEC = 30LL * 86400LL;

// Порог детектора ботов: столько ОРДЕРОВ за 30 дней ни один человек не делает.
// Такой кошелёк - маркет-мейкер или алгоритм: в рейтинге он бессмыслен (его
// прибыль не воспроизводима), а бюджет дозагрузки съедает весь.
//
// Значение то же, что у спотового детектора: разводить пороги по площадкам
// смысла нет, одинаковые проще держать в голове. Тысяча ордеров за месяц -
// это тридцать три в день каждый день без выходных.
constexpr int HL_MAX_BOT_TRADES = 1000;

// Кэш рейтинга объявлен здесь, а не рядом с рейтингом: бан обнуляет его
// отметку, чтобы исключённый кошелёк исчез из топа сразу, а не через 5 минут.
std::mutex g_rankMutex;
long long g_rankBuiltAt = 0;

// Тот же разделитель, что в спотовом рейтинге: экраны стоят рядом в одном
// меню, и разная ширина линии сразу бросалась бы в глаза.
const char* const HL_CARD_SEPARATOR = "\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501";

// ============================== Своя база ==============================

sqlite3* g_hlDb = nullptr;
std::mutex g_hlDbMutex;

std::string hlDbFile() {
    const char* p = std::getenv("WHALE_HL_DB_FILE");
    return (p && *p) ? std::string(p) : std::string("hyperliquid.db");
}

long long nowSec() { return static_cast<long long>(std::time(nullptr)); }
long long nowMs()  { return nowSec() * 1000LL; }

// ============================ Разбор чисел ============================
// Площадка отдаёт все числа строками ("18.435"). Переводим в наносы целыми
// числами: доллары через double дают расхождения в копейках, а они потом
// всплывают в рейтинге как необъяснимая разница.

bool parseDecimalToNanos(const std::string& s, long long& out) {
    out = 0;
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[i] == '-') { neg = true; i++; }
    else if (s[i] == '+') i++;

    long long intPart = 0;
    size_t intDigits = 0;
    // Предел целой части: LLONG_MAX / 1e9, потому что дальше идёт умножение на
    // наносы. Проверять ОБЯЗАТЕЛЬНО после присоединения цифры, а не до: иначе
    // последняя цифра успевает вытолкнуть число за границу уже после проверки,
    // и получается не отказ, а тихая порча - отрицательный размер сделки или
    // отрицательный PnL из ниоткуда.
    constexpr long long MAX_WHOLE_UNITS = 9223372036LL;   // LLONG_MAX / NANOS_PER_UNIT
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
        intPart = intPart * 10 + (s[i] - '0');
        if (intPart > MAX_WHOLE_UNITS) return false;
        intDigits++;
    }
    if (intDigits == 0) return false;

    long long frac = 0;
    int fracDigits = 0;
    if (i < s.size() && s[i] == '.') {
        i++;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
            if (fracDigits < 9) { frac = frac * 10 + (s[i] - '0'); fracDigits++; }
        }
    }
    if (i != s.size()) return false;
    while (fracDigits < 9) { frac *= 10; fracDigits++; }

    out = intPart * NANOS_PER_UNIT + frac;
    if (neg) out = -out;
    return true;
}

// Цена × размер. Оба уже в наносах, поэтому произведение - в наносах в
// квадрате; делим обратно. 128 бит здесь не роскошь: цена 100 000 и размер
// миллион дают 1e29, что знаковое 64-битное переполняет.
bool notionalNanos(const std::string& pxStr, const std::string& szStr, long long& out) {
    long long px = 0, sz = 0;
    if (!parseDecimalToNanos(pxStr, px) || !parseDecimalToNanos(szStr, sz)) return false;
    if (px < 0) px = -px;
    if (sz < 0) sz = -sz;
    __int128 product = static_cast<__int128>(px) * static_cast<__int128>(sz);
    __int128 result = product / NANOS_PER_UNIT;
    if (result > static_cast<__int128>(9000000000000000000LL)) return false;
    out = static_cast<long long>(result);
    return true;
}

std::string fmtUsd(long long nanos) { return formatUsdNanosSigned(nanos, false); }

// Количество монет. В сериях размеры складываются, поэтому печатать строку из
// ответа площадки уже нельзя - число собирается у нас. Хвостовые нули убираем:
// "3100" читается лучше, чем "3100.000000000".
std::string formatQtyNanos(long long nanos) {
    if (nanos < 0) nanos = -nanos;
    const long long whole = nanos / NANOS_PER_UNIT;
    long long frac = nanos % NANOS_PER_UNIT;
    std::string out = formatThousands(static_cast<uint64_t>(whole));
    if (frac == 0) return out;
    std::string f = std::to_string(frac);
    f.insert(0, 9 - f.size(), '0');
    while (!f.empty() && f.back() == '0') f.pop_back();
    return out + "." + f;
}

// Метка направления письма. Telegram выбирает направление КАЖДОЙ строки по её
// первому буквенному символу: строка с арабской подписью встаёт справа, а
// строка, начинающаяся с "PnL" или с адреса 0x..., - слева. В арабском
// интерфейсе соседние строки из-за этого разъезжаются.
//
// U+200F - нулевой ширины символ, сам по себе невидимый, но "сильный" справа
// налево. Поставленный первым, он задаёт направление всей строки явно.
// Для остальных языков возвращается пустая строка, и ничего не меняется.
const char* dirMark(Lang lang) {
    return lang == Lang::AR ? "\u200F" : "";
}


// Цена требует своей точности. Общий форматтер округляет до центов, и монета
// за $0.014682 превращается в "$0.01" - число, по которому ничего не сходится:
// умножишь на количество и получишь совсем не тот размер сделки, что строкой
// выше. Знаков после запятой берём тем больше, чем дешевле монета.
std::string formatPriceNanos(long long nanos) {
    const bool neg = nanos < 0;
    if (neg) nanos = -nanos;

    int digits;
    if (nanos >= 1000LL * NANOS_PER_UNIT)      digits = 2;   // от $1000
    else if (nanos >= NANOS_PER_UNIT)          digits = 4;   // от $1
    else if (nanos >= NANOS_PER_UNIT / 100)    digits = 6;   // от $0.01
    else                                       digits = 9;   // дешевле цента

    long long scale = 1;
    for (int i = 0; i < 9 - digits; i++) scale *= 10;
    long long units = nanos / NANOS_PER_UNIT;
    long long frac = (nanos % NANOS_PER_UNIT + scale / 2) / scale;

    long long fracLimit = 1;
    for (int i = 0; i < digits; i++) fracLimit *= 10;
    if (frac >= fracLimit) { units++; frac = 0; }            // округление вверх через разряд

    std::string fracStr = std::to_string(frac);
    fracStr.insert(0, static_cast<size_t>(digits) - fracStr.size(), '0');
    // Хвостовые нули только мешают, но два знака оставляем всегда - иначе
    // ровные цены выглядели бы как целые числа без копеек.
    while (fracStr.size() > 2 && fracStr.back() == '0') fracStr.pop_back();

    return std::string(neg ? "-$" : "$") +
           formatThousands(static_cast<uint64_t>(units)) + "." + fracStr;
}

// ========================= Кэш отслеживаемых адресов =========================
// Фид отдаёт ВСЕ сделки площадки, а нужны единицы. Сверка идёт на каждую
// сделку, поэтому список держим своей копией в памяти, а не берём блокировку
// главной базы тысячи раз в минуту.

// Список перп-рынков. Нужен не только для подписки: дозагрузка обязана по
// нему отсеивать спотовые сделки - userFillsByTime отдаёт ВСЕ сделки кошелька
// на площадке, а у спота нет ни плеча, ни маржи, ни ликвидации, и в перповом
// рейтинге такие строки дают ROI 0% и пустое плечо.
std::mutex g_coinsMutex;
std::set<std::string> g_perpCoins;

bool perpCoinsLoaded() {
    std::lock_guard<std::mutex> l(g_coinsMutex);
    return !g_perpCoins.empty();
}

bool isPerpCoin(const std::string& coin) {
    std::lock_guard<std::mutex> l(g_coinsMutex);
    if (g_perpCoins.empty()) return true;   // список ещё не загружен - не теряем данные
    return g_perpCoins.count(coin) > 0;
}

void setPerpCoins(const std::vector<std::string>& coins) {
    std::set<std::string> fresh(coins.begin(), coins.end());
    std::lock_guard<std::mutex> l(g_coinsMutex);
    g_perpCoins.swap(fresh);
}

// Списки отдаём снимком-указателем, а не копией, по образцу WATCHERS_PTR в
// main.cpp. Разница принципиальная: фид разбирает тысячи сделок в минуту, и
// копирование множества на каждое сообщение - это по паре тысяч выделений
// памяти впустую. Указатель копируется за наносекунды, а сам набор живёт,
// пока его кто-то держит.
using AddressSet = std::set<std::string>;
std::mutex g_walletsMutex;
std::shared_ptr<const AddressSet> g_watched = std::make_shared<const AddressSet>();
std::shared_ptr<const AddressSet> g_banned  = std::make_shared<const AddressSet>();

std::shared_ptr<const AddressSet> watchedSnapshot() {
    std::lock_guard<std::mutex> l(g_walletsMutex);
    return g_watched;
}
std::shared_ptr<const AddressSet> bannedSnapshot() {
    std::lock_guard<std::mutex> l(g_walletsMutex);
    return g_banned;
}

bool isBanned(const std::string& addr) {
    return bannedSnapshot()->count(addr) > 0;
}

std::set<std::string> loadBannedWallets() {
    std::set<std::string> out;
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return out;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s, "SELECT wallet FROM hl_banned")) return out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string w = safeColumnText(s, 0);
        if (!w.empty()) out.insert(w);
    }
    sqlite3_finalize(s);
    return out;
}

void reloadWatchedWallets() {
    std::vector<std::string> src = hlWatchedAddresses();
    std::set<std::string> fresh(src.begin(), src.end());
    std::set<std::string> banned = loadBannedWallets();   // до захвата, иначе
                                                          // два мьютекса разом
    auto freshPtr  = std::make_shared<const AddressSet>(std::move(fresh));
    auto bannedPtr = std::make_shared<const AddressSet>(std::move(banned));
    std::lock_guard<std::mutex> l(g_walletsMutex);
    g_watched = freshPtr;
    g_banned = bannedPtr;
}

size_t watchedCount() { return watchedSnapshot()->size(); }

// ============================== Счётчики ==============================

std::atomic<unsigned long long> g_tradesSeen{0};
std::atomic<unsigned long long> g_hits{0};
std::atomic<unsigned long long> g_enriched{0};
std::atomic<unsigned long long> g_alertsSent{0};
std::atomic<unsigned long long> g_budgetSkips{0};
std::atomic<unsigned long long> g_reconnects{0};
std::atomic<unsigned long long> g_botsBanned{0};
std::atomic<int> g_subscribedCoins{0};
std::atomic<long long> g_lastMsgTs{0};
std::atomic<bool> g_connected{false};

// ========================= Бюджет запросов к REST =========================

std::mutex g_budgetMutex;
long long g_budgetWindow = 0;
int g_budgetSpent = 0;

bool spendBudget(int weight) {
    std::lock_guard<std::mutex> l(g_budgetMutex);
    long long minute = nowSec() / 60;
    if (minute != g_budgetWindow) { g_budgetWindow = minute; g_budgetSpent = 0; }
    if (g_budgetSpent + weight > HL_BUDGET_PER_MINUTE) return false;
    g_budgetSpent += weight;
    return true;
}

json infoPost(const json& body, int weight) {
    if (!spendBudget(weight)) {
        g_budgetSkips.fetch_add(1, std::memory_order_relaxed);
        return json();
    }
    std::string resp = http(HL_INFO_URL, body.dump(), 15);
    if (resp.empty()) return json();
    json j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) return json();
    return j;
}

// ====================== Очередь на дозагрузку ======================

std::mutex g_queueMutex;
std::set<std::string> g_enrichQueue;
// Когда кошелёк можно трогать снова. Держим в памяти, а не читаем из базы:
// иначе при дебаунсе в 30 секунд и цикле в 2 секунды каждый ждущий кошелёк
// давал бы полтора десятка бессмысленных запросов к базе.
std::unordered_map<std::string, long long> g_nextEnrichAt;

void queueWallet(const std::string& addr) {
    std::lock_guard<std::mutex> l(g_queueMutex);
    g_enrichQueue.insert(addr);
}

bool readyToEnrich(const std::string& addr) {
    std::lock_guard<std::mutex> l(g_queueMutex);
    auto it = g_nextEnrichAt.find(addr);
    return it == g_nextEnrichAt.end() || nowSec() >= it->second;
}

void setNextEnrich(const std::string& addr, long long at) {
    std::lock_guard<std::mutex> l(g_queueMutex);
    g_nextEnrichAt[addr] = at;
}

// ====================== Поток и его состояние ======================

std::atomic<bool> g_hlRunning{false};
std::thread g_feedThread;
std::thread g_enrichThread;

bool keepGoing() {
    return g_hlRunning.load(std::memory_order_relaxed) &&
           running.load(std::memory_order_relaxed);
}

void interruptibleSleep(int seconds) {
    for (int i = 0; i < seconds * 10 && keepGoing(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ========================= Список рынков (REST) =========================

// Два списка из одного запроса, и они РАЗНЫЕ:
//   subscribe - на что подписываться (делистнутые не нужны, сделок не дают);
//   all       - что считать фьючерсом при отсеве спота. Делистнутый рынок
//               по-прежнему фьючерс, и закрывающие сделки по нему идут -
//               выкинуть их значило бы потерять настоящие перп-сделки.
bool fetchPerpCoins(std::vector<std::string>& subscribe, std::vector<std::string>& all) {
    subscribe.clear();
    all.clear();
    json body;
    body["type"] = "meta";
    body["dex"] = "";
    json j = infoPost(body, HL_WEIGHT_META);
    if (!j.is_object() || !j.contains("universe") || !j["universe"].is_array()) {
        std::cerr << "[HL] meta: неожиданный ответ" << std::endl;
        return false;
    }
    for (const auto& u : j["universe"]) {
        if (!u.is_object() || !u.contains("name") || !u["name"].is_string()) continue;
        std::string name = u["name"].get<std::string>();
        if (name.empty()) continue;
        all.push_back(name);
        if (!u.value("isDelisted", false)) subscribe.push_back(name);
    }
    return !all.empty();
}

// ====================== Состояние позиций (REST) ======================
// Плечо и маржа живут не в сделке, а в состоянии счёта: в данных о сделке этих
// полей нет вообще. Запрос дешёвый (вес 2 против 20 у истории сделок), поэтому
// берём его на каждую дозагрузку.

struct PositionInfo {
    bool known = false;
    // Когда снимок сделан и жива ли позиция СЕЙЧАС. Без этого не отличить
    // частичное закрытие (позиция осталась, цена ликвидации настоящая и
    // сдвинулась) от полного (позиции нет, ликвидировать нечего). Разница
    // видна только по тому, вернул ли счёт эту монету последним запросом.
    long long snapshotAt = 0;
    bool stillOpen = false;
    int leverage = 0;
    bool isolated = false;
    long long marginUsedNanos = 0;
    long long positionValueNanos = 0;
    long long liquidationPxNanos = 0;
};

// При полном закрытии позиция исчезает из ответа, и плеча там больше нет.
// Поэтому последний известный снимок держим в памяти: алерт о закрытии
// показывает плечо, с которым позиция велась.
std::mutex g_posMutex;
std::unordered_map<std::string, PositionInfo> g_lastPositions;  // "адрес|монета"
// Депозит по кошельку. Отдельно от позиций намеренно: он приходит в том же
// ответе, но относится ко всему счёту. Хранить его внутри позиции значило бы
// терять его у китов, закрывающих позиции целиком, - а это как раз те, у кого
// доходность интереснее всего.
std::unordered_map<std::string, long long> g_accountValue;

std::string posKey(const std::string& wallet, const std::string& coin) {
    return wallet + "|" + coin;
}

void fetchAccountState(const std::string& wallet) {
    json body;
    body["type"] = "clearinghouseState";
    body["user"] = wallet;
    body["dex"] = "";
    json j = infoPost(body, HL_WEIGHT_CLEARINGHOUSE);
    if (!j.is_object()) return;

    long long accountValue = 0;
    if (j.contains("marginSummary") && j["marginSummary"].is_object())
        parseDecimalToNanos(j["marginSummary"].value("accountValue", std::string("0")), accountValue);

    // Депозит записываем ДО разбора позиций: даже если открытых позиций нет
    // вовсе, счёт существует и его размер известен.
    {
        std::lock_guard<std::mutex> l(g_posMutex);
        if (accountValue > 0) g_accountValue[wallet] = accountValue;
    }

    if (!j.contains("assetPositions") || !j["assetPositions"].is_array()) return;

    std::lock_guard<std::mutex> l(g_posMutex);
    // Закрытые позиции мы намеренно не удаляем - из них берётся плечо для
    // алерта о закрытии. Но расти бесконечно кэш не должен, поэтому при
    // переполнении сбрасываем целиком: он восстановится сам за один проход.
    if (g_lastPositions.size() > HL_POSITION_CACHE_CAP) {
        g_lastPositions.clear();
        g_accountValue.clear();
        std::cout << "[HL] кэш позиций сброшен по достижении предела" << std::endl;
    }
    for (const auto& ap : j["assetPositions"]) {
        if (!ap.is_object() || !ap.contains("position") || !ap["position"].is_object()) continue;
        const json& p = ap["position"];
        std::string coin = p.value("coin", std::string());
        if (coin.empty()) continue;

        PositionInfo info;
        info.known = true;
        info.snapshotAt = nowSec();
        if (p.contains("leverage") && p["leverage"].is_object()) {
            const json& lev = p["leverage"];
            if (lev.contains("value") && lev["value"].is_number())
                info.leverage = lev["value"].get<int>();
            info.isolated = lev.value("type", std::string()) == "isolated";
        }
        parseDecimalToNanos(p.value("marginUsed", std::string("0")), info.marginUsedNanos);
        parseDecimalToNanos(p.value("positionValue", std::string("0")), info.positionValueNanos);
        parseDecimalToNanos(p.value("liquidationPx", std::string("0")), info.liquidationPxNanos);

        g_lastPositions[posKey(wallet, coin)] = info;
    }
}

long long lastKnownAccountValue(const std::string& wallet) {
    std::lock_guard<std::mutex> l(g_posMutex);
    auto it = g_accountValue.find(wallet);
    return it == g_accountValue.end() ? 0 : it->second;
}

PositionInfo lastKnownPosition(const std::string& wallet, const std::string& coin) {
    std::lock_guard<std::mutex> l(g_posMutex);
    auto it = g_lastPositions.find(posKey(wallet, coin));
    return it == g_lastPositions.end() ? PositionInfo{} : it->second;
}

// ====================== Служебное состояние кошелька ======================

struct WalletState {
    bool seeded = false;       // первый проход уже сделан, можно слать алерты
    long long lastFillTs = 0;  // время последней сохранённой сделки, мс
    long long lastEnriched = 0;
    long long debounce = HL_ENRICH_DEBOUNCE_SEC;
};

bool loadWalletState(const std::string& wallet, WalletState& out) {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return false;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT seeded, last_fill_ts, last_enriched, debounce_sec FROM hl_wallet_state WHERE wallet=?"))
        return false;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        out.seeded = sqlite3_column_int(s, 0) != 0;
        out.lastFillTs = sqlite3_column_int64(s, 1);
        out.lastEnriched = sqlite3_column_int64(s, 2);
        out.debounce = sqlite3_column_int64(s, 3);
        // Значение в базе могло быть записано прежней версией с меньшим базовым
        // дебаунсом. Поднимаем до текущего базового - иначе смена константы не
        // подействовала бы на уже известные кошельки, и правка оказалась бы
        // бесполезной ровно там, где нужна. Повышенный дебаунс гиперактивных
        // (600) при этом не трогаем: он больше базового и выставлен осознанно.
        if (out.debounce < HL_ENRICH_DEBOUNCE_SEC) out.debounce = HL_ENRICH_DEBOUNCE_SEC;
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

void saveWalletState(const std::string& wallet, const WalletState& st) {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "INSERT INTO hl_wallet_state(wallet,seeded,last_fill_ts,last_enriched,debounce_sec) "
            "VALUES(?,?,?,?,?) ON CONFLICT(wallet) DO UPDATE SET "
            "seeded=excluded.seeded, last_fill_ts=excluded.last_fill_ts, "
            "last_enriched=excluded.last_enriched, debounce_sec=excluded.debounce_sec"))
        return;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, st.seeded ? 1 : 0);
    sqlite3_bind_int64(s, 3, st.lastFillTs);
    sqlite3_bind_int64(s, 4, st.lastEnriched);
    sqlite3_bind_int64(s, 5, st.debounce);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

// Откат незакрытой транзакции. Если запись прервалась на середине пачки,
// открытый BEGIN остался бы висеть и заблокировал бы всю дальнейшую запись.
void rollbackIfOpen() {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (g_hlDb && sqlite3_get_autocommit(g_hlDb) == 0)
        sqlite3_exec(g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
}

// Уборка. Без неё база растёт вечно: рейтинг смотрит 30 дней, всё остальное -
// мёртвый груз. Запас в 15 дней на случай пересчёта задним числом.
// Вычистка нефьючерсных строк по СПИСКУ РЫНКОВ, а не по виду названия:
// угадывать спот по косой черте ненадёжно, названия на площадке разные.
// Мьютексы берём по очереди, а не вложенно - список монет и база защищены
// разными замками.
void purgeNonPerpFills() {
    std::vector<std::string> distinctCoins;
    {
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (!g_hlDb) return;
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(g_hlDb, &s, "SELECT DISTINCT coin FROM hl_fills")) return;
        while (sqlite3_step(s) == SQLITE_ROW) distinctCoins.push_back(safeColumnText(s, 0));
        sqlite3_finalize(s);
    }

    std::vector<std::string> alien;
    for (const std::string& c : distinctCoins)
        if (!c.empty() && !isPerpCoin(c)) alien.push_back(c);
    if (alien.empty()) return;

    // ПРЕДОХРАНИТЕЛЬ от сломанного сопоставления имён. Признак поломки - когда
    // НИ ОДИН рынок из базы не опознан как фьючерсный: значит разошёлся формат
    // имён, а не данные испортились.
    //
    // Раньше здесь стояло "больше половины", и это было ошибкой: после долгой
    // работы без фильтра спотовых строк накапливается больше, чем фьючерсных,
    // и порог блокировал бы именно ту уборку, ради которой всё писалось.
    if (alien.size() == distinctCoins.size()) {
        std::cerr << "[HL] уборка ОТМЕНЕНА: НИ ОДИН рынок из " << distinctCoins.size()
                  << " не опознан как фьючерсный - сломалось сопоставление имён. "
                  << "Примеры: ";
        for (size_t i = 0; i < alien.size() && i < 5; i++)
            std::cerr << alien[i] << (i + 1 < std::min<size_t>(alien.size(), 5) ? ", " : "");
        std::cerr << std::endl;
        return;
    }

    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return;
    int purged = 0;
    for (const std::string& c : alien) {
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(g_hlDb, &s, "DELETE FROM hl_fills WHERE coin=?")) continue;
        sqlite3_bind_text(s, 1, c.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_DONE) purged += sqlite3_changes(g_hlDb);
        sqlite3_finalize(s);
    }
    if (purged > 0)
        std::cout << "[HL] удалено нефьючерсных строк: " << purged
                  << " по " << alien.size() << " рынкам" << std::endl;
}

void cleanupOldFills() {
    const long long cutoffMs = (nowSec() - HL_FILL_TTL_SEC) * 1000LL;
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return;
    sqlite3_stmt* s = nullptr;
    if (prepareOrLog(g_hlDb, &s, "DELETE FROM hl_fills WHERE ts < ?")) {
        sqlite3_bind_int64(s, 1, cutoffMs);
        const int deleted = (sqlite3_step(s) == SQLITE_DONE) ? sqlite3_changes(g_hlDb) : 0;
        sqlite3_finalize(s);
        if (deleted > 0) std::cout << "[HL] удалено сделок старше срока: " << deleted << std::endl;
    }
    // Состояние кошельков, которые давно не появлялись, тоже незачем хранить:
    // при следующей встрече оно просто создастся заново.
    if (prepareOrLog(g_hlDb, &s, "DELETE FROM hl_wallet_state WHERE last_enriched < ?")) {
        sqlite3_bind_int64(s, 1, nowSec() - HL_FILL_TTL_SEC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

// Сколько сделок у кошелька в окне рейтинга. Индекс (wallet, ts) закрывает
// этот запрос целиком.
int countFillsInWindow(const std::string& wallet) {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return 0;
    sqlite3_stmt* s = nullptr;
    // Считаем ЗАКРЫВАЮЩИЕ сделки - ровно то число, которое видно в карточке
    // рейтинга строкой "Сделки". Раньше здесь считались разные ордера, и это
    // было логично само по себе, но снаружи выглядело поломкой: в карточке
    // 1025 при пороге 1000, а бана нет. Показанное и судимое должны совпадать.
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT COUNT(*) FROM hl_fills"
            " WHERE wallet=? AND ts >= ? AND closed_pnl_nanos != 0")) return 0;
    sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (nowSec() - HL_RANK_WINDOW_SEC) * 1000LL);
    int n = 0;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

// Заносит кошелёк в бан. true - бан именно сейчас записан (а не был раньше):
// по нему решается, снимать ли кошелёк с сервисного аккаунта.
bool banWallet(const std::string& wallet, int trades) {
    bool saved = false;
    {
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (!g_hlDb) return false;
        sqlite3_stmt* s = nullptr;
        if (!prepareOrLog(g_hlDb, &s,
                "INSERT OR IGNORE INTO hl_banned(wallet,banned_at,trades) VALUES(?,?,?)"))
            return false;
        sqlite3_bind_text(s, 1, wallet.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, nowSec());
        sqlite3_bind_int(s, 3, trades);
        if (sqlite3_step(s) == SQLITE_DONE) saved = sqlite3_changes(g_hlDb) > 0;
        sqlite3_finalize(s);
    }
    if (!saved) return false;

    {
        // Набор неизменяемый, поэтому дополняем копией и подменяем указатель.
        // Баны редки, а чтения идут постоянно - платить за них выгоднее здесь.
        std::lock_guard<std::mutex> l(g_walletsMutex);
        auto updated = std::make_shared<AddressSet>(*g_banned);
        updated->insert(wallet);
        g_banned = updated;
    }
    { std::lock_guard<std::mutex> l(g_rankMutex); g_rankBuiltAt = 0; }   // рейтинг пересчитать
    return true;
}

// Сохранение сделки. false - такая уже была: tid у площадки уникален, поэтому
// первичный ключ сам гасит повторы от перекрытия окон дозагрузки. Возврат
// заодно решает, слать ли алерт: о повторе сообщать не надо.
// ВАЖНО: вызывается с уже захваченным g_hlDbMutex. Своей блокировки внутри нет
// намеренно - иначе транзакция вокруг пачки оставалась бы открытой в моменты,
// когда база никем не удерживается, и чужая запись молча попадала бы внутрь
// чужой транзакции, а при откате исчезала бы вместе с ней.
bool saveFillLocked(const std::string& wallet, const json& f, long long closedPnlNanos,
                    long long feeNanos, long long notionalNanosVal, const PositionInfo& pos,
                    long long accountValueNanos) {
    long long tid = f.value("tid", 0LL);
    if (tid == 0) return false;
    if (!g_hlDb) return false;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "INSERT OR IGNORE INTO hl_fills"
            "(tid,wallet,coin,dir,side,px,sz,closed_pnl_nanos,fee_nanos,"
            "notional_nanos,margin_nanos,leverage,oid,account_value_nanos,ts,hash) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
        return false;
    sqlite3_bind_int64(s, 1, tid);
    sqlite3_bind_text(s, 2, wallet.c_str(), -1, SQLITE_TRANSIENT);
    std::string coin = f.value("coin", std::string());
    std::string dir  = f.value("dir", std::string());
    std::string side = f.value("side", std::string());
    std::string px   = f.value("px", std::string());
    std::string sz   = f.value("sz", std::string());
    std::string hash = f.value("hash", std::string());
    sqlite3_bind_text(s, 3, coin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, dir.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, side.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 6, px.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 7, sz.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 8, closedPnlNanos);
    sqlite3_bind_int64(s, 9, feeNanos);
    sqlite3_bind_int64(s, 10, notionalNanosVal);
    sqlite3_bind_int64(s, 11, pos.known ? pos.marginUsedNanos : 0);
    sqlite3_bind_int(s,   12, pos.known ? pos.leverage : 0);
    sqlite3_bind_int64(s, 13, f.value("oid", 0LL));
    sqlite3_bind_int64(s, 14, accountValueNanos);
    sqlite3_bind_int64(s, 15, f.value("time", 0LL));
    sqlite3_bind_text(s,  16, hash.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    int changed = sqlite3_changes(g_hlDb);
    sqlite3_finalize(s);
    return ok && changed > 0;
}

// ============================== Текст алерта ==============================

std::string dirKey(const std::string& dir) {
    if (dir.find("Open Long")   != std::string::npos) return "hl_open_long";
    if (dir.find("Close Long")  != std::string::npos) return "hl_close_long";
    if (dir.find("Open Short")  != std::string::npos) return "hl_open_short";
    if (dir.find("Close Short") != std::string::npos) return "hl_close_short";
    if (dir.find("Long > Short") != std::string::npos ||
        dir.find("Short > Long") != std::string::npos) return "hl_flip";
    if (dir.find("Liquidat")    != std::string::npos) return "hl_liquidated";
    return "hl_trade";
}

std::string dirEmoji(const std::string& key) {
    if (key == "hl_open_long" || key == "hl_close_short") return "\U0001F7E2";  // зелёный
    if (key == "hl_close_long" || key == "hl_open_short") return "\U0001F534";  // красный
    if (key == "hl_liquidated") return "\U0001F4A5";                            // взрыв
    return "\u26A1";
}

// Одна серия сделок: подряд идущие операции по одной монете в одну сторону,
// собранные за окно дебаунса. Кит, набиравший позицию тремя заходами за
// минуту, принял одно решение - и человеку должно прийти одно сообщение, а не
// три. Разные монеты и разные направления в одну серию не сливаются: это уже
// разные решения, и склеивать их значило бы потерять смысл.
struct HlAlertData {
    std::string coin;
    std::string dirKey;
    int fillCount = 0;
    long long notionalNanos = 0;   // сумма по серии
    long long closedPnlNanos = 0;  // сумма по серии
    long long qtyNanos = 0;        // сумма по серии
    long long avgPxNanos = 0;      // средняя цена исполнения серии
    PositionInfo pos;
    long long accountValueNanos = 0;
};

std::string buildHlAlert(const std::string& label, const HlAlertData& a, Lang lang) {
    const std::string coin = safeString(a.coin.empty() ? "?" : a.coin, 32);
    const std::string& key = a.dirKey;
    const long long closedPnlNanos = a.closedPnlNanos;
    const long long notionalNanosVal = a.notionalNanos;
    const PositionInfo& pos = a.pos;
    const long long accountValueNanos = a.accountValueNanos;

    const char* const dm = dirMark(lang);

    std::stringstream m;
    m << dm << "\U0001F4BC <b>" << safeString(label) << "</b>\n\n";
    m << dm << dirEmoji(key) << " <b>" << tr(lang, key) << " " << coin << "</b>\n";

    // Размер СДЕЛКИ и размер ПОЗИЦИИ - разные вещи, и путать их нельзя:
    // кит может закрыть $146 из позиции в $28 800. Раньше первая строка
    // называлась "размер позиции" и стояла рядом с залогом всей позиции,
    // отчего выходило, что человек вложил в сто раз больше, чем наторговал.
    m << dm << "\U0001F4B0 " << tr(lang, "hl_trade_size") << ": <b>" << fmtUsd(notionalNanosVal) << "</b>\n";

    // Показываем, что сообщение покрывает несколько сделок - иначе сумма и
    // средняя цена выглядели бы как параметры одной операции.
    if (a.fillCount > 1) {
        m << dm << "\U0001F522 " << tr(lang, "hl_fills_in_series") << ": <b>"
          << a.fillCount << "</b>\n";
    }

    if (pos.stillOpen && pos.positionValueNanos > 0) {
        m << dm << "\U0001F4CA " << tr(lang, "hl_position_size") << ": <b>"
          << fmtUsd(pos.positionValueNanos) << "</b>\n";
    }

    if (pos.known && pos.leverage > 0) {
        m << dm << "\u2699\uFE0F " << tr(lang, "hl_leverage") << ": <b>" << pos.leverage << "\u00D7 "
          << tr(lang, pos.isolated ? "hl_isolated" : "hl_cross") << "</b>\n";
    }

    // Залог ВСЕЙ позиции, а не этой сделки. При кросс-марже это вообще
    // бухгалтерская доля: формально позицию обеспечивает весь счёт. Поэтому
    // рядом даём процент от счёта - иначе "$9,602" ни о чём не говорит, пока
    // не знаешь, три это миллиона на счету или десять тысяч.
    if (pos.stillOpen && pos.marginUsedNanos > 0) {
        m << dm << "\U0001F4B5 " << tr(lang, "hl_collateral") << ": <b>" << fmtUsd(pos.marginUsedNanos) << "</b>";
        if (accountValueNanos > 0) {
            const double share = 100.0 * static_cast<double>(pos.marginUsedNanos)
                                       / static_cast<double>(accountValueNanos);
            m << " <i>(" << formatPercent(share, false) << " " << tr(lang, "hl_of_account") << ")</i>";
        }
        m << "\n";
    }

    // Цена серии - средневзвешенная по объёму, а не последняя: при наборе
    // позиции заходами последняя цена не описывает сделку целиком.
    if (a.avgPxNanos > 0)
        m << dm << "\U0001F4CD " << tr(lang, "hl_price") << ": <b>"
          << formatPriceNanos(a.avgPxNanos) << "</b>\n";

    if (a.qtyNanos > 0)
        m << dm << "\U0001F4E6 " << tr(lang, "hl_qty") << ": <b>"
          << formatQtyNanos(a.qtyNanos) << " " << coin << "</b>\n";

    // closedPnl отличен от нуля только у закрывающих сделок - у открывающих
    // прибыли ещё нет, и строка была бы обманчивым "+$0".
    if (closedPnlNanos != 0) {
        m << dm << (closedPnlNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ") << tr(lang, "hl_pnl") << ": <b>"
          << formatUsdNanosSigned(closedPnlNanos, true) << "</b>\n";
    }

    // Ликвидация показывается всегда, пока позиция жива - в том числе при
    // частичном закрытии, где она как раз самое интересное: кит уменьшил
    // позицию, и цена ликвидации отъехала на безопасное расстояние. Скрывать
    // её нужно только тогда, когда закрывать больше нечего.
    if (pos.stillOpen && pos.liquidationPxNanos > 0) {
        m << dm << "\u2620\uFE0F " << tr(lang, "hl_liq") << ": <b>"
          << formatPriceNanos(pos.liquidationPxNanos) << "</b>\n";
    } else if (pos.known && !pos.stillOpen && closedPnlNanos != 0) {
        // Явная строка лучше молчания: иначе непонятно, то ли позиция закрыта
        // целиком, то ли данные просто не пришли.
        m << dm << "\u2705 " << tr(lang, "hl_position_closed") << "\n";
    }
    if (accountValueNanos > 0) {
        m << dm << "\U0001F3E6 " << tr(lang, "hl_account") << ": <b>" << fmtUsd(accountValueNanos) << "</b>\n";
    }

    m << "\n" << dm << "\U0001F310 " << tr(lang, "hl_venue") << ": <b>Hyperliquid</b>";
    // Ссылки на обозреватель нет намеренно: сеть другая, ссылка на BSC-скан
    // здесь вела бы в никуда.
    return m.str();
}

void dispatchHlAlert(const std::string& wallet, const HlAlertData& a) {
    const long long notionalNanosVal = a.notionalNanos;
    if (notionalNanosVal <= 0) return;
    std::vector<HlRecipient> recipients = hlWatchersFor(wallet);
    if (recipients.empty()) return;

    std::map<std::pair<std::string, Lang>, std::vector<std::string>> byLabelLang;
    for (const HlRecipient& r : recipients) {
        if (r.chatId == SERVICE_CHAT_ID) continue;
        if (!isPremium(r.chatId)) continue;          // перп-алерты только по подписке
        if (static_cast<uint64_t>(notionalNanosVal) < r.thresholdNanos) continue;
        Lang lang = langFromCode(getUserLanguage(r.chatId));
        byLabelLang[{r.label, lang}].push_back(r.chatId);
    }
    if (byLabelLang.empty()) return;

    for (auto& entry : byLabelLang) {
        std::string msg = buildHlAlert(entry.first.first, a, entry.first.second);
        if (g_msgQueue.enqueueToRecipients(msg, entry.second))
            g_alertsSent.fetch_add(1, std::memory_order_relaxed);
    }
}

// ============================== Дозагрузка ==============================

void enrichWallet(const std::string& wallet) {
    if (isBanned(wallet)) return;   // мог попасть в очередь до бана

    WalletState st;
    const bool known = loadWalletState(wallet, st);
    const long long now = nowSec();

    if (known && now - st.lastEnriched < st.debounce) {
        setNextEnrich(wallet, st.lastEnriched + st.debounce);
        queueWallet(wallet);   // ещё рано - вернём в очередь, разберём позже
        return;
    }

    const long long startMs = (known && st.seeded)
        ? st.lastFillTs + 1
        : nowMs() - HL_SEED_LOOKBACK_MS;

    json body;
    body["type"] = "userFillsByTime";
    body["user"] = wallet;
    body["startTime"] = startMs;
    // Без этого один ордер, исполненный стаканом по кускам, приходит
    // несколькими записями - и вместо одной сделки на $50 000 ушло бы пять
    // алертов на случайные доли от неё. Ни одно из показанных чисел не
    // соответствовало бы решению, которое кит принял.
    body["aggregateByTime"] = true;
    json fills = infoPost(body, HL_WEIGHT_USER_FILLS);
    if (!fills.is_array()) {
        queueWallet(wallet);   // бюджет исчерпан или площадка не ответила
        return;
    }

    g_enriched.fetch_add(1, std::memory_order_relaxed);

    const bool wasSeeded = known && st.seeded;
    // Плечо и маржу берём одним запросом на весь кошелёк, а не на сделку:
    // состояние счёта общее, а вес запроса тратится каждый раз.
    const long long stateAt = nowSec();
    if (!fills.empty()) fetchAccountState(wallet);

    // Проход 1: разбор и расчёт. Обращений к базе нет, поэтому и блокировка
    // здесь не нужна - а заодно не приходится держать её во время работы с
    // кэшем позиций.
    struct Prepared {
        const json* fill;
        long long closedPnl;
        long long fee;
        long long notional;
        PositionInfo pos;
    };
    std::vector<Prepared> prepared;
    prepared.reserve(fills.size());
    size_t skippedNonPerp = 0;
    long long maxTs = st.lastFillTs;

    for (const auto& f : fills) {
        if (!f.is_object()) continue;
        const long long ts = f.value("time", 0LL);
        if (ts > maxTs) maxTs = ts;

        // Спотовые сделки на Hyperliquid сюда попадать не должны: рейтинг и
        // алерты построены вокруг плеча и маржи, которых у спота нет.
        // Отличаем по списку перп-рынков, а не по виду названия: спотовые
        // пары бывают и без косой черты.
        if (!isPerpCoin(f.value("coin", std::string()))) { skippedNonPerp++; continue; }

        Prepared p;
        p.fill = &f;
        p.closedPnl = 0;
        p.fee = 0;
        parseDecimalToNanos(f.value("closedPnl", std::string("0")), p.closedPnl);
        parseDecimalToNanos(f.value("fee", std::string("0")), p.fee);
        p.notional = 0;
        notionalNanos(f.value("px", std::string("0")), f.value("sz", std::string("0")), p.notional);
        p.pos = lastKnownPosition(wallet, f.value("coin", std::string()));
        // Монета есть в свежем снимке счёта - значит позиция жива: закрытие было
        // частичным. Нет - закрыли целиком, и всё, что мы помним о позиции,
        // относится к тому, чего больше не существует.
        p.pos.stillOpen = p.pos.known && p.pos.snapshotAt >= stateAt;
        prepared.push_back(std::move(p));
    }

    // Проход 2: запись. Вся пачка под ОДНИМ захватом мьютекса - транзакция не
    // должна существовать в моменты, когда база свободна. Первичное наполнение
    // приносит сотни сделок разом, и без общей транзакции каждая запись была бы
    // отдельным сбросом на диск.
    const long long accountValue = lastKnownAccountValue(wallet);

    std::vector<size_t> freshRows;
    size_t stored = 0;
    {
        const bool bulk = prepared.size() > 20;
        std::lock_guard<std::mutex> l(g_hlDbMutex);
        if (g_hlDb) {
            if (bulk) sqlite3_exec(g_hlDb, "BEGIN", nullptr, nullptr, nullptr);
            for (size_t i = 0; i < prepared.size(); i++) {
                const Prepared& p = prepared[i];
                if (saveFillLocked(wallet, *p.fill, p.closedPnl, p.fee, p.notional, p.pos,
                                   accountValue)) {
                    stored++;
                    if (p.notional > 0) freshRows.push_back(i);
                }
            }
            if (bulk) sqlite3_exec(g_hlDb, "COMMIT", nullptr, nullptr, nullptr);
        }
    }

    // Проход 3: сборка серий и рассылка. ВНЕ блокировки базы перпов - здесь и
    // обращение к главной базе за языком получателя, и постановка в очередь
    // отправки. Держать на этом свой мьютекс значило бы вешать экран рейтинга
    // у всех остальных на время сетевых операций.
    //
    // Сделки за окно дебаунса собираются по монете и направлению: кит, бравший
    // позицию тремя заходами за минуту, принял одно решение, и сообщение
    // должно быть одно. Разные монеты и разные стороны остаются раздельными.
    if (wasSeeded && !freshRows.empty()) {
        std::map<std::string, HlAlertData> series;
        for (size_t i : freshRows) {
            const Prepared& p = prepared[i];
            const std::string coin = p.fill->value("coin", std::string());
            const std::string dk = dirKey(p.fill->value("dir", std::string()));

            HlAlertData& a = series[coin + "|" + dk];
            if (a.fillCount == 0) {          // первая сделка серии задаёт общее
                a.coin = coin;
                a.dirKey = dk;
            }
            a.fillCount++;
            a.notionalNanos += p.notional;
            a.closedPnlNanos += p.closedPnl;
            long long sz = 0;
            if (parseDecimalToNanos(p.fill->value("sz", std::string("0")), sz)) {
                if (sz < 0) sz = -sz;
                a.qtyNanos += sz;
            }
            // Состояние позиции берём от ПОСЛЕДНЕЙ сделки серии: плечо, залог и
            // цена ликвидации к этому моменту уже учитывают всю серию целиком.
            a.pos = p.pos;
            a.accountValueNanos = accountValue;
        }

        for (auto& kv : series) {
            HlAlertData& a = kv.second;
            // Средневзвешенная цена: сумма денег делится на сумму монет.
            if (a.qtyNanos > 0) {
                const __int128 num = static_cast<__int128>(a.notionalNanos) * NANOS_PER_UNIT;
                a.avgPxNanos = static_cast<long long>(num / a.qtyNanos);
            }
            dispatchHlAlert(wallet, a);
        }
    }

    // Отсеяли всё до единой сделки - это не нормальная работа фильтра, а почти
    // наверняка расхождение в именах рынков. Молчать об этом нельзя: снаружи
    // выглядело бы как "кит просто не торгует".
    if (!fills.empty() && skippedNonPerp == fills.size()) {
        std::cerr << "[HL] ВНИМАНИЕ: у " << wallet << " отсеяны ВСЕ " << fills.size()
                  << " сделок как нефьючерсные. Первая монета: "
                  << fills[0].value("coin", std::string("?"))
                  << " - сверь с именами рынков из meta." << std::endl;
    } else if (stored > 0 && !wasSeeded) {
        std::cout << "[HL] первичное наполнение " << wallet << ": " << stored
                  << " сделок записано молча" << std::endl;
    }

    // Гиперактивный адрес в одиночку съел бы весь бюджет - переводим на редкий опрос.
    if (fills.size() >= static_cast<size_t>(HL_HYPERACTIVE_FILLS)) {
        if (st.debounce < HL_HYPERACTIVE_DEBOUNCE_SEC) {
            st.debounce = HL_HYPERACTIVE_DEBOUNCE_SEC;
            std::cout << "[HL] " << wallet << " торгует слишком часто, опрос раз в "
                      << HL_HYPERACTIVE_DEBOUNCE_SEC << "с" << std::endl;
        }
    }
    // Если сделок не было, maxTs остаётся нулём - и следующий запрос ушёл бы с
    // startTime=1, то есть забрал бы ВСЮ историю кошелька. А так как seeded к
    // тому моменту уже true, по всей этой истории полетели бы алерты. Двигаем
    // границу к началу запрошенного окна.
    if (maxTs < startMs) maxTs = startMs - 1;
    st.seeded = true;
    st.lastFillTs = maxTs;
    st.lastEnriched = now;
    saveWalletState(wallet, st);
    setNextEnrich(wallet, now + st.debounce);

    // Детектор ботов. Считаем ПОСЛЕ записи, по фактическому содержимому базы,
    // а не по размеру последней пачки: алгоритм может торговать ровно, не
    // выдавая себя всплесками, и попадётся только на объёме за месяц.
    const int total = countFillsInWindow(wallet);
    if (total > HL_MAX_BOT_TRADES && banWallet(wallet, total)) {
        g_botsBanned.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[HL] бот на фьючерсах: " << wallet << " - " << total
                  << " ордеров за 30 дней, исключён из перп-рейтинга" << std::endl;
    }
    // С сервисного аккаунта кошелёк НЕ снимаем, в отличие от спотового
    // детектора. Маркет-мейкер на фьючерсах не обязан быть алгоритмом на BSC:
    // это разные площадки и разные стратегии. Спотовая часть судит сама, по
    // своему порогу. Бан здесь остаётся местным - перп-рейтинг, дозагрузка и
    // перп-алерты, - а спотовое отслеживание продолжается как ни в чём не бывало.
}

void enricherLoop() {
    // Ноль, а не текущее время: первая уборка должна пройти сразу, как только
    // подтянется список рынков, а не через час работы. Бот перезапускают чаще,
    // чем раз в час, и с прежним отсчётом уборка не случалась НИ РАЗУ - мусор
    // копился, хотя фильтр стоял.
    long long lastCleanup = 0;
    while (keepGoing()) {
        std::set<std::string> batch;
        {
            std::lock_guard<std::mutex> l(g_queueMutex);
            batch.swap(g_enrichQueue);
        }
        for (const std::string& w : batch) {
            if (!keepGoing()) break;
            if (!readyToEnrich(w)) { queueWallet(w); continue; }
            // Исключение на одном кошельке не должно уносить поток: без
            // перехвата оно вышло бы из потока и обрушило весь процесс,
            // а вместе с ним и спотовую часть бота.
            try {
                enrichWallet(w);
            } catch (const std::exception& e) {
                std::cerr << "[HL] сбой дозагрузки " << w << ": " << e.what() << std::endl;
                rollbackIfOpen();
            } catch (...) {
                std::cerr << "[HL] неизвестный сбой дозагрузки " << w << std::endl;
                rollbackIfOpen();
            }
        }
        // Ждём список рынков: без него isPerpCoin считает фьючерсом всё подряд,
        // и уборка прошла бы вхолостую, отметившись как выполненная.
        if (perpCoinsLoaded() &&
            (lastCleanup == 0 || nowSec() - lastCleanup >= HL_CLEANUP_INTERVAL_SEC)) {
            purgeNonPerpFills();
            cleanupOldFills();
            lastCleanup = nowSec();
        }

        for (int i = 0; i < 20 && keepGoing(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "[HL] дозагрузчик остановлен" << std::endl;
}

// ============================== WebSocket ==============================

bool wsSendText(CURL* c, const std::string& payload) {
    size_t sent = 0;
    CURLcode rc = curl_ws_send(c, payload.data(), payload.size(), &sent, 0, CURLWS_TEXT);
    // Частичная отправка означала бы, что остаток надо досылать продолжением
    // кадра. Сообщения здесь короткие (подписка, пинг), поэтому такой случай
    // трактуем как сбой соединения, а не городим сборку кадров по кускам.
    if (rc != CURLE_OK || sent != payload.size()) {
        if (rc != CURLE_OK)
            std::cerr << "[HL] отправка не удалась: " << curl_easy_strerror(rc) << std::endl;
        return false;
    }
    return true;
}

bool wsSubscribeTrades(CURL* c, const std::string& coin) {
    json sub;
    sub["method"] = "subscribe";
    sub["subscription"]["type"] = "trades";
    sub["subscription"]["coin"] = coin;
    return wsSendText(c, sub.dump());
}

// Ожидание данных на сокете. Без него цикл на CURLE_AGAIN крутился бы
// вхолостую и съедал ядро.
void waitReadable(CURL* c, int timeoutMs) {
    curl_socket_t sock = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK || sock == CURL_SOCKET_BAD) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return;
    }
    struct pollfd pfd;
    pfd.fd = static_cast<int>(sock);
    pfd.events = POLLIN;
    pfd.revents = 0;
    poll(&pfd, 1, timeoutMs);
}

// ========================= Разбор входящих сообщений =========================

void handleTrades(const json& data) {
    if (!data.is_array()) return;

    const auto watchedPtr = watchedSnapshot();
    const auto bannedPtr = bannedSnapshot();
    const AddressSet& watched = *watchedPtr;
    const AddressSet& banned = *bannedPtr;
    if (watched.empty()) {
        g_tradesSeen.fetch_add(data.size(), std::memory_order_relaxed);
        return;
    }

    for (const auto& t : data) {
        if (!t.is_object()) continue;
        g_tradesSeen.fetch_add(1, std::memory_order_relaxed);
        if (!t.contains("users") || !t["users"].is_array()) continue;

        for (const auto& u : t["users"]) {
            if (!u.is_string()) continue;
            const std::string addr = toLower(u.get<std::string>());
            if (!watched.count(addr)) continue;
            // Забаненный алгоритм не должен ни тратить бюджет дозагрузки, ни
            // порождать алерты - отсекаем в самом начале, до всех расчётов.
            if (banned.count(addr)) continue;

            g_hits.fetch_add(1, std::memory_order_relaxed);

            // Порог проверяем ЗДЕСЬ, до всякой дозагрузки, и по нотионалу -
            // он считается из фида бесплатно. Если фильтровать по марже,
            // пришлось бы запрашивать состояние счёта на каждую сделку каждого
            // кита только чтобы узнать, проходит она порог или нет; бюджета
            // REST на это не хватило бы. Мелочь отсекается даром, и запросы
            // тратятся только на то, что кому-то действительно уйдёт.
            long long notional = 0;
            if (!notionalNanos(t.value("px", std::string("0")),
                               t.value("sz", std::string("0")), notional)) continue;

            // Сервисный аккаунт наполняет рейтинг, а не получает алерты:
            // порог к нему не применяется, иначе кошельки, залитые через
            // /import, не попали бы в hl_fills и рейтинг остался бы пустым.
            // Перпы доступны только по подписке, поэтому и дозагрузку затевать
            // стоит лишь ради тех, кто её получит. Бесплатный подписчик на
            // кошелёк не повод тратить лимит площадки.
            bool serviceWatched = false;
            uint64_t minThreshold = 0;
            bool haveUser = false;
            for (const HlRecipient& r : hlWatchersFor(addr)) {
                if (r.chatId == SERVICE_CHAT_ID) { serviceWatched = true; continue; }
                if (!isPremium(r.chatId)) continue;
                if (!haveUser || r.thresholdNanos < minThreshold) {
                    minThreshold = r.thresholdNanos;
                    haveUser = true;
                }
            }
            if (!serviceWatched && !haveUser) continue;             // подписчиков нет
            if (!serviceWatched && static_cast<uint64_t>(notional) < minThreshold) continue;

            queueWallet(addr);
        }
    }
}

void handleWsMessage(const std::string& raw) {
    g_lastMsgTs.store(nowSec(), std::memory_order_relaxed);

    json j = json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    const std::string channel = j.value("channel", std::string());
    if (channel == "trades") {
        // Данные приходят из сети и формат может измениться без предупреждения.
        // Уронить на них поток фида нельзя - переподключение не поможет,
        // прилетит то же самое.
        try {
            if (j.contains("data")) handleTrades(j["data"]);
        } catch (const std::exception& e) {
            std::cerr << "[HL] сбой разбора сделок: " << e.what() << std::endl;
        }
    } else if (channel == "subscriptionResponse") {
        g_subscribedCoins.fetch_add(1, std::memory_order_relaxed);
    } else if (channel == "error") {
        std::cerr << "[HL] ошибка площадки: " << safeString(raw, 300) << std::endl;
    }
    // "pong" отдельной обработки не требует: сам факт сообщения уже обновил
    // отметку времени выше, а она и есть признак живого соединения.
}

// ============================ Один сеанс связи ============================

bool runSession(const std::vector<std::string>& coins) {
    CURL* c = curl_easy_init();
    if (!c) return false;

    curl_easy_setopt(c, CURLOPT_URL, HL_WS_URL);
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        if (rc == CURLE_UNSUPPORTED_PROTOCOL)
            logCritical("[HL] libcurl собран без поддержки WebSocket - перпы работать не будут");
        else
            std::cerr << "[HL] соединение не установлено: " << curl_easy_strerror(rc) << std::endl;
        curl_easy_cleanup(c);
        return false;
    }

    g_connected.store(true, std::memory_order_relaxed);
    g_subscribedCoins.store(0, std::memory_order_relaxed);
    g_lastMsgTs.store(nowSec(), std::memory_order_relaxed);
    std::cout << "[HL] соединение установлено, рынков к подписке: " << coins.size() << std::endl;

    // После обрыва площадка НИЧЕГО не помнит, поэтому весь список отправляется
    // заново при каждом сеансе.
    bool alive = true;
    for (const std::string& coin : coins) {
        if (!keepGoing()) { alive = false; break; }
        if (!wsSubscribeTrades(c, coin)) { alive = false; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(HL_SUBSCRIBE_PACE_MS));
    }

    long long lastPing = nowSec();
    std::string message;

    while (alive && keepGoing()) {
        char buf[16384];
        size_t rlen = 0;
        const struct curl_ws_frame* meta = nullptr;

        CURLcode r = curl_ws_recv(c, buf, sizeof(buf), &rlen, &meta);

        if (r == CURLE_AGAIN) {
            waitReadable(c, 500);
        } else if (r != CURLE_OK) {
            std::cerr << "[HL] соединение потеряно: " << curl_easy_strerror(r) << std::endl;
            alive = false;
        } else if (meta && (meta->flags & CURLWS_CLOSE)) {
            std::cerr << "[HL] площадка закрыла соединение" << std::endl;
            alive = false;
        } else if (meta && (meta->flags & (CURLWS_PING | CURLWS_PONG))) {
            // На служебный пинг libcurl отвечает сам, нам он неинтересен.
        } else if (meta) {
            message.append(buf, rlen);
            if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
                handleWsMessage(message);
                message.clear();
            }
        }

        if (!alive) break;

        const long long now = nowSec();
        if (now - lastPing >= HL_PING_INTERVAL_SEC) {
            if (!wsSendText(c, "{\"method\":\"ping\"}")) break;
            lastPing = now;
        }
        if (now - g_lastMsgTs.load(std::memory_order_relaxed) > HL_SILENCE_TIMEOUT_SEC) {
            std::cerr << "[HL] тишина в канале, переподключаюсь" << std::endl;
            break;
        }
    }

    g_connected.store(false, std::memory_order_relaxed);
    curl_easy_cleanup(c);
    return true;
}

// ============================== Поток фида ==============================

void feedLoop() {
    std::vector<std::string> coins;
    long long coinsFetchedAt = 0;
    long long walletsLoadedAt = 0;
    int backoff = HL_RECONNECT_MIN_SEC;

    while (keepGoing()) {
        const long long now = nowSec();

        if (coins.empty() || now - coinsFetchedAt >= HL_COINS_REFRESH_SEC) {
            std::vector<std::string> fresh, allPerps;
            if (fetchPerpCoins(fresh, allPerps)) {
                coins.swap(fresh);
                setPerpCoins(allPerps);      // отсев ведём по ПОЛНОМУ списку
                coinsFetchedAt = now;
            } else if (coins.empty()) {
                std::cerr << "[HL] список рынков получить не удалось, повтор через "
                          << backoff << "с" << std::endl;
                interruptibleSleep(backoff);
                backoff = std::min(backoff * 2, HL_RECONNECT_MAX_SEC);
                continue;
            }
        }

        if (now - walletsLoadedAt >= HL_WALLETS_REFRESH_SEC) {
            reloadWatchedWallets();
            walletsLoadedAt = now;
        }

        const bool connected = runSession(coins);
        if (!keepGoing()) break;

        g_reconnects.fetch_add(1, std::memory_order_relaxed);
        backoff = connected ? HL_RECONNECT_MIN_SEC : std::min(backoff * 2, HL_RECONNECT_MAX_SEC);
        interruptibleSleep(backoff);
    }

    std::cout << "[HL] поток фида остановлен" << std::endl;
}

// ============================== Рейтинг перпов ==============================
// Считается проще спотового: площадка отдаёт готовый closedPnl по каждой
// закрывающей сделке, поэтому себестоимость восстанавливать не нужно.

constexpr int HL_MIN_CLOSED_TRADES = 5;
constexpr int HL_PER_PAGE = 5;
constexpr long long HL_RANK_CACHE_SEC = 300;

struct PerpRow {
    std::string wallet;
    long long pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int closedTrades = 0;
    double avgLeverage = 0.0;
    bool roiKnown = false;
    long long volumeNanos = 0;
    long long lastTs = 0;
};


std::vector<PerpRow> g_rankCache;

std::vector<PerpRow> computeRanking() {
    std::vector<PerpRow> rows;
    const long long sinceMs = (nowSec() - HL_RANK_WINDOW_SEC) * 1000LL;

    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return rows;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "SELECT wallet,"
            " SUM(closed_pnl_nanos),"
            " SUM(CASE WHEN closed_pnl_nanos != 0 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN closed_pnl_nanos > 0 THEN 1 ELSE 0 END),"
            " SUM(notional_nanos),"
            " AVG(CASE WHEN leverage > 0 THEN leverage END),"
            " AVG(CASE WHEN account_value_nanos > 0 THEN account_value_nanos END),"
            " MAX(ts)"
            " FROM hl_fills f WHERE f.ts >= ?"
            " AND NOT EXISTS (SELECT 1 FROM hl_banned b WHERE b.wallet = f.wallet)"
            " GROUP BY wallet"))
        return rows;
    sqlite3_bind_int64(s, 1, sinceMs);

    while (sqlite3_step(s) == SQLITE_ROW) {
        PerpRow r;
        r.wallet = safeColumnText(s, 0);
        r.pnlNanos = sqlite3_column_int64(s, 1);
        r.closedTrades = sqlite3_column_int(s, 2);
        const int wins = sqlite3_column_int(s, 3);
        r.volumeNanos = sqlite3_column_int64(s, 4);
        r.avgLeverage = sqlite3_column_double(s, 5);
        const double avgAccount = sqlite3_column_double(s, 6);
        r.lastTs = sqlite3_column_int64(s, 7);

        if (r.closedTrades < HL_MIN_CLOSED_TRADES) continue;
        r.winRatePercent = static_cast<int>((100LL * wins) / r.closedTrades);
        // ROI = прибыль к депозиту. Знаменатель - СРЕДНИЙ размер счёта за окно,
        // а не сумма по сделкам: депозит один и тот же, сколько бы раз кит ни
        // торговал. Через сумму получалось бы "сколько снято с одной сделки",
        // и у маркет-мейкера с тремя сотнями сделок выходили доли процента при
        // реальной прибыли в тысячи долларов.
        //
        // Спотовый рейтинг считает ROI иначе - прибыль к затратам, то есть
        // доходность сделки. Числа с двух экранов напрямую не сравнимы.
        if (avgAccount > 0.0) {
            r.roiPercent = 100.0 * static_cast<double>(r.pnlNanos) / avgAccount;
            r.roiKnown = true;
        }
        rows.push_back(r);
    }
    sqlite3_finalize(s);
    return rows;
}

void rankingSnapshot(std::vector<PerpRow>& localCopy) {
    // Пересчёт делаем ВНЕ блокировки кэша. Иначе один пользователь, открывший
    // рейтинг в момент, когда дозагрузчик держит базу пачкой вставок, вешал бы
    // экран рейтинга всем остальным: он ждал бы базу, удерживая кэш.
    bool needRebuild;
    {
        std::lock_guard<std::mutex> l(g_rankMutex);
        needRebuild = g_rankCache.empty() || nowSec() - g_rankBuiltAt >= HL_RANK_CACHE_SEC;
        if (needRebuild) g_rankBuiltAt = nowSec();   // метку ставим сразу, чтобы
    }                                               // соседние запросы не считали то же самое

    if (needRebuild) {
        std::vector<PerpRow> fresh = computeRanking();
        std::lock_guard<std::mutex> l(g_rankMutex);
        g_rankCache.swap(fresh);
        localCopy = g_rankCache;
        return;
    }
    std::lock_guard<std::mutex> l(g_rankMutex);
    localCopy = g_rankCache;
}

enum class PerpKind { PNL, ROI, WINRATE, ACTIVE };

bool parsePerpKind(const std::string& v, PerpKind& out) {
    if (v == "pnl")     { out = PerpKind::PNL;     return true; }
    if (v == "roi")     { out = PerpKind::ROI;     return true; }
    if (v == "winrate") { out = PerpKind::WINRATE; return true; }
    if (v == "active")  { out = PerpKind::ACTIVE;  return true; }
    return false;
}

std::string perpKindStr(PerpKind k) {
    switch (k) {
        case PerpKind::ROI:     return "roi";
        case PerpKind::WINRATE: return "winrate";
        case PerpKind::ACTIVE:  return "active";
        default:                return "pnl";
    }
}

// Заголовок страницы - в точности как у спота, включая эмодзи и ключи
// переводов. Экраны соседние, и своя формулировка для того же самого
// читалась бы как другой рейтинг.
std::string perpTitle(PerpKind k, Lang lang) {
    switch (k) {
        case PerpKind::ROI:     return "\U0001F4C8 " + tr(lang, "rk_top_roi_30d");
        case PerpKind::WINRATE: return "\U0001F3AF " + tr(lang, "rk_top_winrate_30d");
        case PerpKind::ACTIVE:  return "\U0001F504 " + tr(lang, "rk_most_active_30d");
        default:                return "\U0001F4B5 " + tr(lang, "rk_top_pnl_30d");
    }
}

void sortByKind(std::vector<PerpRow>& rows, PerpKind kind) {
    switch (kind) {
        case PerpKind::ROI:
            std::sort(rows.begin(), rows.end(), [](const PerpRow& a, const PerpRow& b) {
                if (a.roiKnown != b.roiKnown) return a.roiKnown;   // неизвестные - вниз
                return a.roiPercent > b.roiPercent;
            });
            break;
        case PerpKind::WINRATE:
            std::sort(rows.begin(), rows.end(), [](const PerpRow& a, const PerpRow& b) {
                if (a.winRatePercent != b.winRatePercent) return a.winRatePercent > b.winRatePercent;
                return a.closedTrades > b.closedTrades;   // при равном винрейте выше тот, кто наторговал больше
            });
            break;
        case PerpKind::ACTIVE:
            std::sort(rows.begin(), rows.end(),
                      [](const PerpRow& a, const PerpRow& b) { return a.closedTrades > b.closedTrades; });
            break;
        default:
            std::sort(rows.begin(), rows.end(),
                      [](const PerpRow& a, const PerpRow& b) { return a.pnlNanos > b.pnlNanos; });
    }
}

std::string rankLabel(int rank) {
    switch (rank) {
        case 1: return "\U0001F947 #1";
        case 2: return "\U0001F948 #2";
        case 3: return "\U0001F949 #3";
        default: return "#" + std::to_string(rank);
    }
}

HlMessage renderPerpPage(const std::string& chatId, PerpKind kind, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    int maxRank = premiumTopTradersLimit(chatId);
    if (maxRank < 1) maxRank = 1;
    const bool showUpgrade = !isPremium(chatId);

    std::vector<PerpRow> rows;
    rankingSnapshot(rows);
    sortByKind(rows, kind);

    const int visible = std::min(static_cast<int>(rows.size()), maxRank);
    const int totalPages = std::max(1, (visible + HL_PER_PAGE - 1) / HL_PER_PAGE);
    page = std::max(1, std::min(page, totalPages));
    const int startIdx = (page - 1) * HL_PER_PAGE;
    const int endIdx = std::min(visible, startIdx + HL_PER_PAGE);

    const char* const dm = dirMark(lang);

    std::stringstream text;
    text << dm << "\U0001F310 <b>Hyperliquid \u2014 " << perpTitle(kind, lang) << "</b>\n\n";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    if (visible == 0) {
        text << dm << "\U0001F4CA " << tr(lang, "hl_rk_empty");
    } else {
        for (int i = startIdx; i < endIdx; i++) {
            const PerpRow& r = rows[static_cast<size_t>(i)];
            const int rank = i + 1;
            text << dm << rankLabel(rank) << "\n";
            text << dm << "<code>" << safeString(r.wallet, 42) << "</code>\n\n";
            text << dm << "\U0001F4B5 <b>PnL:</b> " << formatUsdNanosSigned(r.pnlNanos, true) << "\n";
            if (r.roiKnown)
                text << dm << "\U0001F4C8 <b>" << tr(lang, "hl_rk_roi_account") << ":</b> " << formatPercent(r.roiPercent, true) << "\n";
            text << dm << "\U0001F3AF <b>" << tr(lang, "ws_winrate") << ":</b> " << r.winRatePercent << "%\n";
            text << dm << "\U0001F504 <b>" << tr(lang, "rk_trades") << ":</b> " << r.closedTrades << "\n";
            // Перповый аналог среднего удержания у спота: плечо говорит о
            // манере торговли больше, чем что-либо ещё.
            if (r.avgLeverage > 0.0) {
                text << dm << "\u2699\uFE0F <b>" << tr(lang, "hl_rk_leverage") << ":</b> "
                     << static_cast<int>(r.avgLeverage + 0.5) << "\u00D7\n";
            }
            if (i + 1 < endIdx) text << "\n" << dm << HL_CARD_SEPARATOR << "\n\n";

            keyboard["inline_keyboard"].push_back(json::array({
                {{"text", tr(lang, "rk_track") + " #" + std::to_string(rank)},
                 {"callback_data", "tt_track:" + r.wallet}}
            }));
        }
    }

    if (showUpgrade && visible > 0) {
        text << "\n" << dm << HL_CARD_SEPARATOR << "\n";
        text << dm << tr(lang, "rk_unlock_top100");
        keyboard["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
        }));
    }

    const std::string kindParam = perpKindStr(kind);
    json navRow = json::array();
    if (page > 1)
        navRow.push_back({{"text", "\u2B05\uFE0F"}, {"callback_data", "hl_page:" + kindParam + ":" + std::to_string(page - 1)}});
    navRow.push_back({{"text", std::to_string(page) + "/" + std::to_string(totalPages)}, {"callback_data", "tt_noop"}});
    if (page < totalPages)
        navRow.push_back({{"text", "\u27A1\uFE0F"}, {"callback_data", "hl_page:" + kindParam + ":" + std::to_string(page + 1)}});
    keyboard["inline_keyboard"].push_back(navRow);
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "hl_menu"}}
    }));

    return {text.str(), keyboard.dump()};
}


} // namespace

// ============================== Публичный интерфейс ==============================

bool initHyperliquid() {
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (g_hlDb) return true;

    const std::string file = hlDbFile();
    if (sqlite3_open(file.c_str(), &g_hlDb) != SQLITE_OK) {
        std::cerr << "[HL] не открыть базу " << file << ": "
                  << (g_hlDb ? sqlite3_errmsg(g_hlDb) : "нет памяти") << std::endl;
        if (g_hlDb) { sqlite3_close(g_hlDb); g_hlDb = nullptr; }
        return false;
    }

    const char* schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        // Списка кошельков здесь нет намеренно: он общий со спотом и живёт в
        // главной базе. Дублировать его значило бы иметь два расходящихся
        // источника правды о том, за кем следим.
        //
        // tid у площадки уникален для сделки - первичный ключ по нему сам гасит
        // повторы, неизбежные при перекрытии окон дозагрузки.
        "CREATE TABLE IF NOT EXISTS hl_fills ("
        "  tid INTEGER PRIMARY KEY,"
        "  wallet TEXT NOT NULL,"
        "  coin TEXT NOT NULL DEFAULT '',"
        "  dir TEXT NOT NULL DEFAULT '',"
        "  side TEXT NOT NULL DEFAULT '',"
        "  px TEXT NOT NULL DEFAULT '',"
        "  sz TEXT NOT NULL DEFAULT '',"
        "  closed_pnl_nanos INTEGER NOT NULL DEFAULT 0,"
        "  fee_nanos INTEGER NOT NULL DEFAULT 0,"
        // Нотионал, маржа и плечо на момент сделки. Без них ROI посчитать не
        // из чего: прибыль в $500 при вложенных $100 и при вложенных $50 000 -
        // совершенно разные результаты, а по одной прибыли они неразличимы.
        "  notional_nanos INTEGER NOT NULL DEFAULT 0,"
        "  margin_nanos INTEGER NOT NULL DEFAULT 0,"
        "  leverage INTEGER NOT NULL DEFAULT 0,"
        // Номер ордера. Детектор ботов считает именно ордера: одна крупная
        // заявка может дробиться стаканом на десятки исполнений, и по ним
        // живой человек выглядел бы алгоритмом.
        "  oid INTEGER NOT NULL DEFAULT 0,"
        // Депозит кита на момент сделки. Нужен для второй метрики: насколько
        // он нарастил СЧЁТ, а не сколько снял с одной сделки. Кит с двумя
        // миллионами и прибылью в сто тысяч сделал 5% - и это честнее говорит
        // о его результате, чем средняя доходность сделки.
        "  account_value_nanos INTEGER NOT NULL DEFAULT 0,"
        "  ts INTEGER NOT NULL DEFAULT 0,"
        "  hash TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS idx_hl_fills_wallet_ts ON hl_fills(wallet, ts);"
        // Рейтинг отбирает по времени и группирует по кошельку - индекса по
        // (wallet, ts) для этого мало, начинать надо со времени.
        "CREATE INDEX IF NOT EXISTS idx_hl_fills_ts ON hl_fills(ts);"
        // Служебное состояние дозагрузки. seeded отвечает за то, чтобы при
        // первой встрече кошелька его прошлые сделки попали в базу молча:
        // иначе человек получил бы пачку алертов о том, что случилось до
        // подписки.
        "CREATE TABLE IF NOT EXISTS hl_wallet_state ("
        "  wallet TEXT PRIMARY KEY,"
        "  seeded INTEGER NOT NULL DEFAULT 0,"
        "  last_fill_ts INTEGER NOT NULL DEFAULT 0,"
        "  last_enriched INTEGER NOT NULL DEFAULT 0,"
        "  debounce_sec INTEGER NOT NULL DEFAULT 30);"
        // Бан навсегда, как в споте: алгоритм, один раз опознанный по объёму,
        // человеком уже не станет.
        "CREATE TABLE IF NOT EXISTS hl_banned ("
        "  wallet TEXT PRIMARY KEY,"
        "  banned_at INTEGER NOT NULL DEFAULT 0,"
        "  trades INTEGER NOT NULL DEFAULT 0);";

    char* err = nullptr;
    if (sqlite3_exec(g_hlDb, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[HL] схема не создана: " << (err ? err : "?") << std::endl;
        if (err) sqlite3_free(err);
        sqlite3_close(g_hlDb);
        g_hlDb = nullptr;
        return false;
    }

    // ------------------------- Перенос старых баз -------------------------
    // CREATE TABLE IF NOT EXISTS на существующей таблице не делает НИЧЕГО - он
    // не добавляет колонки, появившиеся позже. База, созданная ранней версией,
    // осталась бы без них, а вставка сделки молча падала бы с "no such column":
    // дозагрузки идут, сделок не прибавляется, алертов нет, и снаружи это
    // выглядит как "киты просто не торгуют".
    //
    // Поэтому каждая колонка, добавленная после первого выпуска, отдельно
    // досоздаётся здесь. Повторный запуск вернёт "duplicate column name" - это
    // ожидаемо и означает, что колонка уже на месте, поэтому ошибку глушим.
    const char* const migrations[] = {
        "ALTER TABLE hl_fills ADD COLUMN notional_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN margin_nanos INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN leverage INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN oid INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE hl_fills ADD COLUMN account_value_nanos INTEGER NOT NULL DEFAULT 0",
    };
    for (const char* sql : migrations) {
        char* mErr = nullptr;
        if (sqlite3_exec(g_hlDb, sql, nullptr, nullptr, &mErr) == SQLITE_OK)
            std::cout << "[HL] база обновлена: " << sql << std::endl;
        if (mErr) sqlite3_free(mErr);
    }

    // Проверка вместо надежды: если после переноса запись сделки всё равно
    // невозможна, лучше сказать об этом громко при старте, чем выяснять по
    // пустому рейтингу через неделю.
    sqlite3_stmt* probe = nullptr;
    if (sqlite3_prepare_v2(g_hlDb,
            "SELECT tid,notional_nanos,margin_nanos,leverage,oid,account_value_nanos"
            " FROM hl_fills LIMIT 1",
            -1, &probe, nullptr) != SQLITE_OK) {
        logCritical(std::string("[HL] таблица сделок непригодна для записи: ")
                    + sqlite3_errmsg(g_hlDb) + " - перпы работать не будут");
        if (probe) sqlite3_finalize(probe);
        sqlite3_close(g_hlDb);
        g_hlDb = nullptr;
        return false;
    }
    sqlite3_finalize(probe);
    return true;
}

void startHyperliquidLoop() {
    if (g_hlRunning.exchange(true)) return;
    reloadWatchedWallets();
    g_feedThread = std::thread(feedLoop);
    g_enrichThread = std::thread(enricherLoop);
}

void stopHyperliquid() {
    g_hlRunning.store(false);
    if (g_feedThread.joinable()) g_feedThread.join();
    if (g_enrichThread.joinable()) g_enrichThread.join();
    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (g_hlDb) {
        sqlite3_close(g_hlDb);
        g_hlDb = nullptr;
    }
}

std::string hyperliquidStatsLine() {
    std::stringstream ss;
    const long long last = g_lastMsgTs.load(std::memory_order_relaxed);
    size_t queued;
    { std::lock_guard<std::mutex> l(g_queueMutex); queued = g_enrichQueue.size(); }

    ss << "\n\n\U0001F310 <b>Hyperliquid</b>\n"
       << (g_connected.load(std::memory_order_relaxed) ? "\u2705 на связи" : "\u274C нет связи")
       << ", рынков: " << g_subscribedCoins.load(std::memory_order_relaxed)
       << "\n\U0001F440 кошельков: " << watchedCount()
       << "\n\U0001F4CA сделок в фиде: " << formatThousands(g_tradesSeen.load(std::memory_order_relaxed))
       << "\n\U0001F3AF попаданий: " << g_hits.load(std::memory_order_relaxed)
       << "\n\U0001F504 дозагрузок: " << g_enriched.load(std::memory_order_relaxed)
       << " (в очереди " << queued << ")"
       << "\n\U0001F4E8 алертов: " << g_alertsSent.load(std::memory_order_relaxed)
       << "\n\U0001F6D1 пропусков по бюджету: " << g_budgetSkips.load(std::memory_order_relaxed)
       << "\n\U0001F916 ботов исключено: " << g_botsBanned.load(std::memory_order_relaxed)
       << "\n\u267B\uFE0F переподключений: " << g_reconnects.load(std::memory_order_relaxed);
    if (last > 0) ss << "\n\u23F1 тишина: " << (nowSec() - last) << "с";
    return ss.str();
}

// ============================== Экраны рейтинга ==============================

HlMessage buildVenueMenu(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "hl_venue_spot")}, {"callback_data", "menu:toptrader_spot"}}
    }));
    // Замок ставим до нажатия: упереться в стену, уже нажав, обиднее.
    const std::string perpLabel = tr(lang, "hl_venue_perp")
                                + (isPremium(chatId) ? "" : "  \U0001F512");
    kb["inline_keyboard"].push_back(json::array({
        {{"text", perpLabel}, {"callback_data", "hl_menu"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));
    // Без разделителя, как в спотовом меню: экраны выбора у обеих площадок
    // должны выглядеть одинаково. Линия остаётся только в карточках рейтинга,
    // где она разделяет трейдеров.
    return {"\U0001F3C6 <b>" + tr(lang, "hl_venue_title") + "</b>\n\n"
            + tr(lang, "hl_venue_choose"), kb.dump()};
}

HlMessage buildPerpTopMenu(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    json kb;
    kb["inline_keyboard"] = json::array();
    const char* kinds[4] = {"pnl", "roi", "winrate", "active"};
    const char* keys[4]  = {"rk_btn_top_pnl", "rk_btn_top_roi",
                            "rk_btn_top_winrate", "rk_btn_most_active"};
    for (int i = 0; i < 4; i++) {
        kb["inline_keyboard"].push_back(json::array({
            {{"text", tr(lang, keys[i])}, {"callback_data", std::string("hl_open:") + kinds[i]}}
        }));
    }
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:toptrader"}}
    }));
    return {"\U0001F310 <b>Hyperliquid \u2014 " + tr(lang, "hl_rk_title") + "</b>\n\n"
            + tr(lang, "hl_rk_choose"), kb.dump()};
}


// Что видит бесплатный пользователь вместо рейтинга. Показываем не голый
// отказ, а из чего состоит перп-карточка: плечо, залог, цена ликвидации и
// готовый PnL - именно того, чего нет в спотовой части, и ради чего подписка.
HlMessage buildPerpLocked(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    std::stringstream t;
    t << "\U0001F310 <b>Hyperliquid \u2014 " << tr(lang, "hl_rk_title") << "</b>\n";
    t << HL_CARD_SEPARATOR << "\n\n";
    t << "\U0001F512 " << tr(lang, "hl_locked_body") << "\n";

    json kb;
    kb["inline_keyboard"] = json::array();
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "mw_upgrade")}, {"callback_data", "menu:premium"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:toptrader"}}
    }));
    return {t.str(), kb.dump()};
}

bool renderHyperliquidView(const std::string& chatId, const std::string& action,
                           const std::string& param, HlMessage& out) {
    // Перпы целиком под подпиской - и меню, и страницы рейтинга.
    if ((action == "hl_menu" || action == "hl_open" || action == "hl_page") &&
        !isPremium(chatId)) {
        out = buildPerpLocked(chatId);
        return true;
    }
    if (action == "hl_menu") { out = buildPerpTopMenu(chatId); return true; }
    if (action == "hl_open") {
        PerpKind kind;
        if (!parsePerpKind(param, kind)) return false;
        out = renderPerpPage(chatId, kind, 1);
        return true;
    }
    if (action == "hl_page") {
        const size_t sep = param.find(':');
        if (sep == std::string::npos) return false;
        PerpKind kind;
        if (!parsePerpKind(param.substr(0, sep), kind)) return false;
        int page = 1;
        try { page = std::stoi(param.substr(sep + 1)); } catch (...) {}
        out = renderPerpPage(chatId, kind, page);
        return true;
    }
    return false;
}

bool handleHyperliquidCallback(const std::string& chatId, const std::string& action,
                               const std::string& param, const std::string& data,
                               long long messageId, const std::string& callbackQueryId) {
    HlMessage msg;
    if (!renderHyperliquidView(chatId, action, param, msg)) return false;
    rememberView(chatId, data);
    replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    return true;
}
