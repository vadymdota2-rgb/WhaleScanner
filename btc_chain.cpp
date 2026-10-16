/*
 * Сканер сети Bitcoin. Что и зачем — в btc_chain.h.
 *
 * Круг работы:
 *   1. Раз в полминуты узнаём высоту последнего блока у узла Bitcoin
 *      (JSON-RPC getblockcount).
 *   2. Каждый новый блок целиком одним запросом getblock с подробностью 3:
 *      у входов там уже есть адрес и сумма потраченного выхода (prevout), без
 *      них не понять, откуда пришли монеты. Узлы — свой (WHALE_BTC_RPC) и
 *      открытые публичные; не ответил один — спрашиваем следующий. Как на
 *      BSC: данные — сама цепочка, без чужих сервисов и чужой разметки.
 *   3. Разбираем транзакции: с биржи на частный адрес — вывод (покупка),
 *      с частного на биржу — завод (продажа). Всё, что крупнее порога, —
 *      строкой в btc_moves; потоки любого размера — суммой по блоку и бирже
 *      в btc_flow.
 *
 * Чему учимся по дороге (btc_labels):
 *   • совместная трата: входы одной транзакции подписаны одним владельцем,
 *     значит адрес, потраченный вместе с биржевым, — тоже биржевой;
 *   • сбор депозитов: три и больше входа одним выходом в горячий кошелёк —
 *     так биржа сметает адреса пополнения, их и запоминаем;
 *   • перекладка без сдачи: биржа платит клиенту всегда со сдачей, выход без
 *     сдачи — перенос между своими кошельками.
 * Транзакции, похожие на CoinJoin (много одинаковых выходов), не учат ничему:
 *   там входы чужие друг другу.
 */
#include "btc_chain.h"
#include "btc_block.h"
#include "btc_etf.h"
#include "btc_gov.h"
#include "autobase.h"
#include "btc_seeds_book.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <tuple>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>

#include "json.hpp"

using json = nlohmann::json;

extern std::atomic<bool> running;

namespace {

constexpr long long SAT = 100000000LL;

sqlite3* g_btcDb = nullptr;
std::mutex g_btcDbMutex;
std::atomic<bool> g_btcRunning{false};
std::thread g_btcThread;

// Счётчики для /statsbtc — как у BSC в /stats: с запуска бота.
struct BtcStats {
    std::atomic<long long> startedAt{0};
    std::atomic<long long> tip{0}, lag{0}, maxLag{0};
    std::atomic<long long> blocks{0}, txs{0}, ins{0}, outs{0}, moves{0}, alerts{0};
    std::atomic<long long> reorgs{0}, skipped{0}, loadFails{0};
    std::atomic<long long> loadMs{0}, loadMsSum{0};
    std::atomic<long long> bytes{0};
    std::atomic<long long> addrAsked{0}, addrFailed{0};
    // Разбор транзакций: куда отнесена каждая.
    std::atomic<long long> fromEx{0}, toEx{0}, sweep{0}, move{0}, bounce{0}, mixed{0}, coinjoin{0},
        coinbase{0}, plain{0};
    // Последняя самоочистка: когда и сколько снято.
    std::atomic<long long> cleanAt{0}, cleanFlow{0}, cleanAddr{0}, cleanEmpty{0}, cleanOld{0};
} g_bs;

void bump(std::atomic<long long>& c, long long n = 1) { c.fetch_add(n, std::memory_order_relaxed); }

long long envSats(const char* key, double defBtc) {
    const char* v = std::getenv(key);
    double btc = defBtc;
    if (v && *v) {
        try { btc = std::stod(v); } catch (...) {}
    }
    if (btc <= 0) btc = defBtc;
    return static_cast<long long>(btc * SAT);
}

// Строка в btc_moves — с 0,2 биткоина: ниже начинается розница, и доска
// крупных движений утонула бы в ней. Потоки по часам считаются с любой суммы.
const long long MOVE_MIN_SATS = envSats("WHALE_BTC_MOVE_MIN", 0.2);
// Насколько отставание догоняем блок за блоком: сутки. Цена каждого блока —
// на его час (priceAt), поэтому старые блоки считаются в верных долларах.
// Простой дольше суток — прыгаем к свежим и пишем, сколько пропущено.
constexpr long long MAX_BEHIND = 144;
constexpr long long KEEP_SEC = 400LL * 86400LL;
// Выученные адреса без новых встреч забываем через три месяца: адресов
// пополнения биржи сметают десятки тысяч в сутки, хранить их вечно — гигабайты.
// Живой адрес освежается каждой встречей и не пропадает.
constexpr long long LEARNED_TTL = 90LL * 86400LL;
// Сколько транзакций у адреса и что на нём лежит — раз в неделю на адрес.
// Тысячи транзакций у частного кошелька не бывает: это сервис, платёжный шлюз
// или биржа, которой нет в разметке. API убирает такие из досок.
constexpr long long ADDR_TTL = 7LL * 86400LL;
constexpr int ADDR_PER_BLOCK = 60;
// Кандидаты автопоиска проверяются своей очередью и раньше остальных: с
// порогом 0,2 BTC их в блоке сотни, и в общие 60 проверок они не влезали —
// а без проверки «не сервис ли» в базу никого не берём. Непроверенные ждут
// следующих блоков до 6 часов, очередь — не длиннее 5000.
constexpr int AUTO_ADDR_PER_BLOCK = 80;
constexpr long long AUTO_PEND_TTL = 6LL * 3600LL;
// Круг: кошелёк получил с биржи и за сутки вернул ей ту же сумму (минус
// комиссия сети, допуск — 1% или 0,0002 BTC). Это не покупка и продажа, а
// перекладка биржи через свой адрес: у Kraken так ходило 9 «выводов» из 10.
// Пара снимается; со второго круга адрес помечается адресом биржи.
constexpr long long BOUNCE_SEC = 86400;
constexpr long long BOUNCE_TOL_SATS = 20000;
constexpr size_t AUTO_PEND_MAX = 5000;
constexpr long long SERVICE_TXS = 1000;
// База сервисного аккаунта пополняется сама без потолка: каждый, кто вывел
// с биржи крупную сумму и не похож на сервис, остаётся в ней. Потолок можно
// поставить WHALE_BTC_WATCH_MAX (число больше нуля); 0 или пусто — без него.
// Импорт через /import потолком не ограничен никогда.
const long long WATCH_MAX = [] {
    const char* v = std::getenv("WHALE_BTC_WATCH_MAX");
    const long long n = (v && *v) ? std::atoll(v) : 0;
    return n > 0 ? n : 0;
}();
// Кто попадает в базу сам: вывел с биржи от 0,2 биткоина за раз и не
// похож на сервис. Не больше 500 в сутки, поиск выключается /autobase.
// Тот же порог держит в базе молчащих: пока на кошельке от 0,2 BTC, не удаляем.
const long long AUTO_MIN_SATS = envSats("WHALE_BTC_AUTO_MIN", 0.2);
// Движения кошельков базы пишутся с пятидесяти долларов — тот же порог, что
// у сделок BSC (MIN_TRADE_USD_NANOS в ranking.cpp) и у алертов. Цены нет —
// запасной порог в монетах: тысячная биткоина.
constexpr long long WATCH_MIN_USD_NANOS = 50LL * 1000000000LL;
constexpr long long WATCH_MIN_SATS_NO_PRICE = SAT / 1000;

bool watchWorth(long long sats, long long priceNanos) {
    if (priceNanos <= 0) return sats >= WATCH_MIN_SATS_NO_PRICE;
    return static_cast<long double>(sats) * priceNanos / SAT >= WATCH_MIN_USD_NANOS;
}

std::unordered_set<std::string> g_watch;
// Подписки людей: адреса строчными, как в user_whales.
std::unordered_set<std::string> g_follow;
std::mutex g_watchMutex;
std::function<void(const BtcAlert&)> g_alertSink;

std::string lower(const std::string& a) {
    std::string r = a;
    for (auto& ch : r) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return r;
}

long long nowSec() { return static_cast<long long>(std::time(nullptr)); }

std::string btcDbFile() {
    const char* p = std::getenv("WHALE_BTC_DB_FILE");
    return (p && *p) ? std::string(p) : std::string("btc.db");
}

// ── сеть ─────────────────────────────────────────────────────────────────

// Блок с узла — 10–15 МБ JSON. Больше 64 МБ ответа не бывает: обрываем,
// чтобы сломанный или чужой узел не съел память бота.
constexpr size_t BODY_MAX = 64u << 20;

size_t writeCb(void* p, size_t s, size_t n, void* out) {
    auto* body = static_cast<std::string*>(out);
    if (body->size() + s * n > BODY_MAX) return 0;
    body->append(static_cast<char*>(p), s * n);
    return s * n;
}

struct Curl {
    CURL* h = curl_easy_init();
    ~Curl() { if (h) curl_easy_cleanup(h); }
};

// Адрес без логина и пароля — для журнала: у своего узла они могут стоять
// прямо в адресе (http://user:pass@127.0.0.1:8332).
std::string safeUrl(const std::string& url) {
    const auto scheme = url.find("://");
    const size_t from = scheme == std::string::npos ? 0 : scheme + 3;
    const auto at = url.find('@', from);
    const auto slash = url.find('/', from);
    std::string r = url;
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) r = url.substr(0, from) + url.substr(at + 1);
    return r.substr(0, 90);
}

// GET, а с телом — POST JSON. Тело ответа отдаём при любом коде: узел
// Bitcoin присылает ошибку RPC с кодом 500, и её текст нужен.
std::string request(const std::string& url, const std::string* post, long timeout, long* status, bool& ok) {
    ok = false;
    if (status) *status = 0;
    thread_local Curl c;
    if (!c.h) return "";
    curl_easy_reset(c.h);
    std::string body;
    curl_slist* hdr = nullptr;
    curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c.h, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c.h, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c.h, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(c.h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c.h, CURLOPT_NOSIGNAL, 1L);
    if (post) {
        // Узлу некуда перенаправлять: POST по чужому адресу не повторяем.
        hdr = curl_slist_append(hdr, "Content-Type: application/json");
        curl_easy_setopt(c.h, CURLOPT_HTTPHEADER, hdr);
        curl_easy_setopt(c.h, CURLOPT_POSTFIELDS, post->c_str());
        curl_easy_setopt(c.h, CURLOPT_POSTFIELDSIZE, static_cast<long>(post->size()));
    } else {
        curl_easy_setopt(c.h, CURLOPT_FOLLOWLOCATION, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(c.h, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(c.h, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
        curl_easy_setopt(c.h, CURLOPT_MAXREDIRS, 3L);
    }
    // Сжатым блок втрое-вчетверо меньше: 3 МБ вместо 13.
    curl_easy_setopt(c.h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c.h, CURLOPT_USERAGENT, "whalescanner/1.0");
    curl_easy_setopt(c.h, CURLOPT_XFERINFOFUNCTION,
        +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return (running.load(std::memory_order_relaxed) &&
                    g_btcRunning.load(std::memory_order_relaxed)) ? 0 : 1; });
    curl_easy_setopt(c.h, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(c.h);
    if (hdr) curl_slist_free_all(hdr);
    long code = 0;
    curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    curl_off_t got = 0;
    if (curl_easy_getinfo(c.h, CURLINFO_SIZE_DOWNLOAD_T, &got) == CURLE_OK && got > 0) bump(g_bs.bytes, got);
    if (rc != CURLE_OK) {
        std::cerr << "[BTC] " << curl_easy_strerror(rc) << " | " << safeUrl(url) << std::endl;
        return "";
    }
    ok = true;
    return body;
}

std::string fetch(const std::string& url, long timeout = 20, long* status = nullptr) {
    bool ok = false;
    long code = 0;
    std::string body = request(url, nullptr, timeout, &code, ok);
    if (status) *status = code;
    return ok && code == 200 ? body : "";
}

bool sleepFor(int sec) {
    for (int i = 0; i < sec * 10; ++i) {
        if (!running.load(std::memory_order_relaxed) || !g_btcRunning.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

long long parseLL(const std::string& s) {
    if (s.empty() || s.size() > 18) return -1;
    for (char ch : s) if (ch < '0' || ch > '9') return -1;
    return std::stoll(s);
}

bool isHash(const std::string& h) {
    return h.size() == 64 && h.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// ── узлы Bitcoin ─────────────────────────────────────────────────────────

// Свой узел — первым: WHALE_BTC_RPC, адреса через запятую (логин и пароль
// можно прямо в адресе). За ним открытые публичные узлы без ключа. Узлу
// нужен Bitcoin Core 25+ — с ним getblock отдаёт prevout у входов.
const std::vector<std::string>& rpcPool() {
    static const std::vector<std::string> pool = [] {
        std::vector<std::string> r;
        auto add = [&r](std::string u) {
            while (!u.empty() && std::isspace(static_cast<unsigned char>(u.back()))) u.pop_back();
            while (!u.empty() && std::isspace(static_cast<unsigned char>(u.front()))) u.erase(0, 1);
            if (u.rfind("https://", 0) != 0 && u.rfind("http://", 0) != 0) return;
            if (std::find(r.begin(), r.end(), u) == r.end()) r.push_back(u);
        };
        if (const char* v = std::getenv("WHALE_BTC_RPC")) {
            std::string all = v;
            for (size_t from = 0; from <= all.size();) {
                size_t comma = all.find(',', from);
                if (comma == std::string::npos) comma = all.size();
                add(all.substr(from, comma - from));
                from = comma + 1;
            }
        }
        add("https://bitcoin-rpc.publicnode.com");
        add("https://bitcoin.drpc.org");
        return r;
    }();
    return pool;
}

// Узел не ответил или ответил мусором — пять минут спрашиваем других, чтобы
// каждый блок не ждал его таймаута. Ошибка RPC (узел ещё не видел блок) —
// не поломка, её не штрафуем.
constexpr long long NODE_REST_SEC = 300;
struct NodeStat { long long rest = 0, ok = 0, fail = 0, ms = 0, lastOk = 0; };
std::mutex g_nodeMutex;
std::vector<NodeStat> g_nodes;

NodeStat& nodeAt(size_t i) {
    if (g_nodes.size() < rpcPool().size()) g_nodes.resize(rpcPool().size());
    return g_nodes[i];
}

bool nodeResting(size_t i) {
    std::lock_guard<std::mutex> l(g_nodeMutex);
    return nodeAt(i).rest > nowSec();
}

void restNode(size_t i) {
    std::lock_guard<std::mutex> l(g_nodeMutex);
    nodeAt(i).rest = nowSec() + NODE_REST_SEC;
    ++nodeAt(i).fail;
}

void nodeOk(size_t i, long long ms) {
    std::lock_guard<std::mutex> l(g_nodeMutex);
    NodeStat& n = nodeAt(i);
    ++n.ok;
    n.ms += ms;
    n.lastOk = nowSec();
}

// Вызов RPC по очереди узлов. take — проверить и забрать ответ: false —
// ответ не годится (у узла нет нужного), идём к следующему. Узлы на отдыхе —
// последними, а не никогда: лучше медленный ответ, чем никакого.
bool btcRpc(const std::string& method, const json& params, long timeout, const std::function<bool(json&)>& take) {
    const auto& pool = rpcPool();
    const std::string body = json{{"jsonrpc", "1.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump();
    std::vector<size_t> order;
    for (size_t i = 0; i < pool.size(); ++i) if (!nodeResting(i)) order.push_back(i);
    for (size_t i = 0; i < pool.size(); ++i) if (nodeResting(i)) order.push_back(i);
    for (size_t i : order) {
        if (!running.load(std::memory_order_relaxed) || !g_btcRunning.load()) return false;
        bool ok = false;
        long code = 0;
        const auto t0 = std::chrono::steady_clock::now();
        json j = json::parse(request(pool[i], &body, timeout, &code, ok), nullptr, false);
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (ok && j.is_object() && j.contains("error") && !j["error"].is_null()) continue;
        if (!ok || !j.is_object() || !j.contains("result")) {
            if (ok) std::cerr << "[BTC] узел " << safeUrl(pool[i]) << ": " << method << " → HTTP " << code << std::endl;
            restNode(i);
            continue;
        }
        if (take(j["result"])) { nodeOk(i, ms); return true; }
        std::cerr << "[BTC] узел " << safeUrl(pool[i]) << ": " << method << " — ответ не годится" << std::endl;
        restNode(i);
    }
    return false;
}

long long tipHeight() {
    long long h = 0;
    btcRpc("getblockcount", json::array(), 10, [&h](json& r) {
        if (!r.is_number_integer() || r.get<long long>() <= 0) return false;
        h = r.get<long long>();
        return true;
    });
    return h;
}

std::string hashAt(long long height) {
    std::string h;
    btcRpc("getblockhash", json::array({height}), 10, [&h](json& r) {
        if (!r.is_string() || !isHash(r.get<std::string>())) return false;
        h = r.get<std::string>();
        return true;
    });
    return h;
}

// Цена биткоина в нано-долларах. Два открытых источника; держим минуту.
long long g_priceNanos = 0;
long long g_priceAt = 0;

long long btcPriceNanos() {
    if (g_priceNanos > 0 && nowSec() - g_priceAt < 60) return g_priceNanos;
    double p = 0;
    try {
        auto j = json::parse(fetch("https://api.coinbase.com/v2/prices/BTC-USD/spot", 10), nullptr, false);
        if (j.is_object() && j.contains("data") && j["data"].is_object())
            p = std::stod(j["data"].value("amount", "0"));
    } catch (...) {}
    if (p <= 0) {
        try {
            auto j = json::parse(fetch("https://api.kraken.com/0/public/Ticker?pair=XBTUSD", 10), nullptr, false);
            if (j.is_object() && j.contains("result") && j["result"].is_object())
                for (auto& [k, v] : j["result"].items())
                    if (v.contains("c") && v["c"].is_array() && !v["c"].empty())
                        p = std::stod(v["c"][0].get<std::string>());
        } catch (...) {}
    }
    // Ни один не ответил — остаётся последняя известная цена, если она не
    // старше часа. Старее — лучше без долларов, чем с неверными.
    if (p > 1000 && p < 10000000) {
        g_priceNanos = static_cast<long long>(p * 1e9);
        g_priceAt = nowSec();
    } else if (nowSec() - g_priceAt > 3600) {
        g_priceNanos = 0;
    }
    return g_priceNanos;
}

// Цена на время блока. Свежий блок — текущая цена; блок старше получаса
// (догоняем после простоя) — закрытие часовой свечи Coinbase за тот час:
// доллары старого вывода по сегодняшней цене были бы неправдой.
std::map<long long, long long> g_hourPrice;

long long priceAt(long long ts) {
    if (ts <= 0 || nowSec() - ts < 1800) return btcPriceNanos();
    const long long hour = ts - ts % 3600;
    auto it = g_hourPrice.find(hour);
    if (it != g_hourPrice.end()) return it->second;
    auto iso = [](long long t) {
        char buf[32];
        std::time_t tt = static_cast<std::time_t>(t);
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&tt));
        return std::string(buf);
    };
    long long p = 0;
    json j = json::parse(fetch("https://api.exchange.coinbase.com/products/BTC-USD/candles?granularity=3600&start=" +
                               iso(hour) + "&end=" + iso(hour + 3600), 10), nullptr, false);
    if (j.is_array())
        for (const auto& c : j)
            if (c.is_array() && c.size() >= 5 && c[0].is_number() && c[0].get<long long>() == hour && c[4].is_number())
                p = static_cast<long long>(c[4].get<double>() * 1e9);
    if (p <= 0) return btcPriceNanos();
    if (g_hourPrice.size() > 500) g_hourPrice.clear();
    g_hourPrice[hour] = p;
    return p;
}

// ── блок ─────────────────────────────────────────────────────────────────

long long jll(const json& j, const char* k) {
    if (!j.contains(k)) return 0;
    const auto& v = j[k];
    if (v.is_number_integer()) return v.get<long long>();
    if (v.is_number()) return static_cast<long long>(v.get<double>());
    return 0;
}

std::string jstr(const json& j, const char* k) {
    if (!j.contains(k) || !j[k].is_string()) return "";
    return j[k].get<std::string>();
}

// Сумма из ответа узла: биткоины числом с восемью знаками → сатоши.
// Не число или вне 0…21 млн — -1, такой блок не берём.
long long btcSats(const json& v) {
    if (!v.is_number()) return -1;
    const double d = v.get<double>();
    if (!(d >= 0 && d <= 21e6)) return -1;
    return std::llround(d * static_cast<double>(SAT));
}

// Адрес выхода; у нестандартных и OP_RETURN его нет — пустая строка.
std::string spkAddr(const json& o) {
    if (!o.is_object() || !o.contains("scriptPubKey") || !o["scriptPubKey"].is_object()) return "";
    return jstr(o["scriptPubKey"], "address");
}

// getblock <hash> 3: tx[].vin[].prevout.{value, scriptPubKey.address},
// tx[].vout[].{value, scriptPubKey.address}. Блок проверяем целиком: тот ли
// хэш и высота, в главной ли он цепочке, все ли транзакции на месте и у
// каждого входа есть prevout. Узел старше Bitcoin Core 25 prevout не даёт —
// без него не понять, откуда монеты, и такой ответ не годится.
bool parseRpcBlock(const json& j, Block& b) {
    if (!j.is_object() || jstr(j, "hash") != b.hash || jll(j, "height") != b.height) return false;
    if (j.contains("confirmations") && jll(j, "confirmations") < 1) return false;
    if (!j.contains("tx") || !j["tx"].is_array() || j["tx"].empty()) return false;
    const auto& txs = j["tx"];
    if (j.contains("nTx") && jll(j, "nTx") != static_cast<long long>(txs.size())) return false;
    b.ts = jll(j, "time");
    b.prev = jstr(j, "previousblockhash");
    if (b.ts <= 0 || (b.height > 0 && !isHash(b.prev))) return false;
    std::vector<BTx> out;
    out.reserve(txs.size());
    for (const auto& t : txs) {
        if (!t.is_object() || !t.contains("vin") || !t["vin"].is_array() || !t.contains("vout") || !t["vout"].is_array())
            return false;
        BTx x;
        x.txid = jstr(t, "txid");
        if (!isHash(x.txid)) return false;
        for (const auto& i : t["vin"]) {
            if (!i.is_object()) return false;
            if (i.contains("coinbase")) { x.coinbase = true; continue; }
            if (!i.contains("prevout") || !i["prevout"].is_object()) return false;
            const long long sats = btcSats(i["prevout"].value("value", json()));
            if (sats < 0) return false;
            x.in.push_back({spkAddr(i["prevout"]), sats});
        }
        if (x.in.empty()) x.coinbase = true;
        for (const auto& o : t["vout"]) {
            const long long sats = o.is_object() ? btcSats(o.value("value", json())) : -1;
            if (sats < 0) return false;
            x.out.push_back({spkAddr(o), sats});
        }
        out.push_back(std::move(x));
    }
    b.txs = std::move(out);
    return true;
}

// Блок целиком с первого узла, который отдаст его правильным. 90 секунд:
// 3 МБ сжатого JSON на медленном узле.
bool loadFromRpc(Block& b) {
    return btcRpc("getblock", json::array({b.hash, 3}), 90, [&b](json& r) { return parseRpcBlock(r, b); });
}

// ── метки ────────────────────────────────────────────────────────────────

// Проверенные вручную: баланс и число транзакций на mempool.space совпадают
// с публичной разметкой. Сомнительные сюда не попали — ложная метка хуже
// пропущенной: она превращает перекладку биржи в «кита, купившего 5 000 BTC».
//
// Вторая часть — открытая разметка крупнейших адресов bitinfocharts.com
// (холодные кошельки бирж из первой тысячи). Взломы, конфискации и пул
// майнинга Binance сюда не взяты: это не биржевые деньги. От холодного
// кошелька сканер сам доходит до горячего — по совместной трате и сбору.
const std::vector<std::pair<const char*, const char*>> SEEDS = {
    {"34xp4vRoCGJym3xR7yCVPFHoCNxv4Twseo", "Binance"},
    {"3M219KR5vEneNb47ewrPfWyb5jQ2DjxRP6", "Binance"},
    {"3LYJfcfHPXYJreMsASk2jkn69LWEYKzexb", "Binance"},
    {"bc1qm34lsc65zpw79lxes69zkqmk6ee3ewf0j77s3h", "Binance"},
    {"3LQUu4v9z6KNch71j7kbj8GPeAGUo1FW6a", "Binance"},
    {"3FHNBLobJnbCTFTVakh5TXmEneyf5PT61B", "Binance"},
    {"1PJiGp2yDLvUgqeBsuZVCBADArNsk6XEiw", "Binance"},
    {"34HpHYiyQwg69gFmCq2BGHjF1DZnZnBeBP", "Binance"},
    {"395vnFScKQ1ay695C6v7gf89UzoFpx3WuJ", "Binance"},
    {"bc1q7t9fxfaakmtk8pj7tdxjvwsng6y9x76czuaf5h", "Binance"},
    {"3HdGoUTbcztBnS7UzY4vSPYhwr424CiWAA", "Binance"},
    {"3LcgLHzTvjLKBixBvkKGiadtiw2GBSKKqH", "Binance"},
    {"bc1qgdjqv0av3q56jvd82tkdjpy7gdp9ut8tlqmgrpmv24sq90ecnvqqjwvw97", "Bitfinex"},
    {"3JZq4atUahhuA9rLhXLMhhTo133J9rF97j", "Bitfinex"},
    {"bc1ql49ydapnjafl5t2cp9zqpjwe6pdgmxy98859v2", "Robinhood"},
    {"3MgEAFWu1HKSnZ5ZsC8qf61ZW18xrP5pgd", "OKX"},
    {"3FM9vDYsN2iuMPKWjAcqgyahdwdrUxhbJ3", "OKX"},
    {"39wVd42giU95ca39sEPkbPTpWygvsBDuA5", "OKX"},
    {"3E5EPMGRL5PC6YDCLcHLVu9ayC3DysMpau", "OKX"},
    {"1CY7fykRLWXeSbKB885Kr4KjQxmDdvW923", "OKX"},
    {"16rF2zwSJ9goQ9fZfYoti5LsUqqegb5RnA", "OKX"},
    {"1LnoZawVFFQihU8d8ntxLMpYheZUfyeVAK", "OKX"},
    {"bc1qr4dl5wa7kl8yu792dceg9z5knl2gkn220lk7a9", "Crypto.com"},
    {"162bzZT2hJfv5Gm3ZmWfWfHJjCtMD6rHhw", "Gate"},
    {"143gLvWYUojXaWZRrxquRKpVNTkhmr415B", "Huobi"},
    {"bc1qchctnvmdva5z9vrpxkkxck64v7nmzdtyxsrq64", "BitMEX"},
    {"bc1q32lyrhp9zpww22phqjwwmelta0c8a5q990ghs6", "Ceffu"},
    // Bybit сам публикует свои кошельки в аудите резервов (Proof of
    // Reserves, отчёт Hacken от 17 декабря 2025): 23 адреса сети Bitcoin,
    // среди них горячий кошелёк с сотнями тысяч транзакций.
    {"12XZMdaAGmcHf4ocFSqpd8jFd1WH7RHUPs", "Bybit"},
    {"12rFmDggwCNrRL6vuPEjzCDSskTRPjDajP", "Bybit"},
    {"139RYdfv7vNtbnK88juMR3s4pgwvB4db9Y", "Bybit"},
    {"14ug9BVtGnxY8ezvd5cr1PXHeXBYXkpsdw", "Bybit"},
    {"16jVbMCcqq1deKrMB3esL2HPso7kvqUsec", "Bybit"},
    {"1DLeNApsHNNzUMNZJVoXeyEY5sdp8vzx3w", "Bybit"},
    {"1GrwDkr33gT6LuumniYjKEGjTLhsL5kmqC", "Bybit"},
    {"1HDnsptgeJ6fYFHCgd1TFzLSRpuVcV51GL", "Bybit"},
    {"1KHch51TT2eazxYkUXtr38GXtPTjf8Mue", "Bybit"},
    {"1Ko6Sbu8VgZaQNZMfRgNjR8zVzVN9wL7aX", "Bybit"},
    {"1LrsskS2hmLvevKiqASDymS8xRmJ7Gp83u", "Bybit"},
    {"1Nb9NUVWpjruzafXh2uKK5v5xxEEL1hPrH", "Bybit"},
    {"bc1q0npwm7hphq4w3pn0m4nr5hmum2sdg725edylgn", "Bybit"},
    {"bc1q2qqqt87kh33s0er58akh7v9cwjgd83z5smh9rp", "Bybit"},
    {"bc1q59nmn5v9tz36talq7g090yue5kf7actqr62f96kakte70eu2948sw6ddxr", "Bybit"},
    {"bc1q7enk8z5gkuzk2sla4vnmzh5qq8jq6wptx0pty5", "Bybit"},
    {"bc1q8msl0p3ph7dzfn7dxwrhays46cklzpnn8mgyp2", "Bybit"},
    {"bc1qa2eu6p5rl9255e3xz7fcgm6snn4wl5kdfh7zpt05qp5fad9dmsys0qjg0e", "Bybit"},
    {"bc1qmv30sf5tnlx52x6vszl0gmey7vae6elzpm2zxw", "Bybit"},
    {"bc1qpz86ltanq6042k36rs0yl0wjpd6tgu8fwckan7", "Bybit"},
    {"bc1qqc0h2sxt9lvrsqt90rtpjqnjj7qcwv457g28h2", "Bybit"},
    {"bc1qr7dl0rtnfvzkfqrvctpk068c8zluknkzapwhe9", "Bybit"},
    {"bc1qs5vdqkusz4v7qac8ynx0vt9jrekwuupx2fl5udp9jql3sr03z3gsr2mf0f", "Bybit"},
};

// Тип адреса по виду: сдача почти всегда того же типа, что и входы.
int addrType(const std::string& a) {
    if (a.rfind("bc1p", 0) == 0) return 4;
    if (a.rfind("bc1q", 0) == 0) return a.size() > 50 ? 3 : 2;
    if (!a.empty() && a[0] == '3') return 1;
    if (!a.empty() && a[0] == '1') return 0;
    return -1;
}

struct Db {
    sqlite3_stmt* getLabel = nullptr;
    sqlite3_stmt* putLabel = nullptr;
    sqlite3_stmt* putMove = nullptr;
    sqlite3_stmt* putFlow = nullptr;
    sqlite3_stmt* addExBal = nullptr;
    sqlite3_stmt* moveExBal = nullptr;
    ~Db() {
        for (auto* s : {getLabel, putLabel, putMove, putFlow, addExBal, moveExBal})
            if (s) sqlite3_finalize(s);
    }
};

bool prep(sqlite3_stmt** s, const char* sql) {
    if (sqlite3_prepare_v2(g_btcDb, sql, -1, s, nullptr) != SQLITE_OK) {
        std::cerr << "[BTC] prepare: " << sqlite3_errmsg(g_btcDb) << " | " << sql << std::endl;
        return false;
    }
    return true;
}

void bindText(sqlite3_stmt* s, int i, const std::string& v) {
    sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

std::string colText(sqlite3_stmt* s, int i) {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

void execSql(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(g_btcDb, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[BTC] sql: " << (err ? err : "?") << std::endl;
        sqlite3_free(err);
    }
}

// Число транзакций и остаток адреса — с mempool.space, запасной blockstream.
// Это не разметка, а та же цепочка, посчитанная по адресу: у узла Bitcoin
// индекса адресов нет (scantxoutset на публичных узлах закрыт). Со своим
// узлом и индексатором (electrs) сюда встанет он.
bool fetchAddrStats(const std::string& a, long long& txs, long long& bal) {
    bump(g_bs.addrAsked);
    json j;
    for (const char* host : {"https://mempool.space/api/address/", "https://blockstream.info/api/address/"}) {
        j = json::parse(fetch(host + a, 10), nullptr, false);
        if (j.is_object() && j.contains("chain_stats")) break;
    }
    if (!j.is_object() || !j.contains("chain_stats") || !j["chain_stats"].is_object()) {
        bump(g_bs.addrFailed);
        return false;
    }
    const auto& c = j["chain_stats"];
    txs = jll(c, "tx_count");
    bal = jll(c, "funded_txo_sum") - jll(c, "spent_txo_sum");
    return true;
}

// Был ли адрес в цепочке до траты `txid` на высоте `height` — по его истории
// с mempool.space (запасной blockstream), страницами по 25 от новых к старым.
// Нашлась транзакция ниже этой высоты — адрес жил раньше. Дольше 8 страниц
// (200 транзакций после переезда — это не новый кошелёк фонда, а узел) или
// источники молчат — «не узнать». Зовётся под замком базы, редко: страниц
// мало, чтобы замок не держался минутами.
int addrFresh(const std::string& a, const std::string& txid, long long height) {
    std::string last;
    bool self = false;
    for (int page = 0; page < 8; ++page) {
        json j;
        for (const char* host : {"https://mempool.space/api/address/", "https://blockstream.info/api/address/"}) {
            j = json::parse(fetch(host + a + "/txs/chain" + (last.empty() ? "" : "/" + last), 15), nullptr, false);
            if (j.is_array()) break;
        }
        if (!j.is_array()) return -1;
        for (const auto& t : j) {
            if (!t.is_object() || !t.contains("status") || !t["status"].is_object()) return -1;
            const long long h = jll(t["status"], "block_height");
            if (h > 0 && h < height) return 0;
            if (jstr(t, "txid") == txid) self = true;
        }
        if (j.size() < 25) return self ? 1 : -1;
        last = jstr(j.back(), "txid");
        if (!isHash(last)) return -1;
    }
    return -1;
}

void saveAddrStats(const std::string& a, long long txs, long long bal) {
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    sqlite3_stmt* s = nullptr;
    if (prep(&s, "INSERT OR REPLACE INTO btc_addr(address, txs, bal_sats, at) VALUES(?,?,?,?)")) {
        bindText(s, 1, a);
        sqlite3_bind_int64(s, 2, txs);
        sqlite3_bind_int64(s, 3, bal);
        sqlite3_bind_int64(s, 4, nowSec());
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

class Scanner {
public:
    bool init() {
        return prep(&db_.getLabel, "SELECT ex FROM btc_labels WHERE address=?") &&
               prep(&db_.putLabel,
                    "INSERT INTO btc_labels(address, ex, how, at) VALUES(?,?,?,?) "
                    "ON CONFLICT(address) DO UPDATE SET at=excluded.at "
                    "WHERE btc_labels.how != 'seed' AND btc_labels.ex = excluded.ex") &&
               prep(&db_.putMove,
                    "INSERT OR IGNORE INTO btc_moves(txid, height, ts, kind, wallet, ex, sats, usd_nanos, price_nanos) "
                    "VALUES(?,?,?,?,?,?,?,?,?)") &&
               prep(&db_.putFlow,
                    "INSERT INTO btc_flow(ts, ex, in_sats, out_sats, in_n, out_n) VALUES(?,?,?,?,?,?) "
                    "ON CONFLICT(ts, ex) DO UPDATE SET in_sats=in_sats+excluded.in_sats, "
                    "out_sats=out_sats+excluded.out_sats, in_n=in_n+excluded.in_n, out_n=out_n+excluded.out_n") &&
               // Остаток адреса биржи: checked_at 0 — ещё не сверен с цепочкой,
               // -1 — адрес пополнения, сметённый в этом же блоке (остаток ~0,
               // дальше его ведут блоки).
               prep(&db_.addExBal,
                    "INSERT OR IGNORE INTO btc_ex_bal(address, bal_sats, bal_height, checked_at) VALUES(?, 0, ?, ?)") &&
               prep(&db_.moveExBal,
                    "UPDATE btc_ex_bal SET bal_sats = bal_sats + ? WHERE address = ? AND bal_height < ? AND checked_at != 0");
    }

    void seed() {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        // Сначала адреса из отчётов о резервах бирж (btc_seeds_book.h, ~1300
        // адресов 59 бирж), потом ручной список — он главнее и правит их.
        std::vector<std::pair<const char*, const char*>> all(BTC_SEEDS_BOOK.begin(), BTC_SEEDS_BOOK.end());
        all.insert(all.end(), SEEDS.begin(), SEEDS.end());
        execSql("BEGIN");
        for (const auto& [a, ex] : all) {
            putLabel(a, ex, "seed");
            sqlite3_reset(db_.addExBal);
            bindText(db_.addExBal, 1, a);
            sqlite3_bind_int64(db_.addExBal, 2, 0);
            sqlite3_bind_int64(db_.addExBal, 3, 0);
            sqlite3_step(db_.addExBal);
            sqlite3_stmt* s = nullptr;
            // Проверенная метка главнее выученной: если адрес раньше записали
            // другой бирже, правим.
            if (prep(&s, "UPDATE btc_labels SET ex=?, how='seed' WHERE address=?")) {
                bindText(s, 1, ex);
                bindText(s, 2, a);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
        }
        execSql("COMMIT");
    }

    enum class Result { DONE, REORG };

    // Остатки адресов бирж — по каждому блоку: вход с адреса биржи — минус,
    // выход на него — плюс. Блоки до сверки адреса с цепочкой (bal_height) в
    // остаток не идут — сверка их уже учла.
    void exBalances(const Block& b) {
        for (const auto& tx : b.txs) {
            for (const auto& i : tx.in)
                if (!i.addr.empty() && !label(i.addr).empty()) exDelta(i.addr, -i.sats, b.height);
            for (const auto& o : tx.out)
                if (!o.addr.empty() && !label(o.addr).empty()) exDelta(o.addr, o.sats, b.height);
        }
    }

    void exDelta(const std::string& a, long long d, long long height) {
        sqlite3_reset(db_.moveExBal);
        sqlite3_bind_int64(db_.moveExBal, 1, d);
        bindText(db_.moveExBal, 2, a);
        sqlite3_bind_int64(db_.moveExBal, 3, height);
        sqlite3_step(db_.moveExBal);
    }

    Result process(const Block& b) {
        height_ = b.height;
        if (rolledBack(b)) return Result::REORG;
        {
            // Блок уже посчитан (перезапуск, повтор после сбоя сети) — второй
            // раз его потоки не складываем: иначе суммы по биржам удваиваются.
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            sqlite3_stmt* s = nullptr;
            bool seen = false;
            if (prep(&s, "SELECT 1 FROM btc_blocks WHERE height=? AND hash=?")) {
                sqlite3_bind_int64(s, 1, b.height);
                bindText(s, 2, b.hash);
                seen = sqlite3_step(s) == SQLITE_ROW;
                sqlite3_finalize(s);
            }
            if (seen) {
                setState("height", std::to_string(b.height));
                std::cout << "[BTC] block " << b.height << " уже посчитан — пропускаю" << std::endl;
                return Result::DONE;
            }
        }
        touched_.clear();
        touchLabel_.clear();
        reloadWatch();
        const long long price = priceAt(b.ts);
        int moves = 0, learned = 0;
        alerts_.clear();
        {
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            execSql("BEGIN");
            scan(b, price, moves, learned);
            exBalances(b);
            // Разметка владельца (/import из любого процесса бота) — до блока.
            etfDrainLabels(g_btcDb);
            etfBlock(g_btcDb, b);
            govBlock(g_btcDb, b);
            execSql("COMMIT");
        }
        bump(g_bs.blocks);
        bump(g_bs.txs, static_cast<long long>(b.txs.size()));
        for (const auto& tx : b.txs) {
            bump(g_bs.ins, static_cast<long long>(tx.in.size()));
            bump(g_bs.outs, static_cast<long long>(tx.out.size()));
        }
        bump(g_bs.moves, moves);
        addrStats();
        autoWatch();
        sendAlerts();
        std::cout << "[BTC] block " << b.height << ": " << b.txs.size() << " tx, " << moves << " moves, "
                  << learned << " new labels" << std::endl;
        return Result::DONE;
    }

    // Перестройка цепочки: предыдущий блок у нового не тот, что посчитан у
    // нас. Тогда посчитанный блок откатываем целиком — его вклад в потоки,
    // его движения — и читаем эту высоту заново. Без этого транзакции
    // осиротевшего блока, попав в новый, считались бы дважды.
    bool rolledBack(const Block& b) {
        if (b.prev.empty()) return false;
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_stmt* s = nullptr;
        std::string have, flowJson;
        long long ts = 0;
        if (prep(&s, "SELECT hash, ts, COALESCE(flow, '') FROM btc_blocks WHERE height=?")) {
            sqlite3_bind_int64(s, 1, b.height - 1);
            if (sqlite3_step(s) == SQLITE_ROW) {
                have = colText(s, 0);
                ts = sqlite3_column_int64(s, 1);
                flowJson = colText(s, 2);
            }
            sqlite3_finalize(s);
        }
        if (have.empty() || have == b.prev) return false;
        std::cerr << "[BTC] перестройка цепочки на блоке " << b.height - 1 << " — откатываю его" << std::endl;
        execSql("BEGIN");
        json f = json::parse(flowJson, nullptr, false);
        if (f.is_object()) {
            for (auto& [ex, v] : f.items()) {
                if (!v.is_array() || v.size() < 4) continue;
                if (prep(&s, "UPDATE btc_flow SET in_sats=in_sats-?, out_sats=out_sats-?, in_n=in_n-?, out_n=out_n-? "
                             "WHERE ts=? AND ex=?")) {
                    for (int k = 0; k < 4; ++k) sqlite3_bind_int64(s, 1 + k, v[k].get<long long>());
                    sqlite3_bind_int64(s, 5, ts);
                    bindText(s, 6, ex);
                    sqlite3_step(s);
                    sqlite3_finalize(s);
                }
            }
        }
        if (prep(&s, "DELETE FROM btc_moves WHERE height=?")) {
            sqlite3_bind_int64(s, 1, b.height - 1);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        if (prep(&s, "DELETE FROM btc_blocks WHERE height=?")) {
            sqlite3_bind_int64(s, 1, b.height - 1);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        etfRollback(g_btcDb, b.height - 1);
        // Остатки, которые вели блоки, — на сверку: откатанный блок в них уже
        // записан. Резервы бирж — крупные адреса (мелочь сверится за неделю).
        govRollback(g_btcDb);
        if (prep(&s, "UPDATE btc_ex_bal SET checked_at = 0 WHERE checked_at != 0 AND bal_sats >= ?")) {
            sqlite3_bind_int64(s, 1, 10LL * SAT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        setState("height", std::to_string(b.height - 2));
        execSql("COMMIT");
        return true;
    }

    // База могла пополниться из другого процесса бота (/import там, где
    // блоки не читаются). Число строк разошлось с памятью — перечитываем.
    void reloadWatch() {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_stmt* s = nullptr;
        long long n = -1;
        if (prep(&s, "SELECT COUNT(*) FROM btc_watch")) {
            if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
        {
            std::lock_guard<std::mutex> w(g_watchMutex);
            if (n < 0 || n == static_cast<long long>(g_watch.size())) return;
        }
        std::unordered_set<std::string> fresh;
        if (prep(&s, "SELECT address FROM btc_watch")) {
            while (sqlite3_step(s) == SQLITE_ROW) fresh.insert(colText(s, 0));
            sqlite3_finalize(s);
        }
        std::lock_guard<std::mutex> w(g_watchMutex);
        g_watch = std::move(fresh);
    }

    // Разбор блока и запись. Вызывается под замком базы, внутри транзакции.
    void scan(const Block& b, long long price, int& moves, int& learned) {
        std::map<std::string, std::array<long long, 4>> flow;  // биржа → in, out, in_n, out_n
        std::unordered_set<std::string> watch, follow;
        {
            std::lock_guard<std::mutex> w(g_watchMutex);
            watch = g_watch;
            follow = g_follow;
        }
        // Свои — кошельки базы и подписки людей. Для них пишется любое
        // движение от $50, и их выход никогда не считается сдачей биржи.
        auto mine = [&](const std::string& a) {
            return !a.empty() && (watch.count(a) || (!follow.empty() && follow.count(lower(a))));
        };
        for (const auto& tx : b.txs) {
            // Кто уже записан этой транзакцией: одна транзакция — одна строка
            // на кошелёк. Раньше кошелёк базы писался дважды — разбором бирж
            // и отдельным проходом по базе, — и в списке стояли дубли.
            std::unordered_set<std::string> done;
            const bool basic = !(tx.coinbase || tx.in.empty() || tx.out.empty());

            std::string exIn;
            bool mixed = false;
            if (basic) {
                for (const auto& i : tx.in) {
                    std::string e = label(i.addr);
                    if (e.empty()) continue;
                    if (exIn.empty()) exIn = e;
                    else if (e != exIn) mixed = true;
                    touchLabel_.insert(i.addr);
                }
            }
            // Похоже на CoinJoin — но не когда платит биржа: пакетная выплата
            // клиентам с пятью одинаковыми суммами (по 0,01 BTC) выглядит так
            // же, а биржи в CoinJoin не участвуют. Раньше такие выводы терялись.
            const bool plain = basic && (!exIn.empty() || !coinjoin(tx));
            if (!basic) bump(g_bs.coinbase);
            else if (!plain) bump(g_bs.coinjoin);
            else if (mixed) bump(g_bs.mixed);

            if (plain && !mixed && !exIn.empty()) {
                // Со своих адресов биржи. Свой кошелёк, потраченный вместе с
                // биржевым, биржевым не объявляем: его добавили люди.
                for (const auto& i : tx.in)
                    if (!i.addr.empty() && !mine(i.addr) && label(i.addr).empty()) {
                        putLabel(i.addr, exIn, "cospend");
                        ++learned;
                    }

                // Получатели: сумма по адресу — один адрес бывает в выходах
                // дважды, а строка на него одна.
                std::map<std::string, long long> rest;
                bool back = false;
                for (const auto& o : tx.out) {
                    if (o.addr.empty() || o.sats <= 0) continue;
                    std::string e = label(o.addr);
                    if (e == exIn) back = true;
                    else if (e.empty()) rest[o.addr] += o.sats;
                    // на другую биржу — перевод между биржами, не покупка
                }
                if (!back && rest.size() == 1 && tx.out.size() == 1 && !mine(rest.begin()->first)) {
                    // Всё одним выходом без сдачи — перекладка между своими
                    // кошельками: клиенту биржа так не платит.
                    putLabel(rest.begin()->first, exIn, "move");
                    ++learned;
                    bump(g_bs.move);
                    continue;
                }
                bump(g_bs.fromEx);
                if (!back && rest.size() >= 2) {
                    // Сдача ушла на новый адрес биржи — угадываем её: тот же
                    // тип, что у входов, и самая крупная. Свой кошелёк сдачей
                    // не бывает. Не помечаем — только не считаем выводом.
                    int t = addrType(tx.in[0].addr);
                    std::string change;
                    long long best = 0;
                    for (const auto& [a, v] : rest)
                        if (!mine(a) && addrType(a) == t && v > best) { change = a; best = v; }
                    if (!change.empty()) rest.erase(change);
                }
                for (const auto& [a, v] : rest) {
                    auto& f = flow[exIn];
                    f[1] += v;
                    f[3] += 1;
                    if (v >= MOVE_MIN_SATS || (mine(a) && watchWorth(v, price))) {
                        record(tx.txid, b, a, 1, exIn, v, price, follow);
                        ++moves;
                    }
                    done.insert(a);
                    if (v >= AUTO_MIN_SATS && !watch.count(a) && autoCand_.size() < AUTO_PEND_MAX && etfFund(a).empty())
                        autoCand_.emplace(a, nowSec());
                }
            } else if (plain && !mixed) {
                // Входы частные. Есть выход на биржу — завод. Продаёт один
                // владелец (входы подписаны вместе), поэтому строка одна: на
                // свой кошелёк, если он среди входов, иначе на самый крупный.
                const BIn* sender = nullptr;
                for (const auto& i : tx.in)
                    if (mine(i.addr) && (!sender || i.sats > sender->sats)) sender = &i;
                if (!sender) {
                    sender = &tx.in[0];
                    for (const auto& i : tx.in) if (i.sats > sender->sats) sender = &i;
                }
                bool sweep = false;
                if (tx.in.size() >= 3 && tx.out.size() == 1) {
                    std::string e = label(tx.out[0].addr);
                    if (!e.empty()) {
                        // Биржа сметает адреса пополнения в горячий кошелёк.
                        sweep = true;
                        for (const auto& i : tx.in)
                            if (!i.addr.empty() && !mine(i.addr)) { putLabel(i.addr, e, "sweep"); ++learned; }
                    }
                }
                std::map<std::string, long long> toEx;
                std::map<std::string, long long> toExN;
                for (const auto& o : tx.out) {
                    if (o.addr.empty() || o.sats <= 0) continue;
                    std::string e = label(o.addr);
                    if (e.empty()) continue;
                    touchLabel_.insert(o.addr);
                    auto& f = flow[e];
                    f[0] += o.sats;
                    f[2] += 1;
                    toEx[e] += o.sats;
                    toExN[e] += 1;
                }
                // Каждая транзакция — ровно в одну графу разбора; круг
                // ниже переносит её из заводов в круги.
                bump(sweep ? g_bs.sweep : toEx.empty() ? g_bs.plain : g_bs.toEx);
                if (!toEx.empty() && !sender->addr.empty() && !(sweep && !mine(sender->addr))) {
                    // Строка одна на транзакцию: вся сумма на биржи, биржа —
                    // та, куда ушло больше.
                    long long total = 0, best = 0;
                    std::string ex;
                    for (const auto& [e, v] : toEx) { total += v; if (v > best) { best = v; ex = e; } }
                    const bool round = toEx.size() == 1 && unBounce(sender->addr, ex, total, b, flow);
                    if (round && !sweep) { bump(g_bs.toEx, -1); bump(g_bs.bounce); }
                    if (round) {
                        // Круг: ни вывода, ни завода не было — потоки назад.
                        auto& f = flow[ex];
                        f[0] = std::max<long long>(0, f[0] - total);
                        f[2] = std::max<long long>(0, f[2] - toExN[ex]);
                        if (bounces_.size() > 100000) bounces_.clear();
                        if (!mine(sender->addr) && ++bounces_[sender->addr] >= 2) {
                            for (const auto& i : tx.in)
                                if (!i.addr.empty() && !mine(i.addr) && label(i.addr).empty()) {
                                    putLabel(i.addr, ex, "bounce");
                                    forgetAuto(i.addr);
                                    purgeWallet(i.addr, b.ts, &flow);
                                    ++learned;
                                }
                        }
                    } else if (total >= MOVE_MIN_SATS || (mine(sender->addr) && watchWorth(total, price))) {
                        record(tx.txid, b, sender->addr, 2, ex, total, price, follow);
                        ++moves;
                    }
                    // Остальные входы того же владельца — та же продажа.
                    for (const auto& i : tx.in) done.insert(i.addr);
                }
            }

            // Свои кошельки, которых разбор бирж не коснулся, — переводы:
            // монеты пришли не с биржи или ушли не на биржу.
            if (!watch.empty() || !follow.empty()) moves += transfers(tx, b, price, watch, follow, done);
        }

        json blockFlow = json::object();
        for (const auto& [ex, f] : flow) {
            sqlite3_reset(db_.putFlow);
            sqlite3_bind_int64(db_.putFlow, 1, b.ts);
            bindText(db_.putFlow, 2, ex);
            for (int k = 0; k < 4; ++k) sqlite3_bind_int64(db_.putFlow, 3 + k, f[k]);
            sqlite3_step(db_.putFlow);
            blockFlow[ex] = json::array({f[0], f[1], f[2], f[3]});
        }
        // Живые адреса бирж освежаются каждой встречей: иначе выученный
        // горячий кошелёк через 90 дней выпадал бы из разметки, даже если
        // через него идут тысячи выплат в сутки.
        sqlite3_stmt* s = nullptr;
        if (!touchLabel_.empty() && prep(&s, "UPDATE btc_labels SET at=? WHERE address=? AND how != 'seed'")) {
            const long long t = nowSec();
            for (const auto& a : touchLabel_) {
                if (a.empty()) continue;
                sqlite3_reset(s);
                sqlite3_bind_int64(s, 1, t);
                bindText(s, 2, a);
                sqlite3_step(s);
            }
            sqlite3_finalize(s);
        }
        if (prep(&s, "INSERT OR REPLACE INTO btc_blocks(height, hash, ts, txs, moves, learned, price_nanos, at, flow) "
                     "VALUES(?,?,?,?,?,?,?,?,?)")) {
            sqlite3_bind_int64(s, 1, b.height);
            bindText(s, 2, b.hash);
            sqlite3_bind_int64(s, 3, b.ts);
            sqlite3_bind_int64(s, 4, static_cast<long long>(b.txs.size()));
            sqlite3_bind_int64(s, 5, moves);
            sqlite3_bind_int64(s, 6, learned);
            sqlite3_bind_int64(s, 7, price);
            sqlite3_bind_int64(s, 8, nowSec());
            bindText(s, 9, blockFlow.dump());
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        setState("height", std::to_string(b.height));
    }

    std::string state(const std::string& k) {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_stmt* s = nullptr;
        std::string v;
        if (prep(&s, "SELECT v FROM btc_state WHERE k=?")) {
            bindText(s, 1, k);
            if (sqlite3_step(s) == SQLITE_ROW) v = colText(s, 0);
            sqlite3_finalize(s);
        }
        return v;
    }

    // Разовая чистка: круги «с биржи и обратно», записанные до того, как
    // сканер научился их видеть (см. BOUNCE_SEC). Обе строки пары удаляются,
    // потоки возвращаются; кто сделал два круга и больше — адрес биржи.
    // Возвращает, сколько пар снято.
    int cancelBounces() {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        struct Mv { long long id, ts, sats; int kind; };
        std::map<std::pair<std::string, std::string>, std::vector<Mv>> by;
        sqlite3_stmt* q = nullptr;
        if (!prep(&q, "SELECT id, wallet, ex, kind, sats, ts FROM btc_moves WHERE ex != '' ORDER BY ts, id"))
            return 0;
        while (sqlite3_step(q) == SQLITE_ROW)
            by[{colText(q, 1), colText(q, 2)}].push_back(
                {sqlite3_column_int64(q, 0), sqlite3_column_int64(q, 5), sqlite3_column_int64(q, 4),
                 sqlite3_column_int(q, 3)});
        sqlite3_finalize(q);

        std::unordered_set<std::string> keep;  // импорт и подписки — не биржа
        if (prep(&q, "SELECT address FROM btc_watch WHERE src != 'auto'")) {
            while (sqlite3_step(q) == SQLITE_ROW) keep.insert(colText(q, 0));
            sqlite3_finalize(q);
        }
        {
            std::lock_guard<std::mutex> w(g_watchMutex);
            for (const auto& f : g_follow) keep.insert(f);
        }

        sqlite3_stmt* del = nullptr;
        sqlite3_stmt* fo = nullptr;
        sqlite3_stmt* fi = nullptr;
        if (!prep(&del, "DELETE FROM btc_moves WHERE id=?") ||
            !prep(&fo, "UPDATE btc_flow SET out_sats=MAX(0, out_sats-?), out_n=MAX(0, out_n-1) WHERE ts=? AND ex=?") ||
            !prep(&fi, "UPDATE btc_flow SET in_sats=MAX(0, in_sats-?), in_n=MAX(0, in_n-1) WHERE ts=? AND ex=?")) {
            for (auto* x : {del, fo, fi}) if (x) sqlite3_finalize(x);
            return 0;
        }
        auto drop = [&](sqlite3_stmt* f, const Mv& m, const std::string& ex) {
            sqlite3_reset(del);
            sqlite3_bind_int64(del, 1, m.id);
            sqlite3_step(del);
            sqlite3_reset(f);
            sqlite3_bind_int64(f, 1, m.sats);
            sqlite3_bind_int64(f, 2, m.ts);
            bindText(f, 3, ex);
            sqlite3_step(f);
        };
        int pairs = 0, labeled = 0, purged = 0;
        std::vector<std::pair<std::string, std::string>> mark;
        execSql("BEGIN");
        for (auto& [key, mv] : by) {
            const auto& [wallet, ex] = key;
            std::vector<bool> used(mv.size(), false);
            int n = 0;
            for (size_t i = 0; i < mv.size(); ++i) {
                if (mv[i].kind != 2) continue;
                const long long tol = std::max<long long>(mv[i].sats / 100, BOUNCE_TOL_SATS);
                for (size_t j = i; j-- > 0;) {
                    const Mv& w = mv[j];
                    if (mv[i].ts - w.ts > BOUNCE_SEC) break;
                    if (used[j] || w.kind != 1 || w.sats < mv[i].sats || w.sats > mv[i].sats + tol) continue;
                    used[j] = used[i] = true;
                    drop(fo, w, ex);
                    drop(fi, mv[i], ex);
                    ++pairs;
                    ++n;
                    break;
                }
            }
            if (n >= 2 && !keep.count(wallet) && !keep.count(lower(wallet)) && label(wallet).empty())
                mark.emplace_back(wallet, ex);
        }
        for (auto* x : {del, fo, fi}) sqlite3_finalize(x);
        for (const auto& [wallet, ex] : mark) {
            if (!label(wallet).empty()) continue;
            putLabel(wallet, ex, "bounce");
            forgetAuto(wallet);
            purged += purgeWallet(wallet, 0, nullptr);
            ++labeled;
        }
        execSql("COMMIT");
        if (pairs)
            std::cout << "[BTC] сняты круги «с биржи и обратно»: " << pairs << " пар, адресов бирж +" << labeled
                      << ", их прочих строк снято " << purged << std::endl;
        return pairs;
    }

    void resetData() {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        execSql("DELETE FROM btc_flow; DELETE FROM btc_moves; DELETE FROM btc_blocks; "
                "DELETE FROM btc_state WHERE k='height';");
    }

    void setStateLocked(const std::string& k, const std::string& v) {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        setState(k, v);
    }

private:
    Db db_;
    long long height_ = 0;
    std::vector<std::string> touched_;
    // Кандидаты автопоиска → когда найдены. Живут между блоками, пока их не
    // проверят (AUTO_PEND_TTL).
    std::unordered_map<std::string, long long> autoCand_;
    std::unordered_set<std::string> touchLabel_;
    // Кому и по какой транзакции алерт уже ушёл: после отката перестроенного
    // блока его транзакции приходят снова, второй алерт на них не нужен.
    std::unordered_set<std::string> alerted_;
    // Сколько кругов «с биржи и обратно» сделал адрес (см. BOUNCE_SEC).
    std::unordered_map<std::string, int> bounces_;

    // Снять вывод, который кошелёк сейчас возвращает той же бирже: строку
    // удалить, поток вывода вернуть. true — круг найден.
    bool unBounce(const std::string& wallet, const std::string& ex, long long sats, const Block& b,
                  std::map<std::string, std::array<long long, 4>>& flow) {
        sqlite3_stmt* q = nullptr;
        if (!prep(&q, "SELECT id, ts, sats FROM btc_moves WHERE wallet=? AND ex=? AND kind=1 "
                      "AND ts BETWEEN ? AND ? AND sats BETWEEN ? AND ? ORDER BY ts DESC, id DESC LIMIT 1"))
            return false;
        const long long tol = std::max<long long>(sats / 100, BOUNCE_TOL_SATS);
        bindText(q, 1, wallet);
        bindText(q, 2, ex);
        sqlite3_bind_int64(q, 3, b.ts - BOUNCE_SEC);
        sqlite3_bind_int64(q, 4, b.ts);
        sqlite3_bind_int64(q, 5, sats);
        sqlite3_bind_int64(q, 6, sats + tol);
        long long id = 0, ts = 0, out = 0;
        if (sqlite3_step(q) == SQLITE_ROW) {
            id = sqlite3_column_int64(q, 0);
            ts = sqlite3_column_int64(q, 1);
            out = sqlite3_column_int64(q, 2);
        }
        sqlite3_finalize(q);
        if (!id) return false;
        if (prep(&q, "DELETE FROM btc_moves WHERE id=?")) {
            sqlite3_bind_int64(q, 1, id);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
        if (ts == b.ts) {
            auto& f = flow[ex];
            f[1] = std::max<long long>(0, f[1] - out);
            f[3] = std::max<long long>(0, f[3] - 1);
        } else if (prep(&q, "UPDATE btc_flow SET out_sats=MAX(0, out_sats-?), out_n=MAX(0, out_n-1) "
                            "WHERE ts=? AND ex=?")) {
            sqlite3_bind_int64(q, 1, out);
            sqlite3_bind_int64(q, 2, ts);
            bindText(q, 3, ex);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
        return true;
    }

    // Адрес оказался адресом биржи: его прошлые строки — перекладки той же
    // биржи, а не сделки. Строки удаляются, потоки возвращаются. Строки
    // текущего блока ещё не записаны в btc_flow — их правим в `flow`.
    // Возвращает, сколько строк снято.
    int purgeWallet(const std::string& a, long long blockTs,
                    std::map<std::string, std::array<long long, 4>>* flow) {
        struct Row { long long id, ts, sats; int kind; std::string ex; };
        std::vector<Row> rows;
        sqlite3_stmt* q = nullptr;
        if (!prep(&q, "SELECT id, ts, sats, kind, ex FROM btc_moves WHERE wallet=?")) return 0;
        bindText(q, 1, a);
        while (sqlite3_step(q) == SQLITE_ROW)
            rows.push_back({sqlite3_column_int64(q, 0), sqlite3_column_int64(q, 1), sqlite3_column_int64(q, 2),
                            sqlite3_column_int(q, 3), colText(q, 4)});
        sqlite3_finalize(q);
        sqlite3_stmt* del = nullptr;
        sqlite3_stmt* fo = nullptr;
        sqlite3_stmt* fi = nullptr;
        if (!prep(&del, "DELETE FROM btc_moves WHERE id=?") ||
            !prep(&fo, "UPDATE btc_flow SET out_sats=MAX(0, out_sats-?), out_n=MAX(0, out_n-1) WHERE ts=? AND ex=?") ||
            !prep(&fi, "UPDATE btc_flow SET in_sats=MAX(0, in_sats-?), in_n=MAX(0, in_n-1) WHERE ts=? AND ex=?")) {
            for (auto* x : {del, fo, fi}) if (x) sqlite3_finalize(x);
            return 0;
        }
        for (const auto& r : rows) {
            sqlite3_reset(del);
            sqlite3_bind_int64(del, 1, r.id);
            sqlite3_step(del);
            if (r.ex.empty()) continue;  // перевод кошелька базы — в потоки бирж не входил
            if (flow && r.ts == blockTs) {
                auto& f = (*flow)[r.ex];
                const int k = r.kind == 1 ? 1 : 0;
                f[k] = std::max<long long>(0, f[k] - r.sats);
                f[k + 2] = std::max<long long>(0, f[k + 2] - 1);
                continue;
            }
            sqlite3_stmt* f = r.kind == 1 ? fo : fi;
            sqlite3_reset(f);
            sqlite3_bind_int64(f, 1, r.sats);
            sqlite3_bind_int64(f, 2, r.ts);
            bindText(f, 3, r.ex);
            sqlite3_step(f);
        }
        for (auto* x : {del, fo, fi}) sqlite3_finalize(x);
        return static_cast<int>(rows.size());
    }

    // Адрес оказался адресом биржи: из базы сервисного аккаунта его, если
    // туда взял поиск, — вон, и в кандидаты он больше не идёт.
    void forgetAuto(const std::string& a) {
        autoCand_.erase(a);
        sqlite3_stmt* q = nullptr;
        if (prep(&q, "DELETE FROM btc_watch WHERE address=? AND src='auto'")) {
            bindText(q, 1, a);
            sqlite3_step(q);
            const bool gone = sqlite3_changes(g_btcDb) > 0;
            sqlite3_finalize(q);
            if (gone) {
                std::lock_guard<std::mutex> w(g_watchMutex);
                g_watch.erase(a);
            }
        }
    }

    // Движение кошелька базы в транзакции: сколько пришло минус сколько ушло.
    // Плюс — монеты пришли (покупка, если с биржи), минус — ушли.
    // Строка в btc_moves и, если на кошелёк подписаны люди, алерт.
    void record(const std::string& txid, const Block& b, const std::string& wallet, int kind,
                const std::string& ex, long long sats, long long price,
                const std::unordered_set<std::string>& follow) {
        const bool fresh = putMove(txid, b, kind, wallet, ex, sats, price);
        const std::string key = lower(wallet);
        if (!fresh || !follow.count(key)) return;
        if (!alerted_.insert(txid + "|" + wallet).second) return;
        if (alerted_.size() > 50000) alerted_.clear();
        BtcAlert al;
        al.wallet = wallet;
        al.key = key;
        al.txid = txid;
        al.ex = ex;
        al.kind = kind == 1 ? (ex.empty() ? BtcAlert::IN : BtcAlert::BUY)
                            : (ex.empty() ? BtcAlert::OUT : BtcAlert::SELL);
        al.sats = sats;
        al.priceNanos = price;
        al.usdNanos = static_cast<long long>(static_cast<long double>(sats) * price / SAT);
        al.ts = b.ts;
        al.height = b.height;
        alerts_.push_back(std::move(al));
        putCase(wallet);
    }

    // Переводы своих кошельков: сколько пришло минус сколько ушло, если
    // биржи на другой стороне нет. С биржей движение уже записал разбор
    // бирж — второй строки на него не будет.
    int transfers(const BTx& tx, const Block& b, long long price, const std::unordered_set<std::string>& watch,
                  const std::unordered_set<std::string>& follow, const std::unordered_set<std::string>& done) {
        auto mine = [&](const std::string& a) {
            return !a.empty() && !done.count(a) && (watch.count(a) || (!follow.empty() && follow.count(lower(a))));
        };
        std::unordered_map<std::string, long long> net;
        for (const auto& i : tx.in) if (mine(i.addr)) net[i.addr] -= i.sats;
        for (const auto& o : tx.out) if (mine(o.addr)) net[o.addr] += o.sats;
        int n = 0;
        for (const auto& [a, v] : net) {
            const long long q = v > 0 ? v : -v;
            if (!q || !watchWorth(q, price)) continue;
            // Биржа на другой стороне есть, а разбор бирж движение не взял
            // (две биржи во входах, CoinJoin, сметание) — не выдумываем ни
            // покупку, ни перевод.
            bool exSide = false;
            if (v > 0) { for (const auto& i : tx.in) if (i.addr != a && !label(i.addr).empty()) { exSide = true; break; } }
            else { for (const auto& o : tx.out) if (o.addr != a && !label(o.addr).empty()) { exSide = true; break; } }
            if (exSide) continue;
            record(tx.txid, b, a, v > 0 ? 1 : 2, "", q, price, follow);
            ++n;
        }
        return n;
    }

    std::vector<BtcAlert> alerts_;

    // Настоящее написание адреса: в user_whales он строчными, а обозреватель
    // base58 в строчных не найдёт.
    void putCase(const std::string& a) {
        sqlite3_stmt* s = nullptr;
        if (!prep(&s, "INSERT OR IGNORE INTO btc_case(lower, addr) VALUES(?,?)")) return;
        bindText(s, 1, lower(a));
        bindText(s, 2, a);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    // Средняя цена прошлых покупок и результат продажи — по движениям
    // кошелька в btc.db до этой транзакции. Тот же счёт по средней, что в
    // рейтинге API, иначе алерт и доска разошлись бы в цифрах.
    void enrichAlert(BtcAlert& a) {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_stmt* s = nullptr;
        // Только движения с биржей: перевод на свой же холодный кошелёк не
        // продажа, а пополнение со своего — не покупка. Так же считает API.
        if (!prep(&s, "SELECT kind, sats, price_nanos FROM btc_moves WHERE wallet=? AND height < ? "
                      "AND price_nanos > 0 AND ex != '' ORDER BY ts, id")) return;
        bindText(s, 1, a.wallet);
        sqlite3_bind_int64(s, 2, a.height);
        long double held = 0, cost = 0, boughtQ = 0, boughtV = 0;
        while (sqlite3_step(s) == SQLITE_ROW) {
            const long double q = static_cast<long double>(sqlite3_column_int64(s, 1)) / SAT;
            const long double p = static_cast<long double>(sqlite3_column_int64(s, 2));
            if (sqlite3_column_int(s, 0) == 1) {
                held += q;
                cost += q * p;
                boughtQ += q;
                boughtV += q * p;
                ++a.priorBuys;
            } else if (held > 0) {
                const long double sold = std::min(q, held);
                cost -= sold * (cost / held);
                held -= sold;
            }
        }
        sqlite3_finalize(s);
        if (a.kind == BtcAlert::SELL || a.kind == BtcAlert::OUT) {
            if (held > 0 && a.priceNanos > 0) {
                const long double avg = cost / held;
                const long double q = std::min(static_cast<long double>(a.sats) / SAT, held);
                a.avgEntryNanos = static_cast<long long>(avg);
                a.pnlNanos = static_cast<long long>(q * (a.priceNanos - avg));
                a.pnlPct = avg > 0 ? static_cast<double>((a.priceNanos / avg - 1) * 100) : 0.0;
                a.hasPnl = a.kind == BtcAlert::SELL;
            }
        } else if (boughtQ > 0) {
            a.avgEntryNanos = static_cast<long long>(boughtV / boughtQ);
        }
    }

    void sendAlerts() {
        if (alerts_.empty()) return;
        std::function<void(const BtcAlert&)> sink;
        {
            std::lock_guard<std::mutex> w(g_watchMutex);
            sink = g_alertSink;
        }
        // Один алерт на кошелёк, вид и биржу за блок: суммы складываются,
        // ссылка — на самую крупную транзакцию.
        std::map<std::tuple<std::string, int, std::string>, BtcAlert> merged;
        std::map<std::tuple<std::string, int, std::string>, long long> biggest;
        for (auto& a : alerts_) {
            auto key = std::make_tuple(a.wallet, a.kind, a.ex);
            auto it = merged.find(key);
            if (it == merged.end()) {
                biggest[key] = a.sats;
                merged.emplace(key, std::move(a));
                continue;
            }
            BtcAlert& m = it->second;
            if (a.sats > biggest[key]) { biggest[key] = a.sats; m.txid = a.txid; }
            m.sats += a.sats;
            m.usdNanos += a.usdNanos;
            m.txs += 1;
        }
        alerts_.clear();
        for (auto& [k, a] : merged) {
            enrichAlert(a);
            if (sink) { sink(a); bump(g_bs.alerts); }
        }
    }

    // Новые киты в базу: крупный вывод с биржи и не сервис по числу
    // транзакций. Без потолка, если WATCH_MAX не задан.
    void autoWatch() {
        if (autoCand_.empty()) return;
        if (!autoRoom(AutoNet::BTC)) return;  // поиск выключен или лимит дня выбран
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_stmt* q = nullptr;
        sqlite3_stmt* ins = nullptr;
        if (!prep(&q, "SELECT txs FROM btc_addr WHERE address=?") ||
            !prep(&ins, "INSERT OR IGNORE INTO btc_watch(address, src, at) VALUES(?, 'auto', ?)")) {
            if (q) sqlite3_finalize(q);
            return;
        }
        int added = 0;
        const long long now = nowSec();
        for (auto it = autoCand_.begin(); it != autoCand_.end();) {
            const std::string a = it->first;
            if (now - it->second > AUTO_PEND_TTL) { it = autoCand_.erase(it); continue; }
            if (!autoRoom(AutoNet::BTC)) break;
            {
                std::lock_guard<std::mutex> w(g_watchMutex);
                if (WATCH_MAX > 0 && static_cast<long long>(g_watch.size()) >= WATCH_MAX) break;
                if (g_watch.count(a)) { it = autoCand_.erase(it); continue; }
            }
            if (!label(a).empty()) { it = autoCand_.erase(it); continue; }
            sqlite3_reset(q);
            bindText(q, 1, a);
            // Ещё не проверен — ждёт своей очереди в addrStats.
            if (sqlite3_step(q) != SQLITE_ROW) { ++it; continue; }
            const bool service = sqlite3_column_int64(q, 0) >= SERVICE_TXS;
            it = autoCand_.erase(it);
            if (service) continue;
            sqlite3_reset(ins);
            bindText(ins, 1, a);
            sqlite3_bind_int64(ins, 2, nowSec());
            if (sqlite3_step(ins) == SQLITE_DONE && sqlite3_changes(g_btcDb) > 0) {
                std::lock_guard<std::mutex> w(g_watchMutex);
                g_watch.insert(a);
                ++added;
                autoCounted(AutoNet::BTC);
            }
        }
        sqlite3_finalize(q);
        sqlite3_finalize(ins);
        if (added) std::cout << "[BTC] в базу сервисного аккаунта: +" << added << std::endl;
    }

    // Число транзакций и остаток кошельков, попавших в btc_moves этим блоком.
    void addrStats() {
        std::vector<std::string> ask;
        // Кандидаты автопоиска — первыми и своим счётом, старшие раньше.
        // Поиск выключен или лимит дня выбран — их не проверяем вовсе.
        std::vector<std::pair<long long, std::string>> cand;
        if (!autoCand_.empty() && autoRoom(AutoNet::BTC))
            for (const auto& [a, at] : autoCand_) cand.emplace_back(at, a);
        std::sort(cand.begin(), cand.end());
        std::vector<std::string> askAuto;
        {
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            std::unordered_set<std::string> seen;
            sqlite3_stmt* s = nullptr;
            if (!prep(&s, "SELECT at FROM btc_addr WHERE address=?")) return;
            for (const auto& c : cand) {
                if (static_cast<int>(askAuto.size()) >= AUTO_ADDR_PER_BLOCK) break;
                if (!seen.insert(c.second).second) continue;
                sqlite3_reset(s);
                bindText(s, 1, c.second);
                if (sqlite3_step(s) == SQLITE_ROW && nowSec() - sqlite3_column_int64(s, 0) < ADDR_TTL) continue;
                askAuto.push_back(c.second);
            }
            for (const auto& a : touched_) {
                if (!seen.insert(a).second) continue;
                sqlite3_reset(s);
                bindText(s, 1, a);
                if (sqlite3_step(s) == SQLITE_ROW && nowSec() - sqlite3_column_int64(s, 0) < ADDR_TTL) continue;
                ask.push_back(a);
            }
            sqlite3_finalize(s);
        }
        if (static_cast<int>(ask.size()) > ADDR_PER_BLOCK) ask.resize(ADDR_PER_BLOCK);
        ask.insert(ask.begin(), askAuto.begin(), askAuto.end());
        for (const auto& a : ask) {
            if (!running) break;
            long long txs = 0, bal = 0;
            if (!fetchAddrStats(a, txs, bal)) continue;
            saveAddrStats(a, txs, bal);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    void setState(const std::string& k, const std::string& v) {
        sqlite3_stmt* s = nullptr;
        if (prep(&s, "INSERT OR REPLACE INTO btc_state(k, v) VALUES(?,?)")) {
            bindText(s, 1, k);
            bindText(s, 2, v);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }

    std::string label(const std::string& a) {
        if (a.empty()) return "";
        sqlite3_reset(db_.getLabel);
        bindText(db_.getLabel, 1, a);
        return sqlite3_step(db_.getLabel) == SQLITE_ROW ? colText(db_.getLabel, 0) : "";
    }

    void putLabel(const std::string& a, const std::string& ex, const char* how) {
        // Кошелёк фонда биржевым не становится: его ведёт btc_etf.cpp.
        if (!etfFund(a).empty()) return;
        if (std::strcmp(how, "seed") != 0) {
            // Резервы биржи: новый адрес сверим с цепочкой; сметённый адрес
            // пополнения пуст — его ведут блоки с этого.
            const bool swept = std::strcmp(how, "sweep") == 0;
            sqlite3_reset(db_.addExBal);
            bindText(db_.addExBal, 1, a);
            sqlite3_bind_int64(db_.addExBal, 2, swept ? height_ : 0);
            sqlite3_bind_int64(db_.addExBal, 3, swept ? -1 : 0);
            sqlite3_step(db_.addExBal);
        }
        sqlite3_reset(db_.putLabel);
        bindText(db_.putLabel, 1, a);
        bindText(db_.putLabel, 2, ex);
        sqlite3_bind_text(db_.putLabel, 3, how, -1, SQLITE_STATIC);
        sqlite3_bind_int64(db_.putLabel, 4, nowSec());
        sqlite3_step(db_.putLabel);
    }

    // true — строка новая (повтор той же транзакции отбрасывает UNIQUE).
    bool putMove(const std::string& txid, const Block& b, int kind, const std::string& wallet,
                 const std::string& ex, long long sats, long long price) {
        sqlite3_reset(db_.putMove);
        bindText(db_.putMove, 1, txid);
        sqlite3_bind_int64(db_.putMove, 2, b.height);
        sqlite3_bind_int64(db_.putMove, 3, b.ts);
        sqlite3_bind_int(db_.putMove, 4, kind);
        bindText(db_.putMove, 5, wallet);
        bindText(db_.putMove, 6, ex);
        sqlite3_bind_int64(db_.putMove, 7, sats);
        // нано-доллары: сатоши × (цена в нано за биткоин) / 1e8, без переполнения
        long long usd = static_cast<long long>(static_cast<long double>(sats) * price / SAT);
        sqlite3_bind_int64(db_.putMove, 8, usd);
        sqlite3_bind_int64(db_.putMove, 9, price);
        const bool fresh = sqlite3_step(db_.putMove) == SQLITE_DONE && sqlite3_changes(g_btcDb) > 0;
        touched_.push_back(wallet);
        return fresh;
    }

    // Много одинаковых выходов при многих входах — CoinJoin: входы принадлежат
    // разным людям, учиться на них нельзя.
    static bool coinjoin(const BTx& tx) {
        if (tx.in.size() < 5 || tx.out.size() < 5) return false;
        std::unordered_map<long long, int> same;
        int best = 0;
        for (const auto& o : tx.out) best = std::max(best, ++same[o.sats]);
        return best >= 5;
    }

};

bool openDb() {
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    const std::string file = btcDbFile();
    if (sqlite3_open(file.c_str(), &g_btcDb) != SQLITE_OK) {
        std::cerr << "[BTC] не открыть базу " << file << std::endl;
        if (g_btcDb) { sqlite3_close(g_btcDb); g_btcDb = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(g_btcDb, 8000);
    execSql(
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        // Чей адрес. how: seed — проверен вручную, cospend — потрачен вместе
        // с биржевым, sweep — сметён в горячий кошелёк, move — перекладка
        // без сдачи, bounce — круг через свой адрес.
        "CREATE TABLE IF NOT EXISTS btc_labels ("
        "  address TEXT PRIMARY KEY, ex TEXT NOT NULL, how TEXT NOT NULL, at INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_btc_labels_at ON btc_labels(at);"
        // Чужую разметку (walletexplorer) больше не берём: её метки, ответы
        // и кластеры со старых баз стираем.
        "DELETE FROM btc_labels WHERE how = 'we';"
        "DROP TABLE IF EXISTS btc_we;"
        "DROP TABLE IF EXISTS btc_clusters;"
        // kind 1 — вывод с биржи (покупка), 2 — завод на биржу (продажа).
        "CREATE TABLE IF NOT EXISTS btc_moves ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  txid TEXT NOT NULL, height INTEGER NOT NULL, ts INTEGER NOT NULL,"
        "  kind INTEGER NOT NULL, wallet TEXT NOT NULL, ex TEXT NOT NULL DEFAULT '',"
        "  sats INTEGER NOT NULL, usd_nanos INTEGER NOT NULL DEFAULT 0, price_nanos INTEGER NOT NULL DEFAULT 0,"
        "  UNIQUE(txid, wallet, kind));"
        "CREATE INDEX IF NOT EXISTS idx_btc_moves_ts ON btc_moves(ts);"
        "CREATE INDEX IF NOT EXISTS idx_btc_moves_wallet ON btc_moves(wallet, ts);"
        // Поток по блоку и бирже, любого размера. По блоку, а не по часу:
        // окно «час» на часовых корзинах захватывало бы до двух часов.
        "CREATE TABLE IF NOT EXISTS btc_flow ("
        "  ts INTEGER NOT NULL, ex TEXT NOT NULL,"
        "  in_sats INTEGER NOT NULL DEFAULT 0, out_sats INTEGER NOT NULL DEFAULT 0,"
        "  in_n INTEGER NOT NULL DEFAULT 0, out_n INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (ts, ex));"
        // База кошельков сервисного аккаунта. src: import — /import
        // владельца, auto — сканер нашёл крупный вывод с биржи.
        "CREATE TABLE IF NOT EXISTS btc_watch ("
        "  address TEXT PRIMARY KEY, src TEXT NOT NULL DEFAULT 'import', at INTEGER NOT NULL DEFAULT 0);"
        // Как адрес пишется на самом деле: user_whales хранит строчными.
        "CREATE TABLE IF NOT EXISTS btc_case (lower TEXT PRIMARY KEY, addr TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS btc_addr ("
        "  address TEXT PRIMARY KEY, txs INTEGER NOT NULL DEFAULT 0, bal_sats INTEGER NOT NULL DEFAULT 0,"
        "  at INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS btc_blocks ("
        "  height INTEGER PRIMARY KEY, hash TEXT NOT NULL, ts INTEGER NOT NULL,"
        "  txs INTEGER NOT NULL DEFAULT 0, moves INTEGER NOT NULL DEFAULT 0, learned INTEGER NOT NULL DEFAULT 0,"
        "  price_nanos INTEGER NOT NULL DEFAULT 0, at INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS btc_state (k TEXT PRIMARY KEY, v TEXT NOT NULL);"
        // Резервы бирж: остаток каждого адреса биржи (биржа — по btc_labels).
        // Сверка с цепочкой один раз (и раз в неделю у крупных), дальше —
        // по блокам. checked_at: 0 — ещё не сверен, -1 — сметённый адрес
        // пополнения (пуст, ведут блоки).
        "CREATE TABLE IF NOT EXISTS btc_ex_bal ("
        "  address TEXT PRIMARY KEY, bal_sats INTEGER NOT NULL DEFAULT 0, bal_height INTEGER NOT NULL DEFAULT 0,"
        "  checked_at INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_btc_ex_bal_checked ON btc_ex_bal(checked_at);"
        // Резервы на конец суток — для «изменение за 7 дней».
        "CREATE TABLE IF NOT EXISTS btc_ex_day ("
        "  day INTEGER NOT NULL, ex TEXT NOT NULL, sats INTEGER NOT NULL, n INTEGER NOT NULL,"
        "  PRIMARY KEY (day, ex));");
    // Вклад блока в потоки — для отката при перестройке цепочки. Колонка
    // добавлена позже: на старой базе её нет, ошибку «уже есть» глотаем.
    sqlite3_exec(g_btcDb, "ALTER TABLE btc_blocks ADD COLUMN flow TEXT", nullptr, nullptr, nullptr);
    etfOpen(g_btcDb);
    etfSetFreshCheck(addrFresh);
    govOpen(g_btcDb);
    sqlite3_stmt* s = nullptr;
    if (prep(&s, "SELECT address FROM btc_watch")) {
        std::lock_guard<std::mutex> w(g_watchMutex);
        while (sqlite3_step(s) == SQLITE_ROW) g_watch.insert(colText(s, 0));
        sqlite3_finalize(s);
    }
    std::cout << "[BTC] база сервисного аккаунта, кошельков: " << g_watch.size() << std::endl;
    return true;
}

// Сколько строк снял запрос — для /statsbtc.
long long execCount(const std::string& sql, long long arg) {
    sqlite3_stmt* s = nullptr;
    if (!prep(&s, sql.c_str())) return 0;
    sqlite3_bind_int64(s, 1, arg);
    const bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok ? sqlite3_changes(g_btcDb) : 0;
}

// Самоочистка раз в час — как у BSC (processed_tx и сжатие WAL):
//   • вклад блока в потоки нужен только для отката при перестройке; блок
//     глубже FINAL_DEPTH уже не перестроится — снимаем, это самая толстая
//     часть строки блока;
//   • сами блоки — 30 дней, движения и потоки — 400 дней, выученные адреса
//     без встреч — 90 дней;
//   • снимок адреса (число транзакций, остаток) старше недели всё равно
//     спросят заново; держим только тех, кто на досках (btc_moves) или в
//     базе (btc_watch) — их читает API;
//   • строки потоков, обнулённые кругами и откатами;
//   • журнал WAL — обратно в ноль, как у BSC.
constexpr long long FINAL_DEPTH = 12;

void cleanup() {
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    const long long now = nowSec();
    const long long cut = now - KEEP_SEC;
    long long old = 0;
    old += execCount("DELETE FROM btc_moves WHERE ts < ?", cut);
    old += execCount("DELETE FROM btc_flow WHERE ts < ?", cut);
    old += execCount("DELETE FROM btc_labels WHERE how != 'seed' AND at < ?", now - LEARNED_TTL);
    old += execCount("DELETE FROM btc_blocks WHERE ts < ?", now - 30LL * 86400LL);
    const long long flow = execCount(
        "UPDATE btc_blocks SET flow = NULL WHERE flow IS NOT NULL "
        "AND height < (SELECT MAX(height) FROM btc_blocks) - ?", FINAL_DEPTH);
    const long long addr = execCount(
        "DELETE FROM btc_addr WHERE at < ? "
        "AND NOT EXISTS (SELECT 1 FROM btc_watch w WHERE w.address = btc_addr.address) "
        "AND NOT EXISTS (SELECT 1 FROM btc_moves m WHERE m.wallet = btc_addr.address)", now - ADDR_TTL);
    const long long empty = execCount(
        "DELETE FROM btc_flow WHERE in_sats <= 0 AND out_sats <= 0 AND in_n <= 0 AND out_n <= 0 AND ts < ?",
        now - 3600);
    etfCleanup(g_btcDb, cut);
    govSnapshot(g_btcDb, now, cut);
    // Резервы бирж: адрес, забытый разметкой, из резервов уходит; снимок
    // суток — сверенные адреса и сметённые (их ведут блоки).
    if (sqlite3_exec(g_btcDb, "DELETE FROM btc_ex_bal WHERE NOT EXISTS "
                     "(SELECT 1 FROM btc_labels b WHERE b.address = btc_ex_bal.address)",
                     nullptr, nullptr, nullptr) == SQLITE_OK)
        old += sqlite3_changes(g_btcDb);
    old += execCount("DELETE FROM btc_ex_day WHERE day < ?", cut);
    execCount("INSERT OR REPLACE INTO btc_ex_day(day, ex, sats, n) "
              "SELECT ?, b.ex, SUM(MAX(e.bal_sats, 0)), COUNT(*) FROM btc_ex_bal e "
              "JOIN btc_labels b ON b.address = e.address WHERE e.checked_at != 0 GROUP BY b.ex",
              now - now % 86400);
    sqlite3_wal_checkpoint_v2(g_btcDb, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
    g_bs.cleanAt = now;
    g_bs.cleanFlow = flow;
    g_bs.cleanAddr = addr;
    g_bs.cleanEmpty = empty;
    g_bs.cleanOld = old;
    if (flow + addr + empty + old > 0)
        std::cout << "[BTC] очистка: откатов блоков " << flow << ", снимков адресов " << addr
                  << ", пустых потоков " << empty << ", старых строк " << old << std::endl;
}

// Тот же срок, что AUTO_IDLE_SEC в main.cpp: столько без движений —
// кандидат на удаление из базы.
constexpr long long AUTO_IDLE_SEC = 30LL * 86400LL;
constexpr int STALE_PER_PASS = 100;

// Найденный поиском кошелёк без движений месяц остаётся в базе, пока на нём
// от AUTO_MIN_SATS (0,2 BTC). Остаток в btc_addr верен, если снят после
// последнего движения кошелька. Кто двигал деньги после снимка (снимок
// обновляется не чаще раза в неделю), тому снимаем заново здесь — в потоке
// биткоина, не в главном: это запросы в сеть.
void refreshIdleBalances() {
    const long long cut = nowSec() - AUTO_IDLE_SEC;
    std::vector<std::string> ask;
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_stmt* s = nullptr;
        if (!prep(&s, "SELECT w.address FROM btc_watch w LEFT JOIN btc_addr a ON a.address=w.address "
                      "WHERE w.src='auto' AND w.at>0 AND w.at<? "
                      "AND NOT EXISTS (SELECT 1 FROM btc_moves m WHERE m.wallet=w.address AND m.ts>=?) "
                      "AND (a.address IS NULL OR a.at < "
                      "     COALESCE((SELECT MAX(m.ts) FROM btc_moves m WHERE m.wallet=w.address), 0)) "
                      "LIMIT ?"))
            return;
        sqlite3_bind_int64(s, 1, cut);
        sqlite3_bind_int64(s, 2, cut);
        sqlite3_bind_int(s, 3, STALE_PER_PASS);
        while (sqlite3_step(s) == SQLITE_ROW) ask.push_back(colText(s, 0));
        sqlite3_finalize(s);
    }
    int done = 0;
    for (const auto& a : ask) {
        if (!g_btcRunning.load()) break;
        long long txs = 0, bal = 0;
        if (fetchAddrStats(a, txs, bal)) {
            saveAddrStats(a, txs, bal);
            ++done;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (done) std::cout << "[BTC] остаток перепроверен у " << done << " неактивных кошельков базы" << std::endl;
}

// Остатки кошельков фондов — сверка с цепочкой: новые сразу, остальные раз в
// сутки. Высота — у узла прямо перед запросом: остаток с mempool.space верен
// на ней, блоки до неё в него уже не пишутся.
void refreshEtfBalances() {
    std::vector<std::string> stale;
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        stale = etfStale(g_btcDb, 200);
    }
    for (const auto& a : stale) {
        if (!running.load(std::memory_order_relaxed) || !g_btcRunning.load()) return;
        const long long h = tipHeight();
        long long txs = 0, bal = 0;
        if (h <= 0 || !fetchAddrStats(a, txs, bal)) continue;
        {
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            etfSetBalance(g_btcDb, a, bal, h);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// Резервы бирж: остатки адресов из разметки сверяем с цепочкой — новые
// сразу, крупные (от 10 BTC) раз в неделю; между сверками остаток ведут
// блоки (Scanner::exBalances). Высота — у узла прямо перед запросом: остаток
// с mempool.space верен на ней, блоки до неё в него уже не пишутся.
constexpr int EX_BAL_PER_PASS = 20;
constexpr long long EX_BAL_BIG = 10LL * SAT;

// Государства: остатки их адресов — сверкой с цепочкой (btc_gov.h), список
// Сальвадора — с его же Национального биткоин-офиса, дважды в сутки.
void refreshGovBalances() {
    std::vector<std::string> stale;
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        stale = govStale(g_btcDb, 40);
    }
    for (const auto& a : stale) {
        if (!running.load(std::memory_order_relaxed) || !g_btcRunning.load()) return;
        const long long h = tipHeight();
        long long txs = 0, bal = 0;
        if (h <= 0 || !fetchAddrStats(a, txs, bal)) continue;
        {
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            govSetBalance(g_btcDb, a, bal, h);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void readGovOfficial() {
    json j = json::parse(fetch("https://bitcoin.gob.sv/api/v1/wallet/ONBTC", 30), nullptr, false);
    if (!j.is_object()) {
        std::cerr << "[GOV] список Сальвадора (bitcoin.gob.sv) не прочитался — повторю позже" << std::endl;
        return;
    }
    std::vector<std::string> addrs;
    for (auto& [a, v] : j.items())
        if (govAddrOk(a)) addrs.push_back(a);
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    govOfficial(g_btcDb, "SV", addrs);
}

void refreshExBalances() {
    std::vector<std::string> ask;
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_stmt* s = nullptr;
        if (!prep(&s, "SELECT address FROM btc_ex_bal WHERE checked_at = 0 OR (checked_at < ? AND bal_sats >= ?) "
                      "ORDER BY checked_at LIMIT ?"))
            return;
        sqlite3_bind_int64(s, 1, nowSec() - ADDR_TTL);
        sqlite3_bind_int64(s, 2, EX_BAL_BIG);
        sqlite3_bind_int(s, 3, EX_BAL_PER_PASS);
        while (sqlite3_step(s) == SQLITE_ROW) ask.push_back(colText(s, 0));
        sqlite3_finalize(s);
    }
    for (const auto& a : ask) {
        if (!running.load(std::memory_order_relaxed) || !g_btcRunning.load()) return;
        const long long h = tipHeight();
        long long txs = 0, bal = 0;
        if (h <= 0 || !fetchAddrStats(a, txs, bal)) continue;
        {
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            sqlite3_stmt* s = nullptr;
            if (prep(&s, "UPDATE btc_ex_bal SET bal_sats = ?, bal_height = ?, checked_at = ? WHERE address = ?")) {
                sqlite3_bind_int64(s, 1, bal);
                sqlite3_bind_int64(s, 2, h);
                sqlite3_bind_int64(s, 3, nowSec());
                bindText(s, 4, a);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// Догонка фондов: блоки, где двигались кошельки фондов, с `since` до высоты
// `upTo` — только разметкой ETF (сканер бирж их не трогает). Нужна при первом
// запуске (два месяца истории для графика) и после простоя дольше суток.
// Какие это блоки — из истории кошельков на mempool.space: та же цепочка,
// посчитанная по адресу; сами блоки — с узлов, как всегда.
constexpr long long ETF_BACK_SEC = 60LL * 86400LL;

// Страница истории адреса: mempool.space, не ответил — blockstream.info; три
// круга с паузой: общий сервис ограничивает частоту запросов и на десятках
// адресов подряд начинает отказывать.
json addrPage(const std::string& path, bool& ok) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        for (const char* host : {"https://mempool.space/api/address/", "https://blockstream.info/api/address/"}) {
            json j = json::parse(fetch(host + path, 15), nullptr, false);
            if (j.is_array()) {
                ok = true;
                return j;
            }
        }
        if (!sleepFor(5 * (attempt + 1))) break;
    }
    ok = false;
    return json();
}

// Как прошла последняя догонка — для /statsbtc.
std::string g_backStatus = "ещё не было";

// true — история всех кошельков прочитана и все блоки разобраны. Иначе
// догонка повторится через час: пометить её сделанной после отказа сервиса
// значило бы остаться без двух месяцев потоков (так и было: «0 BTC»).
bool etfBackfillFor(const std::vector<std::string>& addrs, long long since, long long upTo) {
    std::set<long long> heights;
    int failed = 0;
    for (const auto& a : addrs) {
        std::string after;
        for (int page = 0; page < 40; ++page) {
            bool ok = false;
            json j = addrPage(a + "/txs/chain" + (after.empty() ? "" : "/" + after), ok);
            if (!ok) {
                ++failed;
                break;
            }
            if (j.empty()) break;
            bool older = false;
            for (const auto& t : j) {
                if (!t.is_object() || !t.contains("status") || !t["status"].is_object()) continue;
                const long long h = jll(t["status"], "block_height");
                if (jll(t["status"], "block_time") < since) { older = true; break; }
                if (h > 0 && h <= upTo) heights.insert(h);
                after = jstr(t, "txid");
            }
            if (older || j.size() < 25 || !isHash(after)) break;
            if (!sleepFor(1)) return false;
        }
        if (!running.load(std::memory_order_relaxed) || !g_btcRunning.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    int done = 0;
    for (long long h : heights) {
        if (!running.load(std::memory_order_relaxed) || !g_btcRunning.load()) return false;
        Block b;
        b.height = h;
        b.hash = hashAt(h);
        if (b.hash.empty() || !loadFromRpc(b)) continue;
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        execSql("BEGIN");
        etfBlock(g_btcDb, b);
        execSql("COMMIT");
        ++done;
    }
    const bool ok = failed == 0 && done == static_cast<int>(heights.size());
    g_backStatus = std::string(ok ? "готово" : "не вся, повтор через час") + " — блоков " + std::to_string(done) +
                   " из " + std::to_string(heights.size()) +
                   (failed ? ", кошельков без истории " + std::to_string(failed) : std::string());
    std::cout << "[ETF] догонка: " << g_backStatus << std::endl;
    return ok;
}

bool etfBackfill(long long since, long long upTo) { return etfBackfillFor(etfAddresses(), since, upTo); }

// Кошельки, размеченные через /import: их два месяца — понемногу, по 100 за
// круг (список бывает в тысячи адресов). Не прочиталось — повтор на
// следующем круге.
constexpr size_t ETF_HIST_PER_PASS = 100;

void backfillImported(long long tip) {
    std::vector<std::string> addrs;
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        addrs = etfHistQueue(g_btcDb, ETF_HIST_PER_PASS);
    }
    if (addrs.empty() || !etfBackfillFor(addrs, nowSec() - ETF_BACK_SEC, tip)) return;
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    etfHistDone(g_btcDb, addrs);
}

// История новых хранилищ хранителя: первая страница их транзакций с
// mempool.space (та же цепочка, посчитанная по адресу) — в переводы для
// разбора по корзинам. Не больше 30 хранилищ за круг.
void readNewVaults() {
    for (const auto& a : etfVaultsToRead(30)) {
        if (!running.load(std::memory_order_relaxed) || !g_btcRunning.load()) return;
        json j = json::parse(fetch("https://mempool.space/api/address/" + a + "/txs/chain", 15), nullptr, false);
        if (!j.is_array())
            j = json::parse(fetch("https://blockstream.info/api/address/" + a + "/txs/chain", 15), nullptr, false);
        if (!j.is_array()) {
            etfVaultRetry(a);
            continue;
        }
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        execSql("BEGIN");
        for (const auto& t : j) {
            if (!t.is_object() || !t.contains("status") || !t["status"].is_object()) continue;
            const long long h = jll(t["status"], "block_height"), ts = jll(t["status"], "block_time");
            if (h <= 0 || ts <= 0 || !t.contains("vin") || !t["vin"].is_array() || !t.contains("vout") ||
                !t["vout"].is_array())
                continue;
            BTx x;
            x.txid = jstr(t, "txid");
            if (!isHash(x.txid)) continue;
            bool ok = true;
            for (const auto& i : t["vin"]) {
                if (i.value("is_coinbase", false)) { x.coinbase = true; continue; }
                if (!i.contains("prevout") || !i["prevout"].is_object()) { ok = false; break; }
                x.in.push_back({jstr(i["prevout"], "scriptpubkey_address"), jll(i["prevout"], "value")});
            }
            for (const auto& o : t["vout"]) x.out.push_back({jstr(o, "scriptpubkey_address"), jll(o, "value")});
            if (ok) etfVaultTx(g_btcDb, h, ts, x);
        }
        etfVaultRead(g_btcDb, a);
        execSql("COMMIT");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void btcLoop() {
    Scanner sc;
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        if (!sc.init()) {
            std::cerr << "[BTC] схема не готова — сеть Bitcoin выключена" << std::endl;
            return;
        }
    }
    sc.seed();
    // Версия данных. Вторая — счёт без дублей: до неё кошелёк базы мог
    // записаться двумя строками на одну продажу, а второй процесс бота —
    // посчитать блок ещё раз. Старые суммы отличить уже нельзя, поэтому
    // потоки и движения один раз начинаются заново. Адреса бирж, база
    // кошельков и всё, что выучено, остаются.
    if (sc.state("data") != "2") {
        sc.resetData();
        sc.setStateLocked("data", "2");
        std::cout << "[BTC] данные потоков и движений начаты заново (версия 2)" << std::endl;
    }
    if (sc.state("bounce") != "1") {
        sc.cancelBounces();
        sc.setStateLocked("bounce", "1");
    }
    g_bs.startedAt = nowSec();
    auto lastBack = std::chrono::steady_clock::now() - std::chrono::hours(2);
    auto lastClean = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto lastBal = std::chrono::steady_clock::now() - std::chrono::minutes(30);
    auto lastGov = std::chrono::steady_clock::now() - std::chrono::hours(12);

    while (running.load(std::memory_order_relaxed) && g_btcRunning.load()) {
        try {
            long long tip = tipHeight();
            if (tip <= 0) { if (!sleepFor(30)) break; continue; }
            g_bs.tip = tip;
            // Версия 2: первая версия помечала догонку сделанной и после
            // отказа сервиса истории — на таких установках она пройдёт заново.
            // Версия 3: добавились кошельки фондов из списков (btc_etf_list.h) —
            // их два месяца тоже догоняются (уже записанное не задвоится).
            if (sc.state("etf_back") != "3" &&
                std::chrono::steady_clock::now() - lastBack > std::chrono::hours(1)) {
                lastBack = std::chrono::steady_clock::now();
                // Сначала остатки кошельков: плату фонду за управление сканер
                // узнаёт по доле от запаса, а без запаса записал бы её оттоком.
                refreshEtfBalances();
                if (etfBackfill(nowSec() - ETF_BACK_SEC, tip)) sc.setStateLocked("etf_back", "3");
            }
            long long last = 0;
            {
                std::string v = sc.state("height");
                last = v.empty() ? 0 : parseLL(v);
            }
            if (last <= 0 || tip - last > MAX_BEHIND) {
                long long from = tip - (last <= 0 ? 1 : 6);
                if (last > 0) {
                    bump(g_bs.skipped, from - last);
                    // Потоки фондов за простой — догонкой, по блокам их кошельков.
                    // Не вышло — полная догонка двух месяцев на следующем круге.
                    if (!etfBackfill(nowSec() - (tip - last) * 600 - 6 * 3600, from))
                        sc.setStateLocked("etf_back", "0");
                }
                if (last > 0)
                    std::cerr << "[BTC] простой дольше суток: блоки " << last + 1 << "–" << from
                              << " пропущены" << std::endl;
                std::cout << "[BTC] start from block " << from + 1 << " (tip " << tip << ")" << std::endl;
                last = from;
                sc.setStateLocked("height", std::to_string(last));
            }
            // Отставание — как Lag у BSC: сколько блоков сети ещё не разобрано.
            auto setLag = [&last, tip] {
                const long long lag = std::max(0LL, tip - last);
                g_bs.lag = lag;
                if (lag > g_bs.maxLag.load()) g_bs.maxLag = lag;
            };
            setLag();
            while (last < tip && running.load(std::memory_order_relaxed) && g_btcRunning.load()) {
                Block b;
                b.height = last + 1;
                b.hash = hashAt(b.height);
                if (b.hash.empty()) break;
                const auto t0 = std::chrono::steady_clock::now();
                const bool loaded = loadFromRpc(b);
                if (loaded) {
                    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    g_bs.loadMs = ms;
                    bump(g_bs.loadMsSum, ms);
                }
                if (!loaded) {
                    bump(g_bs.loadFails);
                    std::cerr << "[BTC] ни один узел не отдал блок " << b.height << ", повторю через полминуты" << std::endl;
                    break;
                }
                if (sc.process(b) == Scanner::Result::REORG) {
                    bump(g_bs.reorgs);
                    last = b.height - 2;
                    setLag();
                    continue;
                }
                last = b.height;
                setLag();
            }
            readNewVaults();
            refreshExBalances();
            if (std::chrono::steady_clock::now() - lastGov > std::chrono::hours(12)) {
                lastGov = std::chrono::steady_clock::now();
                readGovOfficial();
            }
            refreshGovBalances();
            if (std::chrono::steady_clock::now() - lastBal > std::chrono::minutes(30)) {
                refreshEtfBalances();
                backfillImported(tip);
                refreshIdleBalances();
                lastBal = std::chrono::steady_clock::now();
            }
            if (std::chrono::steady_clock::now() - lastClean > std::chrono::hours(1)) {
                cleanup();
                lastClean = std::chrono::steady_clock::now();
            }
        } catch (const std::exception& e) {
            std::cerr << "[BTC] " << e.what() << std::endl;
        }
        if (!sleepFor(30)) break;
    }
}

}  // namespace

void startBtcLoop() {
    const char* off = std::getenv("WHALE_BTC");
    if (off && std::string(off) == "0") {
        std::cout << "[BTC] выключено (WHALE_BTC=0)" << std::endl;
        return;
    }
    if (g_btcRunning.exchange(true)) return;
    if (!openDb()) {
        g_btcRunning.store(false);
        return;
    }
    // Сканер один на базу. Если на сервере запущено несколько ботов (по
    // сети на процесс) с общей btc.db, второй сканер посчитал бы каждый блок
    // ещё раз. Кто первым взял замок файла, тот и читает блоки; остальные
    // только отвечают на /import и /statsbtc.
    static int lockFd = -1;
    lockFd = ::open((btcDbFile() + ".lock").c_str(), O_RDWR | O_CREAT, 0644);
    if (lockFd < 0 || ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        std::cout << "[BTC] блоки уже читает другой процесс — здесь только база и статистика" << std::endl;
        g_btcRunning.store(false);
        return;
    }
    // Сколько кошельков поиск добавил сегодня — для суточного лимита.
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(g_btcDb, "SELECT COUNT(*) FROM btc_watch WHERE src='auto' AND at>=?", -1, &s,
                               nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, static_cast<long long>(time(nullptr)) / 86400 * 86400);
            if (sqlite3_step(s) == SQLITE_ROW) autoSeed(AutoNet::BTC, sqlite3_column_int(s, 0));
            sqlite3_finalize(s);
        }
    }
    g_btcThread = std::thread(btcLoop);
}

double btcAutoMinBtc() { return static_cast<double>(AUTO_MIN_SATS) / 1e8; }

long long btcAutoCount() { return btcWatchCount(true); }

long long btcWatchCount(bool autoOnly) {
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    if (!g_btcDb) return 0;
    sqlite3_stmt* s = nullptr;
    long long n = 0;
    if (prep(&s, autoOnly ? "SELECT COUNT(*) FROM btc_watch WHERE src='auto'" : "SELECT COUNT(*) FROM btc_watch")) {
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return n;
}

int btcPruneAuto(long long cut) {
    // Найденные поиском (src='auto') и ни разу не двигавшие деньги с `cut`:
    // убрать из базы, не банить — крупно выведут с биржи снова, поиск
    // добавит их снова. Импортированные вручную не трогаем.
    // Держателей не трогаем: пока на кошельке от AUTO_MIN_SATS (0,2 BTC), он
    // остаётся, сколько бы ни лежал. Остаток берём, только если снимок
    // свежий — снят после последнего движения; несвежий сначала обновит
    // refreshIdleBalances, до тех пор кошелёк остаётся.
    std::vector<std::string> gone;
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        if (!g_btcDb) return 0;
        sqlite3_stmt* s = nullptr;
        if (prep(&s, "SELECT w.address FROM btc_watch w JOIN btc_addr a ON a.address=w.address "
                     "WHERE w.src='auto' AND w.at>0 AND w.at<? "
                     "AND NOT EXISTS (SELECT 1 FROM btc_moves m WHERE m.wallet=w.address AND m.ts>=?) "
                     "AND a.bal_sats < ? "
                     "AND a.at >= COALESCE((SELECT MAX(m.ts) FROM btc_moves m WHERE m.wallet=w.address), 0)")) {
            sqlite3_bind_int64(s, 1, cut);
            sqlite3_bind_int64(s, 2, cut);
            sqlite3_bind_int64(s, 3, AUTO_MIN_SATS);
            while (sqlite3_step(s) == SQLITE_ROW) gone.push_back(colText(s, 0));
            sqlite3_finalize(s);
        }
        if (!gone.empty() && prep(&s, "DELETE FROM btc_watch WHERE address=? AND src='auto'")) {
            for (const auto& a : gone) {
                sqlite3_reset(s);
                bindText(s, 1, a);
                sqlite3_step(s);
            }
            sqlite3_finalize(s);
        }
    }
    if (!gone.empty()) {
        std::lock_guard<std::mutex> w(g_watchMutex);
        for (const auto& a : gone) g_watch.erase(a);
    }
    return static_cast<int>(gone.size());
}

int btcBanServices() {
    // Найденный поиском кошелёк набрал SERVICE_TXS транзакций — это уже не
    // кит, а сервис, платёжный шлюз или биржа без разметки. Убираем из базы
    // навсегда: поиск такой адрес не возьмёт снова — autoWatch отсекает его
    // по тому же числу транзакций в btc_addr, а оно только растёт.
    // Импортированные вручную не трогаем.
    std::vector<std::pair<std::string, long long>> bad;
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        if (!g_btcDb) return 0;
        sqlite3_stmt* s = nullptr;
        if (prep(&s, "SELECT w.address, a.txs FROM btc_watch w JOIN btc_addr a ON a.address=w.address "
                     "WHERE w.src='auto' AND a.txs>=?")) {
            sqlite3_bind_int64(s, 1, SERVICE_TXS);
            while (sqlite3_step(s) == SQLITE_ROW) bad.emplace_back(colText(s, 0), sqlite3_column_int64(s, 1));
            sqlite3_finalize(s);
        }
        if (!bad.empty() && prep(&s, "DELETE FROM btc_watch WHERE address=? AND src='auto'")) {
            for (const auto& b : bad) {
                sqlite3_reset(s);
                bindText(s, 1, b.first);
                sqlite3_step(s);
            }
            sqlite3_finalize(s);
        }
    }
    if (!bad.empty()) {
        std::lock_guard<std::mutex> w(g_watchMutex);
        for (const auto& b : bad) g_watch.erase(b.first);
    }
    for (const auto& b : bad)
        std::cout << "[BTC] сервис, не кит: " << b.first << " (" << b.second << " транзакций) — убран из базы навсегда"
                  << std::endl;
    return static_cast<int>(bad.size());
}

void stopBtc() {
    g_btcRunning.store(false);
    if (g_btcThread.joinable()) g_btcThread.join();
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    if (g_btcDb) {
        sqlite3_close(g_btcDb);
        g_btcDb = nullptr;
    }
}

namespace {

const char* const BECH32 = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
const char* const BASE58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

unsigned bech32Polymod(const std::vector<int>& v) {
    static const unsigned G[5] = {0x3b6a57b2u, 0x26508e6du, 0x1ea119fau, 0x3d4233ddu, 0x2a1462b3u};
    unsigned c = 1;
    for (int x : v) {
        unsigned top = c >> 25;
        c = ((c & 0x1ffffffu) << 5) ^ static_cast<unsigned>(x);
        for (int i = 0; i < 5; ++i) if ((top >> i) & 1u) c ^= G[i];
    }
    return c;
}

}  // namespace

std::string normBtcAddress(const std::string& raw) {
    std::string a;
    for (char ch : raw) if (!std::isspace(static_cast<unsigned char>(ch))) a += ch;
    if (a.size() > 3 && (a[0] == 'b' || a[0] == 'B') && (a[1] == 'c' || a[1] == 'C') && a[2] == '1')
        for (auto& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return a;
}

bool isBtcAddress(const std::string& raw) {
    if (raw.empty() || raw.size() > 90) return false;
    if (raw[0] == 'b' || raw[0] == 'B') {
        // Bech32: смешанный регистр запрещён стандартом.
        bool lo = false, up = false;
        for (char ch : raw) {
            if (std::islower(static_cast<unsigned char>(ch))) lo = true;
            if (std::isupper(static_cast<unsigned char>(ch))) up = true;
        }
        if (lo && up) return false;
        const std::string a = normBtcAddress(raw);
        if (a.rfind("bc1", 0) != 0 || a.size() < 14 || a.size() > 74) return false;
        std::vector<int> v = {3, 3, 0, 2, 3};  // «bc», развёрнутое для контрольной суммы
        for (size_t i = 3; i < a.size(); ++i) {
            const char* p = std::strchr(BECH32, a[i]);
            if (!p || !a[i]) return false;
            v.push_back(static_cast<int>(p - BECH32));
        }
        const unsigned c = bech32Polymod(v);
        return c == 1u || c == 0x2bc830a3u;  // bech32 (segwit v0) или bech32m (taproot)
    }
    if ((raw[0] != '1' && raw[0] != '3') || raw.size() < 26 || raw.size() > 35) return false;
    for (char ch : raw) if (!ch || !std::strchr(BASE58, ch)) return false;
    return true;
}

bool isBtcKey(const std::string& a) {
    if (a.rfind("bc1", 0) == 0) return isBtcAddress(a);
    if ((a[0] != '1' && a[0] != '3') || a.size() < 26 || a.size() > 35) return false;
    for (char ch : a) if (!std::isalnum(static_cast<unsigned char>(ch))) return false;
    return true;
}

EtfImportResult btcEtfImport(const std::vector<std::string>& addrs, const std::string& fund) {
    std::vector<std::string> norm;
    for (const auto& a : addrs) norm.push_back(normBtcAddress(a));
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    if (!g_btcDb) return {};
    execSql("BEGIN");
    EtfImportResult r = etfImport(g_btcDb, norm, fund);
    execSql("COMMIT");
    return r;
}

BtcImportResult btcImport(const std::vector<std::string>& addrs) {
    BtcImportResult r;
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    if (!g_btcDb) return r;
    sqlite3_stmt* s = nullptr;
    sqlite3_stmt* m = nullptr;
    if (!prep(&s, "INSERT OR IGNORE INTO btc_watch(address, src, at) VALUES(?, 'import', ?)")) return r;
    // Уже в базе, но добавлен поиском — теперь как импортированный: чистка
    // неактивных его больше не уберёт. Метки на экране это не даёт.
    if (!prep(&m, "UPDATE btc_watch SET src = 'import' WHERE address = ? AND src = 'auto'")) {
        sqlite3_finalize(s);
        return r;
    }
    execSql("BEGIN");
    for (const auto& raw : addrs) {
        const std::string a = normBtcAddress(raw);
        sqlite3_reset(s);
        bindText(s, 1, a);
        sqlite3_bind_int64(s, 2, nowSec());
        if (sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(g_btcDb) > 0) {
            ++r.added;
            std::lock_guard<std::mutex> w(g_watchMutex);
            g_watch.insert(a);
            continue;
        }
        sqlite3_reset(m);
        bindText(m, 1, a);
        if (sqlite3_step(m) == SQLITE_DONE && sqlite3_changes(g_btcDb) > 0) ++r.marked;
        else ++r.dup;
    }
    execSql("COMMIT");
    sqlite3_finalize(s);
    sqlite3_finalize(m);
    return r;
}

void btcSetFollowed(std::unordered_set<std::string> lowerAddrs) {
    std::lock_guard<std::mutex> w(g_watchMutex);
    g_follow = std::move(lowerAddrs);
}

void btcSetAlertSink(std::function<void(const BtcAlert&)> sink) {
    std::lock_guard<std::mutex> w(g_watchMutex);
    g_alertSink = std::move(sink);
}

size_t btcWatchCount() {
    std::lock_guard<std::mutex> w(g_watchMutex);
    return g_watch.size();
}

std::string btcStatsLine() {
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    if (!g_btcDb) return "₿ <b>Bitcoin</b>\n\nСканер выключен (WHALE_BTC=0 или база не открылась).";
    auto one = [](const char* sql, long long arg = -1) -> long long {
        sqlite3_stmt* s = nullptr;
        long long v = 0;
        if (prep(&s, sql)) {
            if (arg >= 0) sqlite3_bind_int64(s, 1, arg);
            if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
        return v;
    };
    auto btc = [](long long sats) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(sats) / SAT);
        return std::string(buf);
    };
    const long long day = nowSec() - 86400;
    long long height = 0, ts = 0, price = 0;
    sqlite3_stmt* s = nullptr;
    if (prep(&s, "SELECT height, ts, price_nanos FROM btc_blocks ORDER BY height DESC LIMIT 1")) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            height = sqlite3_column_int64(s, 0);
            ts = sqlite3_column_int64(s, 1);
            price = sqlite3_column_int64(s, 2);
        }
        sqlite3_finalize(s);
    }
    // Числа — с пробелами по разрядам, время — коротко, как в /stats BSC.
    auto num = [](long long v) {
        std::string d = std::to_string(v < 0 ? -v : v), r;
        for (size_t i = 0; i < d.size(); ++i) {
            if (i && (d.size() - i) % 3 == 0) r += ' ';
            r += d[i];
        }
        return (v < 0 ? "-" : "") + r;
    };
    auto ago = [](long long sec) {
        if (sec < 0) sec = 0;
        if (sec < 120) return std::to_string(sec) + " с";
        if (sec < 7200) return std::to_string(sec / 60) + " мин";
        if (sec < 2 * 86400) return std::to_string(sec / 3600) + " ч " + std::to_string(sec % 3600 / 60) + " мин";
        return std::to_string(sec / 86400) + " д " + std::to_string(sec % 86400 / 3600) + " ч";
    };
    auto secs = [](long long ms) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f с", static_cast<double>(ms) / 1000.0);
        std::string r = buf;
        for (auto& ch : r) if (ch == '.') ch = ',';
        return r;
    };
    auto mb = [](long long bytes) {
        char buf[32];
        if (bytes >= (10LL << 30)) std::snprintf(buf, sizeof buf, "%.1f ГБ", static_cast<double>(bytes) / (1LL << 30));
        else std::snprintf(buf, sizeof buf, "%.0f МБ", static_cast<double>(bytes) / (1LL << 20));
        std::string r = buf;
        for (auto& ch : r) if (ch == '.') ch = ',';
        return r;
    };
    const long long now = nowSec();
    const long long started = g_bs.startedAt.load();
    const long long blocks = g_bs.blocks.load();

    std::string out = "₿ <b>Bitcoin</b>\n\n<b>Сканер</b>";
    out += "\n⏳ Lag: " + num(g_bs.lag.load()) + " blocks (max: " + num(g_bs.maxLag.load()) + ")";
    out += "\n⛓ Сеть: блок " + num(g_bs.tip.load()) + " · разобран " + num(height);
    if (ts) out += " (" + ago(now - ts) + " назад)";
    if (started) out += "\n⏱ Работает: " + ago(now - started);
    out += "\n📦 Блоков: " + num(blocks) + " с запуска · " +
           num(one("SELECT COUNT(*) FROM btc_blocks WHERE ts >= ?", day)) + " за сутки";
    out += "\n🔄 Перестроек: " + num(g_bs.reorgs.load()) + " · пропущено при простое: " + num(g_bs.skipped.load()) +
           " · не загрузилось: " + num(g_bs.loadFails.load());
    out += "\n🔍 TX: " + num(g_bs.txs.load()) + " с запуска · " +
           num(one("SELECT COALESCE(SUM(txs), 0) FROM btc_blocks WHERE ts >= ?", day)) + " за сутки";
    out += "\n↔️ Входов " + num(g_bs.ins.load()) + " · выходов " + num(g_bs.outs.load());
    out += "\n📝 Движений записано: " + num(g_bs.moves.load()) + " · 📨 алертов: " + num(g_bs.alerts.load());
    if (blocks > 0)
        out += "\n⚡ Блок грузится: " + secs(g_bs.loadMs.load()) + " (среднее " + secs(g_bs.loadMsSum.load() / blocks) + ")";
    out += "\n📶 Трафик: " + mb(g_bs.bytes.load()) + " с запуска";
    if (price) out += "\n💰 Цена: $" + num(price / 1000000000LL);

    {
        std::lock_guard<std::mutex> n(g_nodeMutex);
        const auto& pool = rpcPool();
        out += "\n\n🌐 <b>Узлы Bitcoin</b> (" + std::to_string(pool.size()) + ")";
        for (size_t i = 0; i < pool.size(); ++i) {
            const NodeStat& st = nodeAt(i);
            std::string host = safeUrl(pool[i]);
            const auto sch = host.find("://");
            if (sch != std::string::npos) host = host.substr(sch + 3);
            const auto slash = host.find('/');
            if (slash != std::string::npos) host = host.substr(0, slash);
            out += "\n· " + host + " — " + (st.rest > now ? "💤 отдыхает" : st.ok ? "✅" : "⏸ запасной") +
                   " ок " + num(st.ok) + " · сбоев " + num(st.fail);
            if (st.ok) out += " · " + secs(st.ms / st.ok);
        }
    }
    out += "\n📇 Адреса (mempool.space): запросов " + num(g_bs.addrAsked.load()) + " · сбоев " +
           num(g_bs.addrFailed.load());

    {
        // Резервы бирж — свой счёт: остатки адресов из разметки.
        const long long total = one("SELECT COALESCE(SUM(MAX(bal_sats, 0)), 0) FROM btc_ex_bal WHERE checked_at != 0");
        const long long done = one("SELECT COUNT(*) FROM btc_ex_bal WHERE checked_at != 0");
        const long long all = one("SELECT COUNT(*) FROM btc_ex_bal");
        out += "\n🏦 Резервы бирж: " + btc(total) + " BTC · адресов сверено " + num(done) + " из " + num(all);
    }

    out += "\n\n📈 <b>Разбор транзакций</b> (с запуска)"
           "\n🟢 С биржи (выводы): " + num(g_bs.fromEx.load()) +
           "\n🚨 На биржу (заводы): " + num(g_bs.toEx.load()) +
           "\n🧺 Сбор депозитов: " + num(g_bs.sweep.load()) +
           "\n🔁 Перекладки биржи: " + num(g_bs.move.load()) +
           "\n♻️ Круги через свой адрес: " + num(g_bs.bounce.load()) +
           "\n🔀 Несколько бирж во входах: " + num(g_bs.mixed.load()) +
           "\n🌀 CoinJoin: " + num(g_bs.coinjoin.load()) +
           "\n⛏ Coinbase: " + num(g_bs.coinbase.load()) +
           "\n📤 Без бирж: " + num(g_bs.plain.load());

    {
        const long long at = g_bs.cleanAt.load();
        out += "\n\n🧹 <b>Самоочистка</b> (раз в час)";
        if (at)
            out += "\nПоследняя: " + ago(now - at) + " назад · откатов блоков " + num(g_bs.cleanFlow.load()) +
                   " · снимков адресов " + num(g_bs.cleanAddr.load()) + " · пустых потоков " +
                   num(g_bs.cleanEmpty.load()) + " · старых строк " + num(g_bs.cleanOld.load());
        else
            out += "\nЕщё не было";
        struct stat st{};
        const std::string file = btcDbFile();
        long long dbSize = stat(file.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : 0;
        long long walSize = stat((file + "-wal").c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : 0;
        out += "\n💾 База: " + mb(dbSize) + " · WAL " + mb(walSize);
    }

    out += etfStats(g_btcDb);
    out += govStats(g_btcDb);
    out += "\n⏮ Догонка истории фондов: " + g_backStatus;

    out += "\n\n<b>Адреса бирж</b>: " + std::to_string(one("SELECT COUNT(*) FROM btc_labels"));
    if (prep(&s, "SELECT how, COUNT(*) FROM btc_labels GROUP BY how ORDER BY 2 DESC")) {
        while (sqlite3_step(s) == SQLITE_ROW)
            out += "\n  " + colText(s, 0) + ": " + std::to_string(sqlite3_column_int64(s, 1));
        sqlite3_finalize(s);
    }
    if (prep(&s, "SELECT ex, COUNT(*) FROM btc_labels GROUP BY ex ORDER BY 2 DESC LIMIT 8")) {
        out += "\nПо биржам:";
        while (sqlite3_step(s) == SQLITE_ROW)
            out += " " + colText(s, 0) + " " + std::to_string(sqlite3_column_int64(s, 1)) + ";";
        sqlite3_finalize(s);
    }

    out += "\n\n<b>База сервисного аккаунта</b>: " + std::to_string(btcWatchCount()) +
           (WATCH_MAX > 0 ? " / " + std::to_string(WATCH_MAX) : std::string(" (без потолка)")) +
           "\n  импорт: " + std::to_string(one("SELECT COUNT(*) FROM btc_watch WHERE src='import'")) +
           "\n  сам нашёл: " + std::to_string(one("SELECT COUNT(*) FROM btc_watch WHERE src='auto'"));

    out += "\n\n<b>За сутки</b>"
           "\nВыводов с бирж (покупки): " + std::to_string(one("SELECT COUNT(*) FROM btc_moves WHERE ts >= ? AND kind = 1 AND ex != ''", day)) +
           "\nЗаводов на биржи (продажи): " + std::to_string(one("SELECT COUNT(*) FROM btc_moves WHERE ts >= ? AND kind = 2 AND ex != ''", day)) +
           "\nДвижений кошельков базы: " + std::to_string(one(
               "SELECT COUNT(*) FROM btc_moves m WHERE ts >= ? AND EXISTS (SELECT 1 FROM btc_watch w WHERE w.address = m.wallet)", day)) +
           "\nПриток на биржи: " + btc(one("SELECT COALESCE(SUM(in_sats), 0) FROM btc_flow WHERE ts >= ?", day)) + " BTC" +
           "\nОтток с бирж: " + btc(one("SELECT COALESCE(SUM(out_sats), 0) FROM btc_flow WHERE ts >= ?", day)) + " BTC";
    return out;
}
