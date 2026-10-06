#include "bsc_exchanges.h"
#include "bsc_seeds_book.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <thread>
#include <map>
#include <sstream>
#include <cmath>
#include <ctime>
#include <deque>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>

#include "token_prices.h"
#include "tx_analyzer.h"
#include "utils.h"

namespace {

// Своя база (bsc_ex.db, как btc.db у биткоина): с базой DEX ни таблиц, ни
// замка общих нет.
sqlite3* g_exDb = nullptr;
std::mutex g_exDbMx;

std::string exDbFile() {
    const char* p = std::getenv("WHALE_BSCEX_DB_FILE");
    return (p && *p) ? std::string(p) : std::string("bsc_ex.db");
}

// Очередь блоков: сканер DEX кладёт сюда переводы блока и идёт дальше, а
// разбирает их свой поток. Медленный разбор или сбой здесь DEX не задержит.
struct QBlock {
    long long block = 0, ts = 0;
    std::vector<BscExTx> txs;
    std::function<bool(const std::string&)> watched;
};
constexpr size_t QUEUE_MAX = 2000;
std::mutex g_qMx;
std::condition_variable g_qCv;
std::deque<QBlock> g_queue;
long long g_lastBlock = -1;  // последний разобранный: повтор и откат не считаются дважды
// Остановка: оба потока модуля ждут на g_qCv и выходят по флагу. Без этого
// выход бота зависал — очередь уничтожалась, пока поток на ней ждал.
std::atomic<bool> g_stop{false};
std::thread g_worker, g_pricer;
std::mutex g_stopMx;
std::condition_variable g_stopCv;  // только для потока цен: сигнал о блоке ему не нужен
std::atomic<bool> g_pricerDone{false};
long long g_dropped = 0;

// Проверены в сети BSC: обычный кошелёк (не контракт), отправлял транзакции.
// Источник имён — метки BscScan и Etherscan (адрес кошелька один во всех
// EVM-сетях); Bybit и Bitget — по горячим кошелькам с миллионами отправок.
// Ещё 71 адрес (OKX, Bitget, Binance, Gate, KuCoin, HTX…) — в bsc_seeds_book.h:
// из отчётов о резервах бирж, собирает tools/bsc_seeds.py. Этот список главнее.
struct Seed { const char* addr; const char* ex; };
const Seed SEEDS[] = {
    {"0x8894e0a0c962cb723c1976a4421c95949be2d4e3", "Binance"},  // Binance: Hot Wallet 6
    {"0xe2fc31f816a9b94326492132018c3aecc4a93ae1", "Binance"},  // Binance: Hot Wallet 7
    {"0xa180fe01b906a1be37be6c534a3300785b20d947", "Binance"},  // Binance: Hot Wallet 16
    {"0xeb2d2f1b8c558a40207669291fda468e50c8a0bb", "Binance"},  // Binance: Hot Wallet 10
    {"0x161ba15a5f335c9f06bb5bbb0a9ce14076fbb645", "Binance"},  // Binance: Hot Wallet 11
    {"0xbd612a3f30dca67bf60a39fd0d35e39b7ab80774", "Binance"},  // Binance: Hot Wallet 13
    {"0x515b72ed8a97f42c568d6a143232775018f133c8", "Binance"},  // Binance: Hot Wallet 12
    {"0x3c783c21a0383057d128bae431894a5c19f9cf06", "Binance"},  // Binance: Hot Wallet 8
    {"0xdccf3b77da55107280bd850ea519df3705d1a75a", "Binance"},  // Binance: Hot Wallet 9
    {"0x9430801ebaf509ad49202aabc5f5bc6fd8a3daf8", "Binance"},  // Binance: Deposit Funder
    {"0x631fc1ea2270e98fbd9d92658ece0f5a269aa161", "Binance"},  // Binance: Hot Wallet
    {"0x73f5ebe90f27b46ea12e5795d16c4b408b19cc6f", "Binance"},  // Binance: Hot Wallet 18
    {"0x1fbe2acee135d991592f167ac371f3dd893a508b", "Binance"},  // Binance: Hot Wallet 19
    {"0x29bdfbf7d27462a2d115748ace2bd71a2646946c", "Binance"},  // Binance: Hot Wallet 17
    {"0xb1256d6b31e4ae87da1d56e5890c66be7f1c038e", "Binance"},  // Binance: Hot Wallet 2
    {"0x01c952174c24e1210d26961d456a77a39e1f0bb0", "Binance"},  // Binance Hot Wallet 10
    {"0x17b692ae403a8ff3a3b2ed7676cf194310dde9af", "Binance"},  // Binance: Hot Wallet 3
    {"0xad9ffffd4573b642959d3b854027735579555cbc", "Binance"},  // Binance: Hot Wallet 5
    {"0x8ff804cc2143451f454779a40de386f913dcff20", "Binance"},  // Binance: Hot Wallet 4
    {"0x7a8a34db9acd10c3b6277473b192fe47192569ca", "Binance"},  // Binance: Hot Wallet 14
    {"0x15ece0d7de25436bcfcf3d62a9085ddc7838aee9", "Binance"},  // Binance: Deposit Funder 3
    {"0xf977814e90da44bfa03b6295a0616a897441acec", "Binance"},  // Binance: Hot Wallet 20
    {"0x5a52e96bacdabb82fd05763e25335261b270efcb", "Binance"},  // Binance 28
    {"0xb3f923eabaf178fc1bd8e13902fc5c61d3ddef5b", "Binance"},  // Wintermute: Binance Deposit
    {"0x345d8e3a1f62ee6b1d483890976fd66168e390f2", "Binance"},  // Binance 23
    {"0x892e9e24aea3f27f4c6e9360e312cce93cc98ebe", "Binance"},  // Binance 30
    {"0xbe0eb53f46cd790cd13851d5eff43d12404d33e8", "Binance"},  // Binance 7
    {"0x328130164d0f2b9d7a52edc73b3632e713ff0ec6", "BitMart"},  // Bitmart: Wallet 2
    {"0x8c128dba2cb66399341aa877315be1054be75da8", "BitMart"},  // Bitmart: Wallet
    {"0x1ab4973a48dc892cd9971ece8e01dcc7688f8f23", "Bitget"},  // проверен: 13M транзакций в сети
    {"0x97b9d2102a9a65a26e1ee82d59e42d1b73b68689", "Bitget"},  // проверен: 12M транзакций в сети
    {"0x0639556f03714a74a5feeaf5736a4a64ff70d206", "Bitget"},  // проверен: 4M транзакций в сети
    {"0x5bdf85216ec1e38d6458c870992a69e38e03f7ef", "Bitget"},  // проверен: 2M транзакций в сети
    {"0xf89d7b9c864f589bbf53a82105107622b35eaa40", "Bybit"},  // проверен: 21M транзакций в сети
    {"0xf379fcd9c996d85de025985ba9b1c9c96daa4a72", "CoinDCX"},  // CoinDCX 3
    {"0x660e3bd3bcda11538fa331282666f1d001b87a42", "CoinDCX"},  // CoinDCX 1
    {"0x8c7efd5b04331efc618e8006f19019a3dc88973e", "CoinDCX"},  // CoinDCX 2
    {"0xae45a8240147e6179ec7c9f92c5a18f9a97b3fca", "Crypto.com"},  // Crypto.com: Deposit Funder
    {"0x72a53cdbbcc1b9efa39c834a540550e23463aacb", "Crypto.com"},  // Crypto.com 3
    {"0x4727250679294802377dd6ca6541b8e459077c95", "FixedFloat"},  // FixedFloat: Hot Wallet
    {"0x0d0707963952f2fba59dd06f2b425ace40b492fe", "Gate"},  // Gate.io
    {"0x6596da8b65995d5feacff8c2936f0b7a2051b0d0", "Gate"},  // Gate.io: Deposit Funder
    {"0xc882b111a75c0c657fc507c04fbfcd2cc984f071", "Gate"},  // Gate.io 5
    {"0xefdca55e4bce6c1d535cb2d0687b5567eef2ae83", "HTX"},  // Huobi 1
    {"0x53f78a071d04224b8e254e243fffc6d9f2f3fa23", "KuCoin"},  // Kucoin: Hot Wallet 2
    {"0xd6216fc19db775df9774a6e33526131da7d19a2c", "KuCoin"},  // KuCoin 6
    {"0x4982085c9e2f89f2ecb8131eca71afad896e89cb", "MEXC"},  // mexc.com
    {"0x2e8f79ad740de90dc5f5a9f0d8d9661a60725e64", "MEXC"},  // Mexc.com 3
    {"0x0211f3cedbef3143223d3acf0e589747933e8527", "MEXC"},  // mexc.com 2
    {"0x6e2673095545280f6f10e22eb861a555c6e94bec", "MaskEX"},  // MaskEX 5
    {"0x1349907c197731c5ed98d8442309a15107cb6bad", "MaskEX"},  // MaskEX 1
    {"0x3dd878a95dcaef2800cd57bb065b5e8f2f438131", "MaskEX"},  // MaskEX 3
    {"0xd7aed730a7c4cf8dfe313b16712af3406f6dca5b", "MaskEX"},  // MaskEX 12
    {"0x46c75fc52e0263946f8f1a75a95c23a767d2f26e", "MaskEX"},  // MaskEX 4
    {"0xc6acb77befebff0359cc581973859eee8cbaeda1", "MaskEX"},  // MaskEX 10
    {"0x84457412efe8b3a05583cb496e1d2c03e6f36155", "MaskEX"},  // MaskEX 6
    {"0x2161217d22fac0188775432f8ba32f1d4272dd19", "MaskEX"},  // MaskEX 2
    {"0xd666ad8d95903bce9b4dcd2cacde5145e36405c2", "MaskEX"},  // MaskEX 11
    {"0xa310b3eeca53b9c115af529faf92bb5ca4b41494", "MaskEX"},  // MaskEX 8
    {"0xa4e71851a8c8eaefeb20a994159f4a443e46059b", "MaskEX"},  // MaskEX 9
    {"0x8458c828d602230e92eb0aac5a6aed5580011b6a", "MaskEX"},  // MaskEX 7
    // Метки Etherscan (адрес кошелька один во всех EVM-сетях); в BSC — горячий
    // кошелёк: 103 тыс. и 17 тыс. отправок.
    {"0x986a2fca9eda0e06fbf7839b89bfc006ee2a23dd", "AscendEX"},
    {"0x09344477fdc71748216a7b8bbe7f2013b893def8", "AscendEX"},
};

// Адресов пополнения у Binance — десятки тысяч новых в сутки. Храним 30 дней
// без встреч, а в памяти — не больше LEARNED_MAX самых свежих: забытый адрес
// снова выучится на следующем сливе, а крупный завод на него дождётся этого в
// очереди ожидания (PEND_SEC).
constexpr long long LEARNED_TTL = 30LL * 86400LL;
constexpr size_t LEARNED_MAX = 300000;
// Адрес пополнения только сливает деньги на биржу: тысяч отправок у него не
// бывает. Кошелёк с таким числом — живой человек или бот, его не учим.
constexpr long long LEARN_MAX_NONCE = 10000;
// Поток — по 10 минут: окно «1 час» тогда и правда час, а не до двух.
constexpr long long BUCKET = 600;
constexpr long long FLOW_KEEP = 32LL * 86400LL;  // самое длинное окно в приложении — 30 дней
constexpr long long PEND_SEC = 6LL * 3600LL;          // ждём, пока выучится адрес пополнения
constexpr size_t PEND_MAX = 300000;
constexpr long long USD = 1000000000LL;               // доллар в нано
constexpr long long FLOW_MIN = 100 * USD;             // мельче — пыль и газ на адреса пополнения
constexpr long long PEND_MIN = 1000 * USD;            // ждать будем только крупное…
constexpr long long MOVE_MIN = 50 * USD;              // …и любое от $50 у наблюдаемых
constexpr long long SANE_MAX = 1000000000LL * USD;    // больше миллиарда — ошибка цены

struct Label { std::string ex; bool seed = false; long long at = 0; };
std::mutex g_mx;
std::unordered_map<std::string, Label> g_labels;
std::unordered_set<std::string> g_dirty;  // выученные и освежённые — записать

struct Pend {
    std::string from, token, hash;
    double qty = 0;
    long long usd = 0, ts = 0, block = 0;
    bool watched = false;
};
std::unordered_map<std::string, std::vector<Pend>> g_pend;
std::deque<std::pair<long long, std::string>> g_pendOrder;
size_t g_pendN = 0;

struct Flow { long long inUsd = 0, outUsd = 0, inN = 0, outN = 0; double inQty = 0, outQty = 0; };
std::unordered_map<std::string, Flow> g_flow;  // "час|биржа|монета"

// Неподписанные сборщики: сюда сливают деньги молодые кошельки (у отправителя
// меньше 50 транзакций) — так выглядит сбор с адресов пополнения биржи. Счёт
// за сутки (UTC); владелец смотрит список (/exunknown), проверяет адрес на
// BscScan и подписывает биржу (/exlabel).
constexpr size_t COLLECT_MAX = 50000;
std::unordered_map<std::string, long long> g_collect;
long long g_collectDay = -1;
long long g_lastFlush = 0;

// Монеты, прошедшие через биржу, у которых в памяти нет цены или знаков: без
// них перевод не оценить, и монета выпадает из потока. Дозаполняет фоновый
// поток (цена, знаки, тикер — с запросами к сети), сканер блоков не ждёт.
constexpr size_t UNPRICED_MAX = 5000;
constexpr long long PRICE_RETRY_SEC = 6 * 3600;
std::unordered_set<std::string> g_unpriced;
std::unordered_map<std::string, long long> g_priceTried;

// Монеты, которые биржи сами выводят. Спам-токены рассылают на адреса бирж
// (заводов «на миллионы» по цене из пустого пула), но биржа выводит только то,
// чем торгует. Поэтому завод засчитывается лишь по монете из этого списка;
// BNB и стейблкоины — всегда.
std::unordered_set<std::string> g_listed;
std::unordered_set<std::string> g_listedNew;  // записать в bsc_ex_tokens
long long g_learned = 0, g_moves = 0;

long long now() { return static_cast<long long>(time(nullptr)); }

long double hexLD(const std::string& h, size_t from, size_t len) {
    long double v = 0;
    // len = npos — «до конца»: from + npos переполнился бы и дал пустой разбор.
    const size_t end = len >= h.size() ? h.size() : std::min(h.size(), from + len);
    for (size_t i = from; i < end; i++) {
        const char c = h[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d >= 0) v = v * 16 + d;
    }
    return v;
}

// Перевод из самой транзакции: BNB или transfer() токена. qty — в монетах,
// usd — в нано (0, если цены нет в памяти).
bool parseTransfer(const BscExTx& tx, std::string& token, std::string& rcpt, double& qty, long long& usd) {
    const std::string& input = tx.input;
    const std::string& to = tx.to;
    long double units = 0;
    long double px = 0;  // доллары за монету
    if (input.empty() || input == "0x") {
        units = hexLD(tx.value, 2, std::string::npos) / 1e18L;
        token = chainCtx().nativeMarker;
        rcpt = to;
        px = static_cast<long double>(nativePriceCachedNanos()) / 1e9L;
    } else if (input.size() >= 138 && input.compare(0, 10, "0xa9059cbb") == 0) {
        token = to;
        rcpt = "0x" + toLower(input.substr(34, 40));
        const bool stable = chainCtx().stablecoins.count(token) > 0;
        const int dec = stable ? 18 : decimalsCached(token);
        const long double raw = hexLD(input, 74, 64);
        if (raw <= 0) return false;
        // Знаков токена нет в памяти — сумма неизвестна (в поток не идёт), но
        // сам перевод нужен: по сливу на кошелёк биржи учится адрес пополнения.
        units = dec < 0 || dec > 36 ? 0 : raw / std::pow(10.0L, dec);
        px = stable ? 1.0L : static_cast<long double>(priceCachedNanos(token, 6 * 3600)) / 1e9L;
    } else {
        return false;
    }
    if (rcpt.size() != 42 || rcpt == "0x0000000000000000000000000000000000000000") return false;
    qty = static_cast<double>(units);
    const long double u = units * px * 1e9L;
    usd = u > 0 && u < static_cast<long double>(SANE_MAX) ? static_cast<long long>(u) : 0;
    return token != chainCtx().nativeMarker || units > 0;
}

// Под g_mx.
const Label* labelLocked(const std::string& a) {
    auto it = g_labels.find(a);
    return it == g_labels.end() ? nullptr : &it->second;
}

// Под g_mx.
bool listedLocked(const std::string& token) {
    return token == chainCtx().nativeMarker || chainCtx().stablecoins.count(token) || g_listed.count(token);
}

// Под g_mx: в памяти не больше LEARNED_MAX выученных — самые старые уходят
// (в базе они остаются до LEARNED_TTL).
void capLearnedLocked() {
    size_t learned = 0;
    for (const auto& [a, lb] : g_labels) learned += !lb.seed;
    if (learned <= LEARNED_MAX) return;
    std::vector<long long> ats;
    ats.reserve(learned);
    for (const auto& [a, lb] : g_labels) if (!lb.seed) ats.push_back(lb.at);
    const size_t drop = learned - LEARNED_MAX * 9 / 10;  // с запасом, чтобы не считать на каждом адресе
    std::nth_element(ats.begin(), ats.begin() + drop, ats.end());
    const long long cut = ats[drop];
    for (auto it = g_labels.begin(); it != g_labels.end();) {
        if (!it->second.seed && it->second.at < cut) it = g_labels.erase(it);
        else ++it;
    }
}

void addFlow(long long ts, const std::string& ex, const std::string& token, bool in, long long usd, double qty) {
    if (usd < FLOW_MIN) return;
    Flow& f = g_flow[std::to_string(ts / BUCKET * BUCKET) + "|" + ex + "|" + token];
    if (in) { f.inUsd += usd; f.inN++; f.inQty += qty; }
    else    { f.outUsd += usd; f.outN++; f.outQty += qty; }
}

void recordMove(const std::string& hash, long long block, long long ts, const std::string& wallet, int kind,
                const std::string& ex, const std::string& token, double qty, long long usd) {
    std::lock_guard<std::mutex> l(g_exDbMx);
    sqlite3_stmt* s;
    if (!prepareOrLog(g_exDb, &s, "INSERT OR IGNORE INTO bsc_ex_moves(tx, block, ts, wallet, kind, ex, token, qty, usd_nanos) "
                              "VALUES(?,?,?,?,?,?,?,?,?)"))
        return;
    sqlite3_bind_text(s, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, block);
    sqlite3_bind_int64(s, 3, ts);
    sqlite3_bind_text(s, 4, wallet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 5, kind);
    sqlite3_bind_text(s, 6, ex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 7, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(s, 8, qty);
    sqlite3_bind_int64(s, 9, usd);
    if (sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(g_exDb) > 0) ++g_moves;
    sqlite3_finalize(s);
}

// Выучили адрес пополнения — ждавшие его заводы засчитываются. Под g_mx.
void resolvePendingLocked(const std::string& addr, const std::string& ex, std::vector<Pend>& movesOut) {
    auto it = g_pend.find(addr);
    if (it == g_pend.end()) return;
    for (const Pend& p : it->second) {
        if (listedLocked(p.token)) addFlow(p.ts, ex, p.token, true, p.usd, p.qty);
        if (p.watched) movesOut.push_back(p);
    }
    g_pendN -= it->second.size();
    g_pend.erase(it);
}

void expirePendingLocked(long long t) {
    while (!g_pendOrder.empty() && (g_pendOrder.front().first < t - PEND_SEC || g_pendN > PEND_MAX)) {
        const auto [ts, addr] = g_pendOrder.front();
        g_pendOrder.pop_front();
        auto it = g_pend.find(addr);
        if (it == g_pend.end()) continue;
        auto& v = it->second;
        const size_t before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Pend& p) { return p.ts <= ts; }), v.end());
        g_pendN -= before - v.size();
        if (v.empty()) g_pend.erase(it);
    }
}

void pricerLoop() {
    while (!g_stop.load()) {
        {
            std::unique_lock<std::mutex> l(g_stopMx);
            g_stopCv.wait_for(l, std::chrono::seconds(20), [] { return g_stop.load(); });
        }
        if (g_stop.load()) break;
        std::vector<std::string> batch;
        {
            std::lock_guard<std::mutex> m(g_mx);
            for (auto it = g_unpriced.begin(); it != g_unpriced.end() && batch.size() < 30;) {
                batch.push_back(*it);
                g_priceTried[*it] = now();
                it = g_unpriced.erase(it);
            }
            if (g_priceTried.size() > 50000) g_priceTried.clear();
        }
        int ok = 0;
        for (const auto& tok : batch) {
            if (g_stop.load()) break;
            try {
                // Кладут в память и в token_cache: следующий перевод оценится,
                // а API покажет тикер.
                if (getDecimals(tok) < 0) continue;
                getSymbol(tok);
                if (getPriceNanos(tok) > 0) ++ok;
            } catch (const std::exception& e) {
                std::cerr << "[BSC-EX] цена " << tok << ": " << e.what() << std::endl;
            }
        }
        if (ok) std::cout << "[BSC-EX] узнали цену монет, прошедших через биржи: " << ok << std::endl;
    }
    g_pricerDone.store(true);
}

}  // namespace

namespace {
void initTables();
void loadLabels();
void workerLoop();
}  // namespace

void initBscExchanges() {
    {
        std::lock_guard<std::mutex> l(g_exDbMx);
        if (g_exDb) return;
        const std::string file = exDbFile();
        if (sqlite3_open(file.c_str(), &g_exDb) != SQLITE_OK) {
            std::cerr << "[BSC-EX] не открыть базу " << file << " — потоки бирж выключены" << std::endl;
            if (g_exDb) { sqlite3_close(g_exDb); g_exDb = nullptr; }
            return;
        }
        sqlite3_busy_timeout(g_exDb, 8000);
        sqlite3_exec(g_exDb, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    }
    initTables();
    loadLabels();
    g_pricer = std::thread(pricerLoop);
    g_worker = std::thread(workerLoop);
}

namespace {
void initTables() {
    std::lock_guard<std::mutex> l(g_exDbMx);
    sqlite3_exec(g_exDb,
                 "CREATE TABLE IF NOT EXISTS bsc_ex_labels (address TEXT PRIMARY KEY, ex TEXT NOT NULL, "
                 "  how TEXT NOT NULL DEFAULT 'learned', at INTEGER NOT NULL DEFAULT 0);"
                 "CREATE TABLE IF NOT EXISTS bsc_ex_moves (id INTEGER PRIMARY KEY AUTOINCREMENT, tx TEXT NOT NULL, "
                 "  block INTEGER NOT NULL, ts INTEGER NOT NULL, wallet TEXT NOT NULL, kind INTEGER NOT NULL, "
                 "  ex TEXT NOT NULL, token TEXT NOT NULL, qty REAL NOT NULL DEFAULT 0, usd_nanos INTEGER NOT NULL DEFAULT 0, "
                 "  UNIQUE(tx, wallet, kind));"
                 "CREATE INDEX IF NOT EXISTS idx_bsc_ex_moves_wallet ON bsc_ex_moves(wallet, ts);"
                 "CREATE INDEX IF NOT EXISTS idx_bsc_ex_moves_ts ON bsc_ex_moves(ts);"
                 "CREATE INDEX IF NOT EXISTS idx_bsc_ex_labels_at ON bsc_ex_labels(at);"
                 "CREATE TABLE IF NOT EXISTS bsc_ex_tokens (token TEXT PRIMARY KEY, at INTEGER NOT NULL DEFAULT 0);"
                 "CREATE TABLE IF NOT EXISTS bsc_ex_flow (ts INTEGER NOT NULL, ex TEXT NOT NULL, token TEXT NOT NULL, "
                 "  in_usd INTEGER NOT NULL DEFAULT 0, out_usd INTEGER NOT NULL DEFAULT 0, "
                 "  in_n INTEGER NOT NULL DEFAULT 0, out_n INTEGER NOT NULL DEFAULT 0, "
                 "  in_qty REAL NOT NULL DEFAULT 0, out_qty REAL NOT NULL DEFAULT 0, PRIMARY KEY (ts, ex, token));",
                 nullptr, nullptr, nullptr);
    sqlite3_stmt* s;
    if (prepareOrLog(g_exDb, &s, "INSERT INTO bsc_ex_labels(address, ex, how, at) VALUES(?,?,'seed',?) "
                             "ON CONFLICT(address) DO UPDATE SET ex=excluded.ex, how='seed' WHERE how!='owner'")) {
        auto put = [&](const char* addr, const char* ex) {
            sqlite3_reset(s);
            sqlite3_bind_text(s, 1, addr, -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 2, ex, -1, SQLITE_STATIC);
            sqlite3_bind_int64(s, 3, now());
            sqlite3_step(s);
        };
        // Сначала адреса из отчётов о резервах, потом ручной список — он
        // главнее и при совпадении правит имя.
        for (const BscSeedBook& sd : BSC_SEEDS_BOOK) put(sd.addr, sd.ex);
        for (const Seed& sd : SEEDS) put(sd.addr, sd.ex);
        sqlite3_finalize(s);
    }
}

void loadLabels() {
    std::lock_guard<std::mutex> l(g_exDbMx);
    sqlite3_stmt* s = nullptr;
    std::lock_guard<std::mutex> m(g_mx);
    if (prepareOrLog(g_exDb, &s, "SELECT address, ex, how, at FROM bsc_ex_labels WHERE how!='learned' "
                                 "UNION ALL SELECT * FROM (SELECT address, ex, how, at FROM bsc_ex_labels "
                                 "WHERE how='learned' ORDER BY at DESC LIMIT ?)")) {
        sqlite3_bind_int64(s, 1, static_cast<long long>(LEARNED_MAX));
        while (sqlite3_step(s) == SQLITE_ROW) {
            Label lb;
            lb.ex = safeColumnText(s, 1);
            const std::string how = safeColumnText(s, 2);
            lb.seed = how == "seed" || how == "owner";  // подписанные владельцем — как стартовые
            lb.at = sqlite3_column_int64(s, 3);
            g_labels[toLower(safeColumnText(s, 0))] = std::move(lb);
        }
        sqlite3_finalize(s);
    }
    if (prepareOrLog(g_exDb, &s, "SELECT token FROM bsc_ex_tokens")) {
        while (sqlite3_step(s) == SQLITE_ROW) g_listed.insert(toLower(safeColumnText(s, 0)));
        sqlite3_finalize(s);
    }
    std::cout << "[BSC-EX] база " << exDbFile() << ", адресов бирж: " << g_labels.size() << " (из них стартовых "
              << std::size(SEEDS) + std::size(BSC_SEEDS_BOOK) << "), торгуемых монет: " << g_listed.size() << std::endl;
}
}  // namespace

std::string bscExchangeOf(const std::string& a) {
    std::lock_guard<std::mutex> m(g_mx);
    const Label* lb = labelLocked(a);
    return lb ? lb->ex : "";
}

namespace {
BscExHit observe(const BscExTx& tx, long long block, long long blockTs,
                 const std::function<bool(const std::string&)>& watched) {
    BscExHit hit;
    const std::string& from = tx.from;
    const std::string& hash = tx.hash;
    if (from.empty() || tx.to.empty()) return hit;
    std::string token, rcpt;
    double qty = 0;
    long long usd = 0;
    if (!parseTransfer(tx, token, rcpt, qty, usd)) return hit;
    const long long t = blockTs > 0 ? blockTs : now();
    const bool unpriced = usd == 0 && token != chainCtx().nativeMarker;

    std::vector<Pend> late;  // заводы, засчитанные задним числом
    std::string exFrom, exTo;
    bool learned = false;
    {
        std::lock_guard<std::mutex> m(g_mx);
        expirePendingLocked(t);
        Label* lf = const_cast<Label*>(labelLocked(from));
        Label* lt = const_cast<Label*>(labelLocked(rcpt));
        if (lf) {
            exFrom = lf->ex;
            // Выученный адрес пополнения снова сливает — он живой.
            if (!lf->seed && t - lf->at > 86400) { lf->at = t; g_dirty.insert(from); }
        }
        if (lt) {
            exTo = lt->ex;
            if (!lt->seed && t - lt->at > 86400) { lt->at = t; g_dirty.insert(rcpt); }
        }
        // Слив на кошелёк биржи из списка с незнакомого адреса — это адрес
        // пополнения той биржи. Наблюдаемые кошельки не учим: человек мог
        // отправить прямо на горячий кошелёк, и он не станет «биржей».
        // Биржа сама вывела монету (с проверенного кошелька) — монета в
        // списке торгуемых.
        if (lf && lf->seed && !lt && token != chainCtx().nativeMarker && g_listed.insert(token).second)
            g_listedNew.insert(token);
        // Торгуемая монета без цены прошла через биржу — в очередь на дозаполнение.
        if (unpriced && (lf || lt) && listedLocked(token) && g_unpriced.size() < UNPRICED_MAX) {
            auto tr = g_priceTried.find(token);
            if (tr == g_priceTried.end() || t - tr->second > PRICE_RETRY_SEC) g_unpriced.insert(token);
        }
        if (lt && lt->seed && !lf && !watched(from) && hexLD(tx.nonce, 2, 64) < LEARN_MAX_NONCE) {
            g_labels[from] = Label{exTo, false, t};
            g_dirty.insert(from);
            exFrom = exTo;
            learned = true;
            ++g_learned;
            resolvePendingLocked(from, exTo, late);
            if (g_learned % 10000 == 0) capLearnedLocked();
        }
        if (!learned) {
            if (!exFrom.empty() && !exTo.empty()) {
                // Внутреннее: между адресами бирж.
            } else if (!exTo.empty()) {
                // Завод — только по торгуемой монете (спам не считаем).
                if (listedLocked(token)) addFlow(t, exTo, token, true, usd, qty);
            } else if (!exFrom.empty()) {
                // Вывод — только с проверенного кошелька биржи: выученный адрес
                // пополнения на сторону не платит, а если платит — это ошибка
                // разметки, и выдавать её за вывод с биржи нельзя.
                if (lf && lf->seed) addFlow(t, exFrom, token, false, usd, qty);
            } else {
                if (hexLD(tx.nonce, 2, 64) < 50) {
                    const long long day = t / 86400;
                    if (day != g_collectDay) { g_collect.clear(); g_collectDay = day; }
                    if (g_collect.size() < COLLECT_MAX || g_collect.count(rcpt)) ++g_collect[rcpt];
                }
                const bool w = watched(from);
                if (usd >= PEND_MIN || (w && usd >= MOVE_MIN)) {
                    g_pend[rcpt].push_back(Pend{from, token, hash, qty, usd, t, block, w});
                    g_pendOrder.emplace_back(t, rcpt);
                    ++g_pendN;
                }
            }
        }
    }
    for (const Pend& p : late)
        recordMove(p.hash, p.block, p.ts, p.from, 2, exFrom, p.token, p.qty, p.usd);
    if (learned) return hit;

    if (!exTo.empty() && exFrom.empty() && watched(from)) {
        if (usd >= MOVE_MIN) recordMove(hash, block, t, from, 2, exTo, token, qty, usd);
        hit = BscExHit{from, exTo, 2};
    } else if (!exFrom.empty() && exTo.empty() && watched(rcpt)) {
        if (usd >= MOVE_MIN) recordMove(hash, block, t, rcpt, 1, exFrom, token, qty, usd);
        hit = BscExHit{rcpt, exFrom, 1};
    }
    return hit;
}

void handleBlock(const QBlock& b) {
    if (b.block <= g_lastBlock) return;
    g_lastBlock = b.block;
    try {
        for (const auto& tx : b.txs) observe(tx, b.block, b.ts, b.watched);
    } catch (const std::exception& e) {
        std::cerr << "[BSC-EX] блок " << b.block << ": " << e.what() << std::endl;
    }
}

void workerLoop() {
    while (!g_stop.load()) {
        QBlock b;
        {
            std::unique_lock<std::mutex> l(g_qMx);
            g_qCv.wait_for(l, std::chrono::seconds(30), [] { return !g_queue.empty() || g_stop.load(); });
            if (g_stop.load()) break;
            if (g_queue.empty()) {
                l.unlock();
                bscExFlush();
                continue;
            }
            b = std::move(g_queue.front());
            g_queue.pop_front();
        }
        handleBlock(b);
        bscExFlush();
    }
    // Остановка: дорабатываем то, что уже в очереди (миллисекунды на блок),
    // иначе эти блоки потерялись бы для потока.
    std::deque<QBlock> rest;
    {
        std::lock_guard<std::mutex> l(g_qMx);
        rest.swap(g_queue);
    }
    for (const auto& b : rest) handleBlock(b);
}

}  // namespace

void bscExEnqueue(long long block, long long blockTs, std::vector<BscExTx> txs,
                  std::function<bool(const std::string&)> watched) {
    {
        std::lock_guard<std::mutex> l(g_qMx);
        if (g_queue.size() >= QUEUE_MAX) {
            g_queue.pop_front();
            if (++g_dropped % 100 == 1)
                std::cerr << "[BSC-EX] очередь полна, старые блоки пропущены: " << g_dropped << std::endl;
        }
        g_queue.push_back(QBlock{block, blockTs, std::move(txs), std::move(watched)});
    }
    g_qCv.notify_one();
}

void bscExFlush(bool force) {
    const long long t = now();
    std::unordered_map<std::string, Flow> flow;
    std::vector<std::pair<std::string, Label>> dirty;
    std::vector<std::string> listedNew;
    long long learned = 0, moves = 0;
    {
        std::lock_guard<std::mutex> m(g_mx);
        if (!force && t - g_lastFlush < 60) return;
        g_lastFlush = t;
        flow.swap(g_flow);
        listedNew.assign(g_listedNew.begin(), g_listedNew.end());
        g_listedNew.clear();
        for (const auto& a : g_dirty) {
            auto it = g_labels.find(a);
            if (it != g_labels.end()) dirty.emplace_back(a, it->second);
        }
        g_dirty.clear();
        learned = g_learned;
        moves = g_moves;
    }
    if (flow.empty() && dirty.empty() && listedNew.empty()) return;
    std::lock_guard<std::mutex> l(g_exDbMx);
    sqlite3_exec(g_exDb, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* s;
    if (!flow.empty() &&
        prepareOrLog(g_exDb, &s, "INSERT INTO bsc_ex_flow(ts, ex, token, in_usd, out_usd, in_n, out_n, in_qty, out_qty) "
                             "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(ts, ex, token) DO UPDATE SET "
                             "in_usd=in_usd+excluded.in_usd, out_usd=out_usd+excluded.out_usd, "
                             "in_n=in_n+excluded.in_n, out_n=out_n+excluded.out_n, "
                             "in_qty=in_qty+excluded.in_qty, out_qty=out_qty+excluded.out_qty")) {
        for (const auto& [key, f] : flow) {
            const size_t a = key.find('|'), b = key.find('|', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            sqlite3_reset(s);
            sqlite3_bind_int64(s, 1, std::stoll(key.substr(0, a)));
            sqlite3_bind_text(s, 2, key.substr(a + 1, b - a - 1).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, key.substr(b + 1).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 4, f.inUsd);
            sqlite3_bind_int64(s, 5, f.outUsd);
            sqlite3_bind_int64(s, 6, f.inN);
            sqlite3_bind_int64(s, 7, f.outN);
            sqlite3_bind_double(s, 8, f.inQty);
            sqlite3_bind_double(s, 9, f.outQty);
            sqlite3_step(s);
        }
        sqlite3_finalize(s);
    }
    if (!dirty.empty() &&
        prepareOrLog(g_exDb, &s, "INSERT INTO bsc_ex_labels(address, ex, how, at) VALUES(?,?,'learned',?) "
                             "ON CONFLICT(address) DO UPDATE SET at=excluded.at WHERE how='learned'")) {
        for (const auto& [a, lb] : dirty) {
            sqlite3_reset(s);
            sqlite3_bind_text(s, 1, a.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, lb.ex.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, lb.at);
            sqlite3_step(s);
        }
        sqlite3_finalize(s);
    }
    if (!listedNew.empty() && prepareOrLog(g_exDb, &s, "INSERT OR IGNORE INTO bsc_ex_tokens(token, at) VALUES(?,?)")) {
        for (const auto& tok : listedNew) {
            sqlite3_reset(s);
            sqlite3_bind_text(s, 1, tok.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, t);
            sqlite3_step(s);
        }
        sqlite3_finalize(s);
    }
    sqlite3_exec(g_exDb, "COMMIT", nullptr, nullptr, nullptr);
    static long long lastLog = 0;
    if (t - lastLog >= 3600) {
        lastLog = t;
        std::cout << "[BSC-EX] выучено адресов пополнения: " << learned << ", заводов и выводов кошельков: " << moves
                  << " (с запуска)" << std::endl;
    }
}

void bscExCleanup() {
    const long long t = now();
    {
        std::lock_guard<std::mutex> l(g_exDbMx);
        sqlite3_stmt* s;
        if (prepareOrLog(g_exDb, &s, "DELETE FROM bsc_ex_flow WHERE ts < ?")) {
            sqlite3_bind_int64(s, 1, t - FLOW_KEEP);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        if (prepareOrLog(g_exDb, &s, "DELETE FROM bsc_ex_moves WHERE ts < ?")) {
            sqlite3_bind_int64(s, 1, t - 400LL * 86400LL);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        if (prepareOrLog(g_exDb, &s, "DELETE FROM bsc_ex_labels WHERE how='learned' AND at < ?")) {
            sqlite3_bind_int64(s, 1, t - LEARNED_TTL);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }
    std::lock_guard<std::mutex> m(g_mx);
    for (auto it = g_labels.begin(); it != g_labels.end();) {
        if (!it->second.seed && it->second.at < t - LEARNED_TTL) it = g_labels.erase(it);
        else ++it;
    }
}

std::string bscExLabelCommand(const std::string& arg) {
    std::istringstream in(arg);
    std::string addr, name;
    in >> addr;
    std::getline(in, name);
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    addr = toLower(addr);
    if (addr.size() != 42 || addr.rfind("0x", 0) != 0 || name.empty() || name.size() > 32 ||
        name.find_first_of("<>&") != std::string::npos) {
        std::lock_guard<std::mutex> m(g_mx);
        std::map<std::string, std::array<long long, 2>> per;  // биржа → стартовые/подписанные, выученные
        for (const auto& [a, lb] : g_labels) per[lb.ex][lb.seed ? 0 : 1]++;
        std::ostringstream o;
        o << "🏦 <b>Биржи BSC</b> — адреса: кошельки бирж · выученные адреса пополнения\n";
        for (const auto& [ex, n] : per) o << "\n" << ex << ": " << n[0] << " · " << n[1];
        o << "\n\nПодписать адрес: <code>/exlabel 0x… Binance</code>"
             "\nСнять подпись: <code>/exlabel 0x… -</code>"
             "\nКандидаты без подписи: <code>/exunknown</code>";
        return o.str();
    }
    const long long t = now();
    std::lock_guard<std::mutex> l(g_exDbMx);
    sqlite3_stmt* s;
    if (name == "-") {
        if (prepareOrLog(g_exDb, &s, "DELETE FROM bsc_ex_labels WHERE address=?")) {
            sqlite3_bind_text(s, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        std::lock_guard<std::mutex> m(g_mx);
        g_labels.erase(addr);
        return "Подпись снята: <code>" + addr + "</code>";
    }
    if (prepareOrLog(g_exDb, &s, "INSERT INTO bsc_ex_labels(address, ex, how, at) VALUES(?,?,'owner',?) "
                             "ON CONFLICT(address) DO UPDATE SET ex=excluded.ex, how='owner', at=excluded.at")) {
        sqlite3_bind_text(s, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, t);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    std::lock_guard<std::mutex> m(g_mx);
    g_labels[addr] = Label{name, true, t};
    g_collect.erase(addr);
    return "✅ <code>" + addr + "</code> — <b>" + name +
           "</b>. Бот будет учить адреса пополнения, которые сливают сюда деньги.";
}

std::string bscExUnknownCommand() {
    // Известные сборщики, которые не биржи: в поток бирж их не берём, а в
    // списке «без подписи» они только заслоняют настоящие биржи.
    static const std::unordered_set<std::string> NOT_EXCHANGE = {
        "0xfa500178de024bf43cfa69b7e636a28ab68f2741",  // Stake.com 6 — казино
        "0xb0b1177e8b32bfb9744c50f1350c6a614cc1aa77",  // Cobo — кастоди
    };
    std::vector<std::pair<long long, std::string>> top;
    {
        std::lock_guard<std::mutex> m(g_mx);
        for (const auto& [a, n] : g_collect)
            if (n >= 10 && !g_labels.count(a) && !NOT_EXCHANGE.count(a)) top.emplace_back(n, a);
    }
    std::sort(top.rbegin(), top.rend());
    if (top.size() > 15) top.resize(15);
    std::ostringstream o;
    o << "🔎 <b>Сборщики без подписи</b> (сегодня, UTC)\n"
         "Сюда сливают деньги молодые кошельки — так биржа собирает деньги с адресов пополнения. "
         "Бывает и ферма аирдропов: проверьте адрес на BscScan.\n";
    if (top.empty()) o << "\nПока никого — загляните позже.";
    for (const auto& [n, a] : top)
        o << "\n<a href=\"https://bscscan.com/address/" << a << "\">" << a.substr(0, 10) << "…" << a.substr(36)
          << "</a> — " << n << " сливов · <code>" << a << "</code>";
    o << "\n\nПодписать: <code>/exlabel 0x… Binance</code>";
    return o.str();
}

void bscExStop() {
    g_stop.store(true);
    g_qCv.notify_all();
    g_stopCv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    // Поток цен может ждать ответа сети (запрос цены — до десятков секунд):
    // остановку бота из-за этого не держим — даём пять секунд и отпускаем.
    for (int i = 0; i < 50 && !g_pricerDone.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (g_pricer.joinable()) {
        if (g_pricerDone.load()) g_pricer.join();
        else g_pricer.detach();
    }
    bscExFlush(true);
    std::lock_guard<std::mutex> l(g_exDbMx);
    if (g_exDb) {
        sqlite3_close(g_exDb);
        g_exDb = nullptr;
    }
}
