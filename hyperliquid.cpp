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
constexpr int HL_BUDGET_PER_MINUTE = 1000;
constexpr int HL_WEIGHT_USER_FILLS = 20;
constexpr int HL_WEIGHT_CLEARINGHOUSE = 2;
constexpr int HL_WEIGHT_META = 20;

// Кит в разгоне торгует пачками. Дозагружать на каждую сделку незачем: один
// запрос отдаёт диапазон, поэтому ждём паузу и забираем всё скопом.
constexpr long long HL_ENRICH_DEBOUNCE_SEC = 30;

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
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
        if (intPart > (9000000000000LL)) return false;   // заведомо вне разумного
        intPart = intPart * 10 + (s[i] - '0');
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

// ========================= Кэш отслеживаемых адресов =========================
// Фид отдаёт ВСЕ сделки площадки, а нужны единицы. Сверка идёт на каждую
// сделку, поэтому список держим своей копией в памяти, а не берём блокировку
// главной базы тысячи раз в минуту.

std::mutex g_walletsMutex;
std::set<std::string> g_watched;

void reloadWatchedWallets() {
    std::vector<std::string> src = hlWatchedAddresses();
    std::set<std::string> fresh(src.begin(), src.end());
    std::lock_guard<std::mutex> l(g_walletsMutex);
    g_watched.swap(fresh);
}

size_t watchedCount() {
    std::lock_guard<std::mutex> l(g_walletsMutex);
    return g_watched.size();
}

// ============================== Счётчики ==============================

std::atomic<unsigned long long> g_tradesSeen{0};
std::atomic<unsigned long long> g_hits{0};
std::atomic<unsigned long long> g_enriched{0};
std::atomic<unsigned long long> g_alertsSent{0};
std::atomic<unsigned long long> g_budgetSkips{0};
std::atomic<unsigned long long> g_reconnects{0};
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

void queueWallet(const std::string& addr) {
    std::lock_guard<std::mutex> l(g_queueMutex);
    g_enrichQueue.insert(addr);
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

std::vector<std::string> fetchPerpCoins() {
    std::vector<std::string> coins;
    json body;
    body["type"] = "meta";
    body["dex"] = "";
    json j = infoPost(body, HL_WEIGHT_META);
    if (!j.is_object() || !j.contains("universe") || !j["universe"].is_array()) {
        std::cerr << "[HL] meta: неожиданный ответ" << std::endl;
        return coins;
    }
    for (const auto& u : j["universe"]) {
        if (!u.is_object() || !u.contains("name") || !u["name"].is_string()) continue;
        // Делистнутые рынки сделок не дают - подписка на них тратит лимит впустую.
        if (u.value("isDelisted", false)) continue;
        std::string name = u["name"].get<std::string>();
        if (!name.empty()) coins.push_back(name);
    }
    return coins;
}

// ====================== Состояние позиций (REST) ======================
// Плечо и маржа живут не в сделке, а в состоянии счёта: в данных о сделке этих
// полей нет вообще. Запрос дешёвый (вес 2 против 20 у истории сделок), поэтому
// берём его на каждую дозагрузку.

struct PositionInfo {
    bool known = false;
    int leverage = 0;
    bool isolated = false;
    long long marginUsedNanos = 0;
    long long positionValueNanos = 0;
    long long liquidationPxNanos = 0;
    long long accountValueNanos = 0;
};

// При полном закрытии позиция исчезает из ответа, и плеча там больше нет.
// Поэтому последний известный снимок держим в памяти: алерт о закрытии
// показывает плечо, с которым позиция велась.
std::mutex g_posMutex;
std::unordered_map<std::string, PositionInfo> g_lastPositions;  // "адрес|монета"

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

    if (!j.contains("assetPositions") || !j["assetPositions"].is_array()) return;

    std::lock_guard<std::mutex> l(g_posMutex);
    for (const auto& ap : j["assetPositions"]) {
        if (!ap.is_object() || !ap.contains("position") || !ap["position"].is_object()) continue;
        const json& p = ap["position"];
        std::string coin = p.value("coin", std::string());
        if (coin.empty()) continue;

        PositionInfo info;
        info.known = true;
        info.accountValueNanos = accountValue;
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
        if (out.debounce <= 0) out.debounce = HL_ENRICH_DEBOUNCE_SEC;
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

// Сохранение сделки. false - такая уже была: tid у площадки уникален, поэтому
// первичный ключ сам гасит повторы от перекрытия окон дозагрузки. Возврат
// заодно решает, слать ли алерт: о повторе сообщать не надо.
bool saveFill(const std::string& wallet, const json& f, long long closedPnlNanos,
              long long feeNanos, long long notionalNanosVal, const PositionInfo& pos) {
    long long tid = f.value("tid", 0LL);
    if (tid == 0) return false;

    std::lock_guard<std::mutex> l(g_hlDbMutex);
    if (!g_hlDb) return false;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(g_hlDb, &s,
            "INSERT OR IGNORE INTO hl_fills"
            "(tid,wallet,coin,dir,side,px,sz,closed_pnl_nanos,fee_nanos,"
            "notional_nanos,margin_nanos,leverage,ts,hash) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
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
    sqlite3_bind_int64(s, 13, f.value("time", 0LL));
    sqlite3_bind_text(s,  14, hash.c_str(), -1, SQLITE_TRANSIENT);
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

std::string buildHlAlert(const std::string& label, const json& f, long long notionalNanosVal,
                         long long closedPnlNanos, const PositionInfo& pos, Lang lang) {
    const std::string coin = safeString(f.value("coin", std::string("?")), 32);
    const std::string dir  = f.value("dir", std::string());
    const std::string key  = dirKey(dir);

    std::stringstream m;
    m << "\U0001F4BC <b>" << safeString(label) << "</b>\n\n";
    m << dirEmoji(key) << " <b>" << tr(lang, key) << " " << coin << "</b>\n";

    m << "\U0001F4B0 " << tr(lang, "hl_size") << ": <b>" << fmtUsd(notionalNanosVal) << "</b>\n";

    if (pos.known && pos.leverage > 0) {
        m << "\u2699\uFE0F " << tr(lang, "hl_leverage") << ": <b>" << pos.leverage << "\u00D7 "
          << tr(lang, pos.isolated ? "hl_isolated" : "hl_cross") << "</b>\n";
    }
    // Главное отличие перпов от спота: размер позиции и сумма под риском - разные
    // числа. При двадцатом плече сделка на $100 000 стоит киту $5 000 своих.
    if (pos.known && pos.marginUsedNanos > 0) {
        m << "\U0001F4B5 " << tr(lang, "hl_margin") << ": <b>" << fmtUsd(pos.marginUsedNanos) << "</b>\n";
    }

    long long pxNanos = 0;
    if (parseDecimalToNanos(f.value("px", std::string("0")), pxNanos) && pxNanos > 0)
        m << "\U0001F4CD " << tr(lang, "hl_price") << ": <b>" << fmtUsd(pxNanos) << "</b>\n";

    m << "\U0001F4E6 " << tr(lang, "hl_qty") << ": <b>"
      << safeString(f.value("sz", std::string("?")), 32) << " " << coin << "</b>\n";

    // closedPnl отличен от нуля только у закрывающих сделок - у открывающих
    // прибыли ещё нет, и строка была бы обманчивым "+$0".
    if (closedPnlNanos != 0) {
        m << (closedPnlNanos >= 0 ? "\U0001F4C8 " : "\U0001F4C9 ") << tr(lang, "hl_pnl") << ": <b>"
          << formatUsdNanosSigned(closedPnlNanos, true) << "</b>\n";
    }
    if (pos.known && pos.liquidationPxNanos > 0) {
        m << "\u2620\uFE0F " << tr(lang, "hl_liq") << ": <b>" << fmtUsd(pos.liquidationPxNanos) << "</b>\n";
    }
    if (pos.known && pos.accountValueNanos > 0) {
        m << "\U0001F3E6 " << tr(lang, "hl_account") << ": <b>" << fmtUsd(pos.accountValueNanos) << "</b>\n";
    }

    m << "\n\U0001F310 " << tr(lang, "hl_venue") << ": <b>Hyperliquid</b>";
    // Ссылки на обозреватель нет намеренно: сеть другая, ссылка на BSC-скан
    // здесь вела бы в никуда.
    return m.str();
}

void dispatchHlAlert(const std::string& wallet, const json& f, long long notionalNanosVal,
                     long long closedPnlNanos, const PositionInfo& pos) {
    std::vector<HlRecipient> recipients = hlWatchersFor(wallet);
    if (recipients.empty()) return;

    std::map<std::pair<std::string, Lang>, std::vector<std::string>> byLabelLang;
    for (const HlRecipient& r : recipients) {
        if (r.chatId == SERVICE_CHAT_ID) continue;
        if (notionalNanosVal < 0) continue;
        if (static_cast<uint64_t>(notionalNanosVal) < r.thresholdNanos) continue;
        Lang lang = langFromCode(getUserLanguage(r.chatId));
        byLabelLang[{r.label, lang}].push_back(r.chatId);
    }
    if (byLabelLang.empty()) return;

    for (auto& entry : byLabelLang) {
        std::string msg = buildHlAlert(entry.first.first, f, notionalNanosVal,
                                       closedPnlNanos, pos, entry.first.second);
        if (g_msgQueue.enqueueToRecipients(msg, entry.second))
            g_alertsSent.fetch_add(1, std::memory_order_relaxed);
    }
}

// ============================== Дозагрузка ==============================

void enrichWallet(const std::string& wallet) {
    WalletState st;
    const bool known = loadWalletState(wallet, st);
    const long long now = nowSec();

    if (known && now - st.lastEnriched < st.debounce) {
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
    json fills = infoPost(body, HL_WEIGHT_USER_FILLS);
    if (!fills.is_array()) {
        queueWallet(wallet);   // бюджет исчерпан или площадка не ответила
        return;
    }

    g_enriched.fetch_add(1, std::memory_order_relaxed);

    const bool wasSeeded = known && st.seeded;
    // Плечо и маржу берём одним запросом на весь кошелёк, а не на сделку:
    // состояние счёта общее, а вес запроса тратится каждый раз.
    if (!fills.empty()) fetchAccountState(wallet);

    long long maxTs = st.lastFillTs;
    for (const auto& f : fills) {
        if (!f.is_object()) continue;
        const long long ts = f.value("time", 0LL);
        if (ts > maxTs) maxTs = ts;

        long long closedPnl = 0, fee = 0;
        parseDecimalToNanos(f.value("closedPnl", std::string("0")), closedPnl);
        parseDecimalToNanos(f.value("fee", std::string("0")), fee);

        // Считаем ДО записи: нотионал, маржа и плечо должны лечь в строку
        // сделки, иначе рейтинг потом не сможет посчитать ROI.
        long long notional = 0;
        notionalNanos(f.value("px", std::string("0")), f.value("sz", std::string("0")), notional);
        const PositionInfo pos = lastKnownPosition(wallet, f.value("coin", std::string()));

        const bool isNew = saveFill(wallet, f, closedPnl, fee, notional, pos);
        if (!isNew || !wasSeeded) continue;   // повтор либо первичное наполнение
        if (notional <= 0) continue;

        dispatchHlAlert(wallet, f, notional, closedPnl, pos);
    }

    // Гиперактивный адрес в одиночку съел бы весь бюджет - переводим на редкий опрос.
    if (fills.size() >= static_cast<size_t>(HL_HYPERACTIVE_FILLS)) {
        if (st.debounce < HL_HYPERACTIVE_DEBOUNCE_SEC) {
            st.debounce = HL_HYPERACTIVE_DEBOUNCE_SEC;
            std::cout << "[HL] " << wallet << " торгует слишком часто, опрос раз в "
                      << HL_HYPERACTIVE_DEBOUNCE_SEC << "с" << std::endl;
        }
    }
    st.seeded = true;
    st.lastFillTs = maxTs;
    st.lastEnriched = now;
    saveWalletState(wallet, st);
}

void enricherLoop() {
    while (keepGoing()) {
        std::set<std::string> batch;
        {
            std::lock_guard<std::mutex> l(g_queueMutex);
            batch.swap(g_enrichQueue);
        }
        for (const std::string& w : batch) {
            if (!keepGoing()) break;
            enrichWallet(w);
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

    std::set<std::string> watched;
    {
        std::lock_guard<std::mutex> l(g_walletsMutex);
        watched = g_watched;
    }
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
            bool serviceWatched = false;
            uint64_t minThreshold = 0;
            bool haveUser = false;
            for (const HlRecipient& r : hlWatchersFor(addr)) {
                if (r.chatId == SERVICE_CHAT_ID) { serviceWatched = true; continue; }
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
        if (j.contains("data")) handleTrades(j["data"]);
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
            std::vector<std::string> fresh = fetchPerpCoins();
            if (!fresh.empty()) {
                coins.swap(fresh);
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

constexpr long long HL_RANK_WINDOW_SEC = 30LL * 86400LL;
constexpr int HL_MIN_CLOSED_TRADES = 5;
constexpr int HL_PER_PAGE = 5;
constexpr long long HL_RANK_CACHE_SEC = 300;

struct PerpRow {
    std::string wallet;
    long long pnlNanos = 0;
    double roiPercent = 0.0;
    int winRatePercent = 0;
    int closedTrades = 0;
    long long volumeNanos = 0;
    long long lastTs = 0;
};

std::mutex g_rankMutex;
std::vector<PerpRow> g_rankCache;
long long g_rankBuiltAt = 0;

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
            " SUM(CASE WHEN closed_pnl_nanos != 0 THEN margin_nanos ELSE 0 END),"
            " SUM(notional_nanos),"
            " MAX(ts)"
            " FROM hl_fills WHERE ts >= ? GROUP BY wallet"))
        return rows;
    sqlite3_bind_int64(s, 1, sinceMs);

    while (sqlite3_step(s) == SQLITE_ROW) {
        PerpRow r;
        r.wallet = safeColumnText(s, 0);
        r.pnlNanos = sqlite3_column_int64(s, 1);
        r.closedTrades = sqlite3_column_int(s, 2);
        const int wins = sqlite3_column_int(s, 3);
        const long long margin = sqlite3_column_int64(s, 4);
        r.volumeNanos = sqlite3_column_int64(s, 5);
        r.lastTs = sqlite3_column_int64(s, 6);

        if (r.closedTrades < HL_MIN_CLOSED_TRADES) continue;
        r.winRatePercent = static_cast<int>((100LL * wins) / r.closedTrades);
        // ROI считаем от суммы вложенной маржи, а не от оборота: у перпов
        // оборот раздут плечом и завысил бы результат в десятки раз.
        if (margin > 0)
            r.roiPercent = 100.0 * static_cast<double>(r.pnlNanos) / static_cast<double>(margin);
        rows.push_back(r);
    }
    sqlite3_finalize(s);
    return rows;
}

const std::vector<PerpRow>& rankingSnapshot(std::vector<PerpRow>& localCopy) {
    std::lock_guard<std::mutex> l(g_rankMutex);
    if (nowSec() - g_rankBuiltAt >= HL_RANK_CACHE_SEC || g_rankCache.empty()) {
        g_rankCache = computeRanking();
        g_rankBuiltAt = nowSec();
    }
    localCopy = g_rankCache;
    return localCopy;
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

std::string perpKindTitleKey(PerpKind k) {
    switch (k) {
        case PerpKind::ROI:     return "hl_rk_roi";
        case PerpKind::WINRATE: return "hl_rk_winrate";
        case PerpKind::ACTIVE:  return "hl_rk_active";
        default:                return "hl_rk_pnl";
    }
}

void sortByKind(std::vector<PerpRow>& rows, PerpKind kind) {
    switch (kind) {
        case PerpKind::ROI:
            std::sort(rows.begin(), rows.end(),
                      [](const PerpRow& a, const PerpRow& b) { return a.roiPercent > b.roiPercent; });
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

std::string shortAddr(const std::string& a) {
    if (a.size() < 12) return a;
    return a.substr(0, 6) + "..." + a.substr(a.size() - 4);
}

std::string rankBadge(int rank) {
    if (rank == 1) return "\U0001F947";
    if (rank == 2) return "\U0001F948";
    if (rank == 3) return "\U0001F949";
    return std::to_string(rank) + ".";
}

HlMessage renderPerpPage(const std::string& chatId, PerpKind kind, int page) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    const int maxRank = premiumTopTradersLimit(chatId);
    const bool showUpgrade = !isPremium(chatId);

    std::vector<PerpRow> rows;
    rankingSnapshot(rows);
    sortByKind(rows, kind);
    if (static_cast<int>(rows.size()) > maxRank) rows.resize(static_cast<size_t>(maxRank));

    std::stringstream t;
    t << "\U0001F310 <b>Hyperliquid \u2014 " << tr(lang, perpKindTitleKey(kind)) << "</b>\n";
    t << "<code>\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501</code>\n\n";

    if (rows.empty()) {
        t << tr(lang, "hl_rk_empty");
    } else {
        if (page < 1) page = 1;
        const int totalPages = (static_cast<int>(rows.size()) + HL_PER_PAGE - 1) / HL_PER_PAGE;
        if (page > totalPages) page = totalPages;
        const size_t from = static_cast<size_t>((page - 1) * HL_PER_PAGE);
        const size_t to = std::min(rows.size(), from + HL_PER_PAGE);

        for (size_t i = from; i < to; i++) {
            const PerpRow& r = rows[i];
            t << rankBadge(static_cast<int>(i) + 1) << " <code>" << shortAddr(r.wallet) << "</code>\n";
            t << "   \U0001F4B0 " << formatUsdNanosSigned(r.pnlNanos, true)
              << "   \U0001F4C8 " << formatPercent(r.roiPercent, true) << "\n";
            t << "   \U0001F3AF " << r.winRatePercent << "%   \U0001F504 "
              << r.closedTrades << " " << tr(lang, "hl_rk_trades") << "\n\n";
        }
        t << tr(lang, "hl_rk_page") << " " << page << "/" << totalPages;
    }
    if (showUpgrade) t << "\n\n" << tr(lang, "hl_rk_upgrade");

    json kb;
    kb["inline_keyboard"] = json::array();
    if (!rows.empty()) {
        const int totalPages = (static_cast<int>(rows.size()) + HL_PER_PAGE - 1) / HL_PER_PAGE;
        int shownPage = page < 1 ? 1 : (page > totalPages ? totalPages : page);
        const size_t from = static_cast<size_t>((shownPage - 1) * HL_PER_PAGE);
        const size_t to = std::min(rows.size(), from + HL_PER_PAGE);
        // Отслеживание переиспользует спотовый tt_track: список кошельков общий,
        // значит и операция та же. Свой обработчик здесь был бы вторым описанием
        // одного и того же действия.
        for (size_t i = from; i < to; i++) {
            kb["inline_keyboard"].push_back(json::array({
                {{"text", "\U0001F4CC " + shortAddr(rows[i].wallet)},
                 {"callback_data", "tt_track:" + rows[i].wallet}}
            }));
        }
        json nav = json::array();
        if (page > 1)
            nav.push_back({{"text", "\u25C0"}, {"callback_data", "hl_page:" + perpKindStr(kind) + ":" + std::to_string(page - 1)}});
        if (page < totalPages)
            nav.push_back({{"text", "\u25B6"}, {"callback_data", "hl_page:" + perpKindStr(kind) + ":" + std::to_string(page + 1)}});
        if (!nav.empty()) kb["inline_keyboard"].push_back(nav);
    }
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "hl_menu"}}
    }));
    return {t.str(), kb.dump()};
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
        "  ts INTEGER NOT NULL DEFAULT 0,"
        "  hash TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS idx_hl_fills_wallet_ts ON hl_fills(wallet, ts);"
        // Служебное состояние дозагрузки. seeded отвечает за то, чтобы при
        // первой встрече кошелька его прошлые сделки попали в базу молча:
        // иначе человек получил бы пачку алертов о том, что случилось до
        // подписки.
        "CREATE TABLE IF NOT EXISTS hl_wallet_state ("
        "  wallet TEXT PRIMARY KEY,"
        "  seeded INTEGER NOT NULL DEFAULT 0,"
        "  last_fill_ts INTEGER NOT NULL DEFAULT 0,"
        "  last_enriched INTEGER NOT NULL DEFAULT 0,"
        "  debounce_sec INTEGER NOT NULL DEFAULT 30);";

    char* err = nullptr;
    if (sqlite3_exec(g_hlDb, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[HL] схема не создана: " << (err ? err : "?") << std::endl;
        if (err) sqlite3_free(err);
        sqlite3_close(g_hlDb);
        g_hlDb = nullptr;
        return false;
    }
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
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "hl_venue_perp")}, {"callback_data", "hl_menu"}}
    }));
    kb["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));
    return {"\U0001F3C6 <b>" + tr(lang, "hl_venue_title") + "</b>\n\n" + tr(lang, "hl_venue_choose"),
            kb.dump()};
}

HlMessage buildPerpTopMenu(const std::string& chatId) {
    const Lang lang = langFromCode(getUserLanguage(chatId));
    json kb;
    kb["inline_keyboard"] = json::array();
    const char* kinds[4] = {"pnl", "roi", "winrate", "active"};
    const char* keys[4]  = {"hl_rk_pnl", "hl_rk_roi", "hl_rk_winrate", "hl_rk_active"};
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

std::string buildHlDailyDigest() {
    // В канал пишем по-английски, как и спотовый дайджест: у канала нет
    // одного пользователя, чей язык можно было бы взять.
    const Lang lang = Lang::EN;

    std::vector<PerpRow> rows;
    rankingSnapshot(rows);
    sortByKind(rows, PerpKind::PNL);
    if (rows.empty()) return "";
    if (rows.size() > 5) rows.resize(5);

    std::stringstream t;
    t << "\U0001F310 <b>Hyperliquid \u2014 " << tr(lang, "hl_rk_pnl") << " (30d)</b>\n";
    t << "<code>\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501</code>\n\n";
    for (size_t i = 0; i < rows.size(); i++) {
        const PerpRow& r = rows[i];
        t << rankBadge(static_cast<int>(i) + 1) << " <code>" << shortAddr(r.wallet) << "</code>\n"
          << "   " << formatUsdNanosSigned(r.pnlNanos, true)
          << "   " << formatPercent(r.roiPercent, true)
          << "   " << r.winRatePercent << "%\n\n";
    }
    return t.str();
}

bool renderHyperliquidView(const std::string& chatId, const std::string& action,
                           const std::string& param, HlMessage& out) {
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
    (void)callbackQueryId;
    HlMessage msg;
    if (!renderHyperliquidView(chatId, action, param, msg)) return false;
    rememberView(chatId, data);
    replyInPlace(chatId, messageId, msg.text, msg.keyboard);
    return true;
}
