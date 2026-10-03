/*
 * Сканер сети Bitcoin. Что и зачем — в btc_chain.h.
 *
 * Круг работы:
 *   1. Раз в полминуты узнаём высоту последнего блока (mempool.space, запасной
 *      blockstream.info).
 *   2. Каждый новый блок целиком одним запросом с blockchain.info: там у
 *      входов уже есть адрес и сумма потраченного выхода, без них не понять,
 *      откуда пришли монеты. Не ответил — тот же блок страницами с
 *      mempool.space.
 *   3. Для крупных транзакций спрашиваем разметку walletexplorer.com: чей
 *      кластер у адреса. Ответы помним — адрес биржи биржей и останется.
 *   4. Разбираем транзакции: с биржи на частный адрес — вывод (покупка),
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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <fcntl.h>
#include <sys/file.h>
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

long long envSats(const char* key, double defBtc) {
    const char* v = std::getenv(key);
    double btc = defBtc;
    if (v && *v) {
        try { btc = std::stod(v); } catch (...) {}
    }
    if (btc <= 0) btc = defBtc;
    return static_cast<long long>(btc * SAT);
}

// Строка в btc_moves — с одного биткоина: ниже начинается розница, и доска
// крупных движений утонула бы в ней. Потоки по часам считаются с любой суммы.
const long long MOVE_MIN_SATS = envSats("WHALE_BTC_MOVE_MIN", 1.0);
// С какой суммы транзакции спрашиваем разметку внешнего сервиса.
const long long LOOKUP_MIN_SATS = envSats("WHALE_BTC_LOOKUP_MIN", 5.0);
// Сколько запросов разметки на один блок: сервис бесплатный, не наглеем.
constexpr int LOOKUPS_PER_BLOCK = 40;
// Насколько отставание догоняем блок за блоком: сутки. Цена каждого блока —
// на его час (priceAt), поэтому старые блоки считаются в верных долларах.
// Простой дольше суток — прыгаем к свежим и пишем, сколько пропущено.
constexpr long long MAX_BEHIND = 144;
constexpr long long KEEP_SEC = 400LL * 86400LL;
constexpr long long WE_EMPTY_TTL = 3LL * 86400LL;
// Выученные адреса без новых встреч забываем через три месяца: адресов
// пополнения биржи сметают десятки тысяч в сутки, хранить их вечно — гигабайты.
// Живой адрес освежается каждой встречей и не пропадает.
constexpr long long LEARNED_TTL = 90LL * 86400LL;
// Сколько транзакций у адреса и что на нём лежит — раз в неделю на адрес.
// Тысячи транзакций у частного кошелька не бывает: это сервис, платёжный шлюз
// или биржа, которой нет в разметке. API убирает такие из досок.
constexpr long long ADDR_TTL = 7LL * 86400LL;
constexpr int ADDR_PER_BLOCK = 60;
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
// Кто попадает в базу сам: вывел с биржи от десяти биткоинов за раз и не
// похож на сервис.
const long long AUTO_MIN_SATS = envSats("WHALE_BTC_AUTO_MIN", 10.0);
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

size_t writeCb(void* p, size_t s, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(p), s * n);
    return s * n;
}

struct Curl {
    CURL* h = curl_easy_init();
    ~Curl() { if (h) curl_easy_cleanup(h); }
};

std::string fetch(const std::string& url, long timeout = 20, long* status = nullptr) {
    thread_local Curl c;
    if (!c.h) return "";
    curl_easy_reset(c.h);
    std::string body;
    curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c.h, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(c.h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c.h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c.h, CURLOPT_FOLLOWLOCATION, 1L);
    // Блок с blockchain.info — семь мегабайт JSON; сжатым он втрое меньше.
    curl_easy_setopt(c.h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c.h, CURLOPT_USERAGENT, "whalescanner/1.0");
    curl_easy_setopt(c.h, CURLOPT_XFERINFOFUNCTION,
        +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return (running.load(std::memory_order_relaxed) &&
                    g_btcRunning.load(std::memory_order_relaxed)) ? 0 : 1; });
    curl_easy_setopt(c.h, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(c.h);
    long code = 0;
    curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    if (rc != CURLE_OK || code != 200) {
        if (rc != CURLE_OK)
            std::cerr << "[BTC] " << curl_easy_strerror(rc) << " | " << url.substr(0, 90) << std::endl;
        return "";
    }
    return body;
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

long long tipHeight() {
    for (const char* u : {"https://mempool.space/api/blocks/tip/height",
                          "https://blockstream.info/api/blocks/tip/height"}) {
        long long h = parseLL(fetch(u, 10));
        if (h > 0) return h;
    }
    return 0;
}

std::string hashAt(long long height) {
    const std::string h = std::to_string(height);
    for (const std::string& u : {"https://mempool.space/api/block-height/" + h,
                                "https://blockstream.info/api/block-height/" + h}) {
        std::string r = fetch(u, 10);
        if (r.size() == 64 && r.find_first_not_of("0123456789abcdef") == std::string::npos) return r;
    }
    return "";
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

struct BIn { std::string addr; long long sats = 0; };
struct BOut { std::string addr; long long sats = 0; };
struct BTx {
    std::string txid;
    bool coinbase = false;
    std::vector<BIn> in;
    std::vector<BOut> out;
    long long inSum() const { long long s = 0; for (auto& i : in) s += i.sats; return s; }
    long long outSum() const { long long s = 0; for (auto& o : out) s += o.sats; return s; }
};

struct Block {
    long long height = 0;
    long long ts = 0;
    std::string hash;
    // Хэш предыдущего блока — по нему видна перестройка цепочки.
    std::string prev;
    std::vector<BTx> txs;
};

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

// Формат blockchain.info: inputs[].prev_out.{addr,value}, out[].{addr,value}.
bool loadFromBlockchainInfo(const std::string& hash, Block& b) {
    std::string raw = fetch("https://blockchain.info/rawblock/" + hash, 90);
    if (raw.empty()) return false;
    json j = json::parse(raw, nullptr, false);
    if (!j.is_object() || !j.contains("tx") || !j["tx"].is_array()) return false;
    b.ts = jll(j, "time");
    b.prev = jstr(j, "prev_block");
    b.txs.reserve(j["tx"].size());
    for (const auto& t : j["tx"]) {
        BTx x;
        x.txid = jstr(t, "hash");
        if (t.contains("inputs") && t["inputs"].is_array()) {
            for (const auto& i : t["inputs"]) {
                if (!i.contains("prev_out") || !i["prev_out"].is_object()) { x.coinbase = true; continue; }
                const auto& p = i["prev_out"];
                x.in.push_back({jstr(p, "addr"), jll(p, "value")});
            }
        }
        if (x.in.empty()) x.coinbase = true;
        if (t.contains("out") && t["out"].is_array())
            for (const auto& o : t["out"]) x.out.push_back({jstr(o, "addr"), jll(o, "value")});
        if (!x.txid.empty()) b.txs.push_back(std::move(x));
    }
    return !b.txs.empty();
}

// Запасной путь — Esplora (mempool.space): по 25 транзакций на страницу.
bool loadFromEsplora(const std::string& hash, Block& b) {
    json meta = json::parse(fetch("https://mempool.space/api/block/" + hash, 15), nullptr, false);
    if (!meta.is_object()) return false;
    long long n = jll(meta, "tx_count");
    b.ts = jll(meta, "timestamp");
    b.prev = jstr(meta, "previousblockhash");
    if (n <= 0) return false;
    b.txs.clear();
    for (long long start = 0; start < n; start += 25) {
        json page;
        for (int attempt = 0; attempt < 3; ++attempt) {
            page = json::parse(fetch("https://mempool.space/api/block/" + hash + "/txs/" + std::to_string(start), 20),
                               nullptr, false);
            if (page.is_array()) break;
            if (!sleepFor(2)) return false;
        }
        if (!page.is_array()) return false;
        for (const auto& t : page) {
            BTx x;
            x.txid = jstr(t, "txid");
            if (t.contains("vin") && t["vin"].is_array()) {
                for (const auto& i : t["vin"]) {
                    if (i.value("is_coinbase", false) || !i.contains("prevout") || !i["prevout"].is_object()) {
                        x.coinbase = true;
                        continue;
                    }
                    x.in.push_back({jstr(i["prevout"], "scriptpubkey_address"), jll(i["prevout"], "value")});
                }
            }
            if (t.contains("vout") && t["vout"].is_array())
                for (const auto& o : t["vout"]) x.out.push_back({jstr(o, "scriptpubkey_address"), jll(o, "value")});
            if (x.in.empty()) x.coinbase = true;
            b.txs.push_back(std::move(x));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    return static_cast<long long>(b.txs.size()) == n;
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

// Кластеры бирж из раздела «Exchanges» walletexplorer.com. Остальные его
// метки — пулы, казино, миксеры — для потока бирж не годятся.
const std::unordered_set<std::string> WE_EXCHANGES = {
    "Huobi.com", "Bittrex.com", "Luno.com", "Kraken.com", "Poloniex.com", "BTC-e.com", "BitZlato.com",
    "Bitstamp.net", "LocalBitcoins.com", "MercadoBitcoin.com.br", "Cryptsy.com", "Binance.com", "Bitcoin.de",
    "CoinSpot.com.au", "Cex.io", "BtcTrade.com", "YoBit.net", "OKCoin.com", "BTCC.com", "BX.in.th",
    "HitBtc.com", "MaiCoin.com", "Bter.com", "Hashnest.com", "Bleutrade.com", "AnxPro.com", "BitBay.net",
    "Bitfinex.com", "CoinMotion.com", "CoinHako.com", "Matbea.com", "Bit-x.com", "VirWoX.com", "Paxful.com",
    "BitBargain.co.uk", "SpectroCoin.com", "Cavirtex.com", "C-Cex.com", "TheRockTrading.com", "FoxBit.com.br",
    "Vircurex.com", "BitVC.com", "Exmo.com", "Btc38.com", "Igot.com", "BlockTrades.us", "SimpleCoin.cz",
    "FYBSG.com", "CampBX.com", "CoinTrader.net", "Bitcurex.com", "Coinmate.io", "Korbit.co.kr", "Vaultoro.com",
    "Exchanging.ir", "796.com", "HappyCoins.com", "BtcMarkets.net", "ChBtc.com", "CoinCafe.com", "LiteBit.eu",
    "UrduBit.com", "BTradeAustralia.com", "MeXBT.com", "Coinomat.com", "OrderBook.net", "LakeBTC.com",
    "BitKonan.com", "QuadrigaCX.com", "Banx.io", "CleverCoin.com", "Gatecoin.com", "Indacoin.com", "CoinArch.com",
    "BitcoinVietnam.com.vn", "CoinChimp.com", "BitcoinP2P.com.br", "Coingi.com", "Cryptonit.net", "Bitso.com",
    "Coinimal.com", "EmpoEX.com", "Ccedk.com", "UseCryptos.com", "Coinbroker.io",
};

// «Kraken.com-old» → кластер «Kraken.com» → на экране «Kraken».
std::string weBase(const std::string& label) {
    for (const char* keep : {"BTC-e.com", "Bit-x.com", "C-Cex.com"})
        if (label.rfind(keep, 0) == 0) return keep;
    auto dash = label.find('-');
    return dash == std::string::npos ? label : label.substr(0, dash);
}

std::string exName(const std::string& base) {
    if (base == "Bitcoin.de" || base == "Cex.io" || base == "796.com") return base == "Cex.io" ? "CEX.IO" : base == "796.com" ? "796" : base;
    auto dot = base.find('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

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
    sqlite3_stmt* getWe = nullptr;
    sqlite3_stmt* putWe = nullptr;
    sqlite3_stmt* getCluster = nullptr;
    sqlite3_stmt* putCluster = nullptr;
    sqlite3_stmt* putMove = nullptr;
    sqlite3_stmt* putFlow = nullptr;
    ~Db() {
        for (auto* s : {getLabel, putLabel, getWe, putWe, getCluster, putCluster, putMove, putFlow})
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

class Scanner {
public:
    bool init() {
        return prep(&db_.getLabel, "SELECT ex FROM btc_labels WHERE address=?") &&
               prep(&db_.putLabel,
                    "INSERT INTO btc_labels(address, ex, how, at) VALUES(?,?,?,?) "
                    "ON CONFLICT(address) DO UPDATE SET at=excluded.at "
                    "WHERE btc_labels.how != 'seed' AND btc_labels.ex = excluded.ex") &&
               prep(&db_.getWe, "SELECT wid, label, at FROM btc_we WHERE address=?") &&
               prep(&db_.putWe, "INSERT OR REPLACE INTO btc_we(address, wid, label, at) VALUES(?,?,?,?)") &&
               prep(&db_.getCluster, "SELECT ex FROM btc_clusters WHERE wid=?") &&
               prep(&db_.putCluster, "INSERT OR IGNORE INTO btc_clusters(wid, ex, at) VALUES(?,?,?)") &&
               prep(&db_.putMove,
                    "INSERT OR IGNORE INTO btc_moves(txid, height, ts, kind, wallet, ex, sats, usd_nanos, price_nanos) "
                    "VALUES(?,?,?,?,?,?,?,?,?)") &&
               prep(&db_.putFlow,
                    "INSERT INTO btc_flow(ts, ex, in_sats, out_sats, in_n, out_n) VALUES(?,?,?,?,?,?) "
                    "ON CONFLICT(ts, ex) DO UPDATE SET in_sats=in_sats+excluded.in_sats, "
                    "out_sats=out_sats+excluded.out_sats, in_n=in_n+excluded.in_n, out_n=out_n+excluded.out_n");
    }

    void seed() {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        for (const auto& [a, ex] : SEEDS) {
            putLabel(a, ex, "seed");
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
    }

    // Кластеры проверенных адресов: всё, что walletexplorer сведёт в тот же
    // кластер, — та же биржа.
    void seedClusters() {
        for (const auto& [a, ex] : SEEDS) {
            std::string wid, label;
            if (!weLookup(a, wid, label)) continue;
            if (!wid.empty()) {
                std::lock_guard<std::mutex> l(g_btcDbMutex);
                sqlite3_reset(db_.putCluster);
                bindText(db_.putCluster, 1, wid);
                bindText(db_.putCluster, 2, ex);
                sqlite3_bind_int64(db_.putCluster, 3, nowSec());
                sqlite3_step(db_.putCluster);
            }
            if (!sleepFor(1)) return;
        }
    }

    enum class Result { DONE, REORG };

    Result process(const Block& b) {
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
        lookups_ = 0;
        touched_.clear();
        autoCand_.clear();
        touchLabel_.clear();
        enrich(b);
        reloadWatch();
        const long long price = priceAt(b.ts);
        int moves = 0, learned = 0;
        alerts_.clear();
        {
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            execSql("BEGIN");
            scan(b, price, moves, learned);
            execSql("COMMIT");
        }
        addrStats();
        autoWatch();
        sendAlerts();
        std::cout << "[BTC] block " << b.height << ": " << b.txs.size() << " tx, " << moves << " moves, "
                  << learned << " new labels, " << lookups_ << " lookups" << std::endl;
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
                    continue;
                }
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
                    if (v >= AUTO_MIN_SATS && !watch.count(a)) autoCand_.insert(a);
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
                for (const auto& o : tx.out) {
                    if (o.addr.empty() || o.sats <= 0) continue;
                    std::string e = label(o.addr);
                    if (e.empty()) continue;
                    touchLabel_.insert(o.addr);
                    auto& f = flow[e];
                    f[0] += o.sats;
                    f[2] += 1;
                    toEx[e] += o.sats;
                }
                if (!toEx.empty() && !sender->addr.empty() && !(sweep && !mine(sender->addr))) {
                    // Строка одна на транзакцию: вся сумма на биржи, биржа —
                    // та, куда ушло больше.
                    long long total = 0, best = 0;
                    std::string ex;
                    for (const auto& [e, v] : toEx) { total += v; if (v > best) { best = v; ex = e; } }
                    if (total >= MOVE_MIN_SATS || (mine(sender->addr) && watchWorth(total, price))) {
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
    int lookups_ = 0;
    std::vector<std::string> touched_;
    std::unordered_set<std::string> autoCand_;
    std::unordered_set<std::string> touchLabel_;
    // Кому и по какой транзакции алерт уже ушёл: после отката перестроенного
    // блока его транзакции приходят снова, второй алерт на них не нужен.
    std::unordered_set<std::string> alerted_;

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
            if (sink) sink(a);
        }
    }

    // Новые киты в базу: крупный вывод с биржи и не сервис по числу
    // транзакций. Без потолка, если WATCH_MAX не задан.
    void autoWatch() {
        if (autoCand_.empty()) return;
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_stmt* q = nullptr;
        sqlite3_stmt* ins = nullptr;
        if (!prep(&q, "SELECT txs FROM btc_addr WHERE address=?") ||
            !prep(&ins, "INSERT OR IGNORE INTO btc_watch(address, src, at) VALUES(?, 'auto', ?)")) {
            if (q) sqlite3_finalize(q);
            return;
        }
        int added = 0;
        for (const auto& a : autoCand_) {
            {
                std::lock_guard<std::mutex> w(g_watchMutex);
                if (WATCH_MAX > 0 && static_cast<long long>(g_watch.size()) >= WATCH_MAX) break;
            }
            if (!label(a).empty()) continue;
            sqlite3_reset(q);
            bindText(q, 1, a);
            if (sqlite3_step(q) != SQLITE_ROW || sqlite3_column_int64(q, 0) >= SERVICE_TXS) continue;
            sqlite3_reset(ins);
            bindText(ins, 1, a);
            sqlite3_bind_int64(ins, 2, nowSec());
            if (sqlite3_step(ins) == SQLITE_DONE && sqlite3_changes(g_btcDb) > 0) {
                std::lock_guard<std::mutex> w(g_watchMutex);
                g_watch.insert(a);
                ++added;
            }
        }
        sqlite3_finalize(q);
        sqlite3_finalize(ins);
        if (added) std::cout << "[BTC] в базу сервисного аккаунта: +" << added << std::endl;
    }

    // Число транзакций и остаток кошельков, попавших в btc_moves этим блоком.
    void addrStats() {
        std::vector<std::string> ask;
        {
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            std::unordered_set<std::string> seen;
            sqlite3_stmt* s = nullptr;
            if (!prep(&s, "SELECT at FROM btc_addr WHERE address=?")) return;
            for (const auto& a : touched_) {
                if (!seen.insert(a).second) continue;
                sqlite3_reset(s);
                bindText(s, 1, a);
                if (sqlite3_step(s) == SQLITE_ROW && nowSec() - sqlite3_column_int64(s, 0) < ADDR_TTL) continue;
                ask.push_back(a);
            }
            sqlite3_finalize(s);
        }
        int n = 0;
        for (const auto& a : ask) {
            if (++n > ADDR_PER_BLOCK) break;
            json j;
            for (const char* host : {"https://mempool.space/api/address/", "https://blockstream.info/api/address/"}) {
                j = json::parse(fetch(host + a, 10), nullptr, false);
                if (j.is_object() && j.contains("chain_stats")) break;
            }
            if (!j.is_object() || !j.contains("chain_stats") || !j["chain_stats"].is_object()) continue;
            const auto& c = j["chain_stats"];
            long long txs = jll(c, "tx_count");
            long long bal = jll(c, "funded_txo_sum") - jll(c, "spent_txo_sum");
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

    // Ответ walletexplorer: кластер и, если есть, его имя. false — сервис не
    // ответил, повторим в другой раз.
    bool weLookup(const std::string& a, std::string& wid, std::string& lab) {
        std::string r = fetch("https://www.walletexplorer.com/api/1/address?address=" + a +
                              "&from=0&count=0&caller=whalescanner", 15);
        json j = json::parse(r, nullptr, false);
        if (!j.is_object()) return false;
        wid = jstr(j, "wallet_id");
        lab = jstr(j, "label");
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3_reset(db_.putWe);
        bindText(db_.putWe, 1, a);
        bindText(db_.putWe, 2, wid);
        bindText(db_.putWe, 3, lab);
        sqlite3_bind_int64(db_.putWe, 4, nowSec());
        sqlite3_step(db_.putWe);
        return true;
    }

    // Биржа по кластеру: проверенный кластер или биржевая метка сервиса.
    std::string exOfCluster(const std::string& wid, const std::string& lab) {
        if (!lab.empty() && WE_EXCHANGES.count(weBase(lab))) return exName(weBase(lab));
        if (wid.empty()) return "";
        sqlite3_reset(db_.getCluster);
        bindText(db_.getCluster, 1, wid);
        return sqlite3_step(db_.getCluster) == SQLITE_ROW ? colText(db_.getCluster, 0) : "";
    }

    // Для крупных транзакций без известных меток спрашиваем разметку: главный
    // вход и крупные выходы. До разбора блока — чтобы разбор уже знал ответы.
    void enrich(const Block& b) {
        std::vector<std::pair<long long, std::string>> ask;
        std::unordered_set<std::string> seen;
        {
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            for (const auto& tx : b.txs) {
                if (tx.coinbase || tx.in.empty() || coinjoin(tx) || tx.outSum() < LOOKUP_MIN_SATS) continue;
                const BIn* top = &tx.in[0];
                for (const auto& i : tx.in) if (i.sats > top->sats) top = &i;
                std::vector<std::pair<long long, std::string>> cand{{tx.outSum(), top->addr}};
                for (const auto& o : tx.out) if (o.sats >= LOOKUP_MIN_SATS) cand.emplace_back(o.sats, o.addr);
                for (const auto& [sats, a] : cand) {
                    if (a.empty() || !seen.insert(a).second || !label(a).empty()) continue;
                    sqlite3_reset(db_.getWe);
                    bindText(db_.getWe, 1, a);
                    if (sqlite3_step(db_.getWe) == SQLITE_ROW) {
                        std::string wid = colText(db_.getWe, 0), lab = colText(db_.getWe, 1);
                        long long at = sqlite3_column_int64(db_.getWe, 2);
                        std::string ex = exOfCluster(wid, lab);
                        if (!ex.empty()) { putLabel(a, ex, "we"); continue; }
                        if (nowSec() - at < WE_EMPTY_TTL) continue;
                    }
                    ask.emplace_back(sats, a);
                }
            }
        }
        // Крупнее — важнее: если лимит кончится, без ответа останутся мелкие.
        std::sort(ask.begin(), ask.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (const auto& [sats, a] : ask) {
            if (lookups_ >= LOOKUPS_PER_BLOCK) break;
            ++lookups_;
            std::string wid, lab;
            if (!weLookup(a, wid, lab)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                continue;
            }
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            std::string ex = exOfCluster(wid, lab);
            if (!ex.empty()) putLabel(a, ex, "we");
        }
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
        // без сдачи, we — разметка walletexplorer.
        "CREATE TABLE IF NOT EXISTS btc_labels ("
        "  address TEXT PRIMARY KEY, ex TEXT NOT NULL, how TEXT NOT NULL, at INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_btc_labels_at ON btc_labels(at);"
        "CREATE TABLE IF NOT EXISTS btc_we ("
        "  address TEXT PRIMARY KEY, wid TEXT NOT NULL DEFAULT '', label TEXT NOT NULL DEFAULT '',"
        "  at INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS btc_clusters ("
        "  wid TEXT PRIMARY KEY, ex TEXT NOT NULL, at INTEGER NOT NULL DEFAULT 0);"
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
        "CREATE TABLE IF NOT EXISTS btc_state (k TEXT PRIMARY KEY, v TEXT NOT NULL);");
    // Вклад блока в потоки — для отката при перестройке цепочки. Колонка
    // добавлена позже: на старой базе её нет, ошибку «уже есть» глотаем.
    sqlite3_exec(g_btcDb, "ALTER TABLE btc_blocks ADD COLUMN flow TEXT", nullptr, nullptr, nullptr);
    sqlite3_stmt* s = nullptr;
    if (prep(&s, "SELECT address FROM btc_watch")) {
        std::lock_guard<std::mutex> w(g_watchMutex);
        while (sqlite3_step(s) == SQLITE_ROW) g_watch.insert(colText(s, 0));
        sqlite3_finalize(s);
    }
    std::cout << "[BTC] база сервисного аккаунта, кошельков: " << g_watch.size() << std::endl;
    return true;
}

void cleanup() {
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    const long long cut = nowSec() - KEEP_SEC;
    const std::string c = std::to_string(cut);
    const std::string w = std::to_string(nowSec() - WE_EMPTY_TTL);
    execSql(("DELETE FROM btc_moves WHERE ts < " + c + ";"
             "DELETE FROM btc_flow WHERE ts < " + c + ";"
             "DELETE FROM btc_labels WHERE how != 'seed' AND at < " + std::to_string(nowSec() - LEARNED_TTL) + ";"
             "DELETE FROM btc_blocks WHERE ts < " + std::to_string(nowSec() - 30LL * 86400LL) + ";"
             "DELETE FROM btc_we WHERE wid = '' AND label = '' AND at < " + w + ";").c_str());
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
    // Номер — версия списка SEEDS: список вырос — кластеры новых адресов
    // надо спросить заново.
    if (sc.state("clusters") != "3") {
        sc.seedClusters();
        sc.setStateLocked("clusters", "3");
    }
    auto lastClean = std::chrono::steady_clock::now() - std::chrono::hours(1);

    while (running.load(std::memory_order_relaxed) && g_btcRunning.load()) {
        try {
            long long tip = tipHeight();
            if (tip <= 0) { if (!sleepFor(30)) break; continue; }
            long long last = 0;
            {
                std::string v = sc.state("height");
                last = v.empty() ? 0 : parseLL(v);
            }
            if (last <= 0 || tip - last > MAX_BEHIND) {
                long long from = tip - (last <= 0 ? 1 : 6);
                if (last > 0)
                    std::cerr << "[BTC] простой дольше суток: блоки " << last + 1 << "–" << from
                              << " пропущены" << std::endl;
                std::cout << "[BTC] start from block " << from + 1 << " (tip " << tip << ")" << std::endl;
                last = from;
                sc.setStateLocked("height", std::to_string(last));
            }
            while (last < tip && running.load(std::memory_order_relaxed) && g_btcRunning.load()) {
                Block b;
                b.height = last + 1;
                b.hash = hashAt(b.height);
                if (b.hash.empty()) break;
                if (!loadFromBlockchainInfo(b.hash, b)) {
                    std::cerr << "[BTC] blockchain.info не отдал блок " << b.height << ", беру с mempool.space" << std::endl;
                    b.txs.clear();
                    if (!loadFromEsplora(b.hash, b)) break;
                }
                if (sc.process(b) == Scanner::Result::REORG) {
                    last = b.height - 2;
                    continue;
                }
                last = b.height;
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
    g_btcThread = std::thread(btcLoop);
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

BtcImportResult btcImport(const std::vector<std::string>& addrs) {
    BtcImportResult r;
    std::lock_guard<std::mutex> l(g_btcDbMutex);
    if (!g_btcDb) return r;
    sqlite3_stmt* s = nullptr;
    if (!prep(&s, "INSERT OR IGNORE INTO btc_watch(address, src, at) VALUES(?, 'import', ?)")) return r;
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
        } else {
            ++r.dup;
        }
    }
    execSql("COMMIT");
    sqlite3_finalize(s);
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
    std::string out = "₿ <b>Bitcoin</b>\n\n<b>Сканер</b>\nПоследний блок: " + std::to_string(height);
    if (ts) out += " (" + std::to_string((nowSec() - ts) / 60) + " мин назад)";
    out += "\nБлоков за сутки: " + std::to_string(one("SELECT COUNT(*) FROM btc_blocks WHERE ts >= ?", day));
    if (price) out += "\nЦена: $" + std::to_string(price / 1000000000LL);

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
    out += "\nОтветов walletexplorer в памяти: " + std::to_string(one("SELECT COUNT(*) FROM btc_we"));

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
