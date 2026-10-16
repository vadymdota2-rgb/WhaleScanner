// Биржевые фонды в сети Bitcoin. Что и почему — в btc_etf.h.
#include "btc_etf.h"
#include "btc_etf_book.h"
#include "btc_etf_list.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace {

constexpr long long SAT = 100000000LL;
// Пыль — меньше 0,01 BTC: так хранитель платит комиссию сети за трату
// кошелька (присылает пылинку и забирает остаток). Это не деньги фонда.
constexpr long long DUST = SAT / 100;
// Сверка остатка кошелька с цепочкой — раз в сутки.
constexpr long long CHECK_SEC = 86400;

// Плата за управление, базисные пункты в год (проспект фонда). Нужна, чтобы
// отличить её от оттока инвесторов.
// Плата за управление, базисные пункты в год (проспекты фондов).
const std::map<std::string, int> FEE_BPS = {{"BITB", 20}, {"HODL", 20}, {"BTCW", 25}, {"EZBC", 19}, {"IBIT", 25}};

enum Kind { IN = 1, OUT = 2, FEE = 3, INTERNAL = 4 };

// Больше стольких новых кошельков за одну трату не учим: у фонда так не
// бывает, а ошибка в одной трате утянула бы в фонд чужие кошельки.
constexpr size_t LEARN_MAX = 20;

std::mutex g_mx;
std::unordered_map<std::string, std::string> g_fund;  // адрес → фонд
// Типы адресов фонда (по опубликованным): выученный кошелёк — того же типа.
std::map<std::string, std::set<int>> g_types;
// Служебные кошельки хранителя: присылают фондам пылинки на комиссию (и
// кошелёк, с которого приходят новые монеты фонда, тоже). Они общие для всех
// клиентов хранителя — фонду не принадлежат и не учатся никогда.
std::set<std::string> g_service;
// Хранилища хранителя: на них кошелёк комиссий хранителя присылает пылинку
// перед каждой тратой. Хранилище — всегда одного клиента (за 17 дней и 2 545
// трат ни одно не потратилось вместе с чужим), но чьё — по самой цепочке не
// видно: это решает разбор переводов по «корзинам» фондов в API.
std::set<std::string> g_vault;
// Кошельки комиссий хранителя: подтверждены, когда прислали пылинку трём
// разным известным хранилищам (кошелькам фондов).
std::set<std::string> g_feeWallet;
std::map<std::string, std::set<std::string>> g_feeSeen;
// Новые хранилища, чью историю ещё надо дочитать: приток в хранилище бывает
// раньше, чем хранитель впервые пришлёт ему пылинку.
std::vector<std::string> g_vaultQueue;
// Перевод хранилища пишется с этой суммы: корзины фондов — от полубиткоина,
// мельче — пыль и сдача.
constexpr long long XFER_MIN = SAT / 20;
constexpr size_t FEE_CONFIRM = 3;
// Кошельки комиссий, найденные по цепочке заранее: 3J7c… прислал пылинки
// кошелькам BITB 206 раз за два года (перед каждой их тратой). Без затравки
// сканер дошёл бы до него сам за неделю-две — и потерял бы эти недели.
const std::vector<const char*> FEE_SEEDS = {"3J7cUjBZxvGRCwFBz3q23zAsnhFfZrDSSU"};

// Сверки за сутки: когда, фонд, на сколько разошлось.
struct Check { long long at; std::string fund; long long drift; };
std::vector<Check> g_checks;
long long g_mixed = 0;

int addrType(const std::string& a) {
    if (a.rfind("bc1p", 0) == 0) return 4;
    if (a.rfind("bc1q", 0) == 0) return a.size() > 50 ? 3 : 2;
    if (!a.empty() && a[0] == '3') return 1;
    if (!a.empty() && a[0] == '1') return 0;
    return -1;
}

long long now() { return static_cast<long long>(std::time(nullptr)); }
long long dayOf(long long ts) { return ts - ts % 86400; }

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
            std::cerr << "[ETF] prepare: " << sqlite3_errmsg(db) << " | " << sql << std::endl;
            s = nullptr;
        }
    }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    Stmt& text(int i, const std::string& v) {
        if (s) sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& num(int i, long long v) {
        if (s) sqlite3_bind_int64(s, i, v);
        return *this;
    }
    bool row() { return s && sqlite3_step(s) == SQLITE_ROW; }
    bool run() { return s && sqlite3_step(s) == SQLITE_DONE; }
    long long col(int i) { return sqlite3_column_int64(s, i); }
    std::string str(int i) {
        const unsigned char* t = sqlite3_column_text(s, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
};

std::string fundOf(const std::string& a) {
    if (a.empty()) return "";
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_fund.find(a);
    return it == g_fund.end() ? "" : it->second;
}

long long holdings(sqlite3* db, const std::string& fund) {
    Stmt q(db, "SELECT COALESCE(SUM(bal_sats), 0) FROM btc_etf_addr WHERE fund=?");
    q.text(1, fund);
    return q.row() ? q.col(0) : 0;
}

void addFlow(sqlite3* db, long long ts, const std::string& fund, int kind, long long sats, int sign) {
    const char* sql =
        kind == IN ? "INSERT INTO btc_etf_flow(day, fund, in_sats, in_n) VALUES(?,?,?,?) "
                     "ON CONFLICT(day, fund) DO UPDATE SET in_sats=in_sats+excluded.in_sats, in_n=in_n+excluded.in_n"
        : kind == OUT ? "INSERT INTO btc_etf_flow(day, fund, out_sats, out_n) VALUES(?,?,?,?) "
                        "ON CONFLICT(day, fund) DO UPDATE SET out_sats=out_sats+excluded.out_sats, out_n=out_n+excluded.out_n"
                      : "INSERT INTO btc_etf_flow(day, fund, fee_sats, fee_n) VALUES(?,?,?,?) "
                        "ON CONFLICT(day, fund) DO UPDATE SET fee_sats=fee_sats+excluded.fee_sats, fee_n=fee_n+excluded.fee_n";
    Stmt q(db, sql);
    q.num(1, dayOf(ts)).text(2, fund).num(3, sign * sats).num(4, sign).run();
}

// Строка движения; поток — только если строка новая (блок, посчитанный
// второй раз, ничего не удвоит).
void putMove(sqlite3* db, const Block& b, const BTx& tx, const std::string& fund, int kind, long long sats,
             const std::string& peer) {
    Stmt q(db, "INSERT OR IGNORE INTO btc_etf_moves(txid, height, ts, fund, kind, sats, peer) VALUES(?,?,?,?,?,?,?)");
    q.text(1, tx.txid).num(2, b.height).num(3, b.ts).text(4, fund).num(5, kind).num(6, sats).text(7, peer);
    if (q.run() && sqlite3_changes(db) > 0) addFlow(db, b.ts, fund, kind, sats, 1);
}

// Отток на этот адрес был перекладкой внутри фонда — снимаем его задним числом.
void unOut(sqlite3* db, const std::string& fund, const std::string& addr) {
    struct Row { long long id, ts, sats; int kind; };
    std::vector<Row> rows;
    {
        Stmt q(db, "SELECT id, ts, sats, kind FROM btc_etf_moves WHERE fund=? AND peer=? AND kind IN (2, 3)");
        q.text(1, fund).text(2, addr);
        while (q.row()) rows.push_back({q.col(0), q.col(1), q.col(2), static_cast<int>(q.col(3))});
    }
    for (const auto& r : rows) {
        addFlow(db, r.ts, fund, r.kind, r.sats, -1);
        Stmt u(db, "UPDATE btc_etf_moves SET kind=4 WHERE id=?");
        u.num(1, r.id).run();
    }
    if (!rows.empty())
        std::cout << "[ETF] " << fund << ": " << addr << " — свой кошелёк, снято оттоков: " << rows.size() << std::endl;
}

// Платил ли фонд на этот адрес раньше (отток) — тогда это участник фонда.
bool paidBefore(sqlite3* db, const std::string& fund, const std::string& addr) {
    Stmt q(db, "SELECT 1 FROM btc_etf_moves WHERE fund=? AND peer=? AND kind=2 LIMIT 1");
    q.text(1, fund).text(2, addr);
    return q.row();
}

// Новый ли адрес в цепочке (первая его трата — эта): спрашивает btc_chain.
EtfFreshCheck g_fresh;
std::map<std::string, int> g_freshMemo;  // только твёрдые ответы: 1 — новый, 0 — жил раньше

bool freshAddr(const std::string& addr, const std::string& txid, long long height) {
    if (auto it = g_freshMemo.find(addr); it != g_freshMemo.end()) return it->second == 1;
    const int r = g_fresh ? g_fresh(addr, txid, height) : -1;
    if (r >= 0) g_freshMemo[addr] = r;
    return r == 1;
}

// Переезд учим только у фондов без опубликованного списка: у BITB список
// кошельков фонд ведёт сам, и чужой адрес в его запас не попадёт.
bool learnableMove(const std::string& addr, const std::string& fund) {
    for (const auto& [a, f] : BTC_ETF_BOOK)
        if (fund == f) return false;
    std::lock_guard<std::mutex> l(g_mx);
    return !addr.empty() && !g_service.count(addr);
}

bool learnable(const std::string& addr, const std::string& fund) {
    std::lock_guard<std::mutex> l(g_mx);
    if (g_service.count(addr)) return false;
    auto t = g_types.find(fund);
    return t != g_types.end() && t->second.count(addrType(addr));
}

void markService(sqlite3* db, const std::string& addr) {
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_service.insert(addr).second) return;
    }
    Stmt q(db, "INSERT OR IGNORE INTO btc_etf_svc(address, at) VALUES(?,?)");
    q.text(1, addr).num(2, now()).run();
    std::cout << "[ETF] служебный кошелёк хранителя (шлёт фондам пыль): " << addr << std::endl;
}

void learn(sqlite3* db, const std::string& addr, const std::string& fund, long long height) {
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_fund.emplace(addr, fund).second) return;
    }
    // Остаток нового кошелька неизвестен: этот блок в него не пишем, сверка
    // с цепочкой (bal_at = 0 — первым в очереди) даст настоящий.
    Stmt q(db, "INSERT OR IGNORE INTO btc_etf_addr(address, fund, how, at, bal_height) VALUES(?,?,'cospend',?,?)");
    q.text(1, addr).text(2, fund).num(3, now()).num(4, height).run();
    unOut(db, fund, addr);
}

void moveBalance(sqlite3* db, const std::string& addr, long long delta, long long height) {
    Stmt q(db, "UPDATE btc_etf_addr SET bal_sats=bal_sats+?, at=? WHERE address=? AND bal_height < ?");
    q.num(1, delta).num(2, now()).text(3, addr).num(4, height).run();
}

// Плата за управление: в последние три или первые три дня месяца, и сумма —
// месячная доля платы от запаса фонда (от половины до полутора).
bool isFee(sqlite3* db, const std::string& fund, long long sats, long long ts) {
    auto it = FEE_BPS.find(fund);
    if (it == FEE_BPS.end()) return false;
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm d{};
    gmtime_r(&t, &d);
    if (d.tm_mday > 3 && d.tm_mday < 28) return false;
    const long double month = static_cast<long double>(holdings(db, fund)) * it->second / 10000.0L / 12.0L;
    return month > 0 && sats >= month * 0.5L && sats <= month * 1.5L;
}

bool isVault(const std::string& a) {
    if (a.empty()) return false;
    std::lock_guard<std::mutex> l(g_mx);
    return g_vault.count(a) > 0;
}

void addVault(sqlite3* db, const std::string& a, const std::string& fee, long long height, bool retro) {
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_vault.insert(a).second) return;
        if (retro && g_vaultQueue.size() < 5000) g_vaultQueue.push_back(a);
    }
    Stmt q(db, "INSERT OR IGNORE INTO btc_cust_vault(address, fee_wallet, at, height, read) VALUES(?,?,?,?,?)");
    q.text(1, a).text(2, fee).num(3, now()).num(4, height).num(5, retro ? 0 : 1).run();
}

// Подпитка комиссией: все входы — один адрес F, выходов два — пылинка на V и
// сдача обратно на F. Так хранитель готовит хранилище к трате.
bool feeTopUp(sqlite3* db, const Block& b, const BTx& tx) {
    if (tx.in.empty() || tx.out.size() != 2) return false;
    const std::string& f = tx.in[0].addr;
    if (f.empty()) return false;
    for (const auto& i : tx.in) if (i.addr != f) return false;
    const BOut* dust = nullptr;
    const BOut* back = nullptr;
    for (const auto& o : tx.out) {
        if (o.addr == f) back = &o;
        else if (!o.addr.empty() && o.sats < DUST) dust = &o;
    }
    if (!dust || !back) return false;
    bool confirmed;
    {
        std::lock_guard<std::mutex> l(g_mx);
        confirmed = g_feeWallet.count(f) > 0;
    }
    if (confirmed) {
        addVault(db, dust->addr, f, b.height, true);
        return true;
    }
    // Ещё не подтверждён: считаем, скольким известным хранилищам он платит.
    bool known = !fundOf(dust->addr).empty() || isVault(dust->addr);
    if (!known) return false;
    size_t n;
    {
        std::lock_guard<std::mutex> l(g_mx);
        auto& seen = g_feeSeen[f];
        if (seen.size() < 64) seen.insert(dust->addr);
        n = seen.size();
        if (n >= FEE_CONFIRM) g_feeWallet.insert(f);
    }
    if (n >= FEE_CONFIRM) {
        Stmt q(db, "INSERT INTO btc_etf_svc(address, at, fee) VALUES(?,?,1) ON CONFLICT(address) DO UPDATE SET fee=1");
        q.text(1, f).num(2, now()).run();
        std::cout << "[ETF] кошелёк комиссий хранителя подтверждён: " << f << std::endl;
    }
    return false;
}

// Перевод, где хотя бы одна сторона — хранилище: сырой факт цепочки для
// разбора по корзинам. Сдача (выход на адрес входа) и пыль не пишутся.
void custTx(sqlite3* db, const Block& b, const BTx& tx) {
    if (tx.coinbase || tx.in.empty()) return;
    if (feeTopUp(db, b, tx)) return;
    std::set<std::string> ins;
    const BIn* top = nullptr;
    bool fromVault = false;
    for (const auto& i : tx.in) {
        ins.insert(i.addr);
        if (isVault(i.addr)) fromVault = true;
        if (!top || i.sats > top->sats) top = &i;
    }
    for (size_t n = 0; n < tx.out.size(); ++n) {
        const auto& o = tx.out[n];
        if (o.addr.empty() || o.sats < XFER_MIN || ins.count(o.addr)) continue;
        const bool toVault = isVault(o.addr);
        if (!fromVault && !toVault) continue;
        {
            std::lock_guard<std::mutex> l(g_mx);
            if (g_feeWallet.count(o.addr)) continue;
        }
        Stmt q(db, "INSERT OR IGNORE INTO btc_cust_xfer(txid, n, height, ts, src, dst, sats, sv, dv) "
                   "VALUES(?,?,?,?,?,?,?,?,?)");
        q.text(1, tx.txid).num(2, static_cast<long long>(n)).num(3, b.height).num(4, b.ts)
            .text(5, top ? top->addr : "").text(6, o.addr).num(7, o.sats).num(8, fromVault ? 1 : 0)
            .num(9, toVault ? 1 : 0).run();
    }
}

void scanTx(sqlite3* db, const Block& b, const BTx& tx) {
    custTx(db, b, tx);
    if (tx.coinbase) return;
    std::map<std::string, long long> inF;
    const BIn* top = nullptr;
    for (const auto& i : tx.in) {
        const std::string f = fundOf(i.addr);
        if (!f.empty()) inF[f] += i.sats;
        if (!top || i.sats > top->sats) top = &i;
    }
    std::map<std::string, long long> outF;
    for (const auto& o : tx.out) {
        const std::string f = fundOf(o.addr);
        if (!f.empty()) outF[f] += o.sats;
    }
    if (inF.empty() && outF.empty()) return;

    if (inF.size() > 1) {
        // Два фонда в одной трате хранитель не смешивает. Случилось — не
        // учимся и потоков не пишем, остатки ведём.
        ++g_mixed;
        std::cerr << "[ETF] в одной трате кошельки разных фондов: " << tx.txid << std::endl;
    } else if (inF.size() == 1) {
        const std::string fund = inF.begin()->first;
        std::set<std::string> fresh;
        for (const auto& i : tx.in)
            if (!i.addr.empty() && fundOf(i.addr).empty() && learnable(i.addr, fund)) fresh.insert(i.addr);
        if (fresh.size() > LEARN_MAX)
            std::cerr << "[ETF] " << fund << ": трата учит сразу " << fresh.size() << " кошельков — не учусь, "
                      << tx.txid << std::endl;
        else
            for (const auto& a : fresh) learn(db, a, fund, b.height);
        std::map<std::string, long long> dest;
        for (const auto& o : tx.out) {
            if (o.addr.empty() || o.sats < DUST) continue;
            const std::string f = fundOf(o.addr);
            if (f == fund) continue;
            dest[o.addr] += o.sats;
        }
        // Переезд: фонд отдал почти всё, что тратил (от 90 %), и не меньше
        // половины всего запаса, на адрес, которого до этой траты в цепочке не
        // было и которому фонд раньше не платил, — это его новый кошелёк, а не
        // отток. Так хранитель перекладывает запас: старый адрес пустеет, новый
        // держит всё. Старый чужой кошелёк (39wVd42… жил с 2023 года и принял
        // от HODL 8 975 BTC) переездом не считается: это отток.
        const long long spent = inF.begin()->second;
        const long long held = holdings(db, fund);
        for (auto it = dest.begin(); it != dest.end();) {
            if (it->second * 10 >= spent * 9 && it->second * 2 >= held && fundOf(it->first).empty() &&
                !paidBefore(db, fund, it->first) && learnableMove(it->first, fund) &&
                freshAddr(it->first, tx.txid, b.height)) {
                learn(db, it->first, fund, b.height);
                std::cout << "[ETF] " << fund << ": запас переехал на " << it->first << std::endl;
                it = dest.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [a, v] : dest) {
            const std::string other = fundOf(a);
            putMove(db, b, tx, fund, !other.empty() || !isFee(db, fund, v, b.ts) ? OUT : FEE, v, a);
            if (!other.empty()) putMove(db, b, tx, other, IN, v, top ? top->addr : "");
        }
    } else {
        bool dust = false;
        for (const auto& o : tx.out)
            if (o.sats < DUST && !fundOf(o.addr).empty()) dust = true;
        // Пылинка на кошелёк фонда — хранитель платит комиссию: его входы —
        // кошельки комиссий.
        if (dust)
            for (const auto& i : tx.in)
                if (!i.addr.empty()) markService(db, i.addr);
        for (const auto& [f, v] : outF)
            if (v >= DUST) putMove(db, b, tx, f, IN, v, top ? top->addr : "");
    }

    for (const auto& i : tx.in)
        if (!fundOf(i.addr).empty()) moveBalance(db, i.addr, -i.sats, b.height);
    for (const auto& o : tx.out)
        if (!fundOf(o.addr).empty()) moveBalance(db, o.addr, o.sats, b.height);
}

std::string btc(long long sats) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(sats) / SAT);
    std::string r = buf;
    for (auto& ch : r) if (ch == '.') ch = ',';
    return r;
}

}  // namespace

std::string etfFund(const std::string& address) { return fundOf(address); }

std::vector<std::string> etfVaultsToRead(size_t limit) {
    std::lock_guard<std::mutex> l(g_mx);
    const size_t n = std::min(limit, g_vaultQueue.size());
    std::vector<std::string> r(g_vaultQueue.begin(), g_vaultQueue.begin() + static_cast<long>(n));
    g_vaultQueue.erase(g_vaultQueue.begin(), g_vaultQueue.begin() + static_cast<long>(n));
    return r;
}

void etfVaultTx(sqlite3* db, long long height, long long ts, const BTx& tx) {
    Block b;
    b.height = height;
    b.ts = ts;
    custTx(db, b, tx);
}

void etfVaultRetry(const std::string& address) {
    std::lock_guard<std::mutex> l(g_mx);
    if (g_vaultQueue.size() < 5000) g_vaultQueue.push_back(address);
}

void etfVaultRead(sqlite3* db, const std::string& address) {
    Stmt q(db, "UPDATE btc_cust_vault SET read=1 WHERE address=?");
    q.text(1, address).run();
}

void etfSetFreshCheck(EtfFreshCheck f) { g_fresh = std::move(f); }

std::vector<std::string> etfAddresses() {
    std::lock_guard<std::mutex> l(g_mx);
    std::vector<std::string> r;
    r.reserve(g_fund.size());
    for (const auto& [a, f] : g_fund) r.push_back(a);
    std::sort(r.begin(), r.end());
    return r;
}

void etfOpen(sqlite3* db) {
    char* err = nullptr;
    const char* schema =
        // how: seed — опубликован фондом, cospend — потрачен вместе с его
        // кошельком. bal_height — остаток верен на этой высоте, блоки до неё
        // в него не пишутся; bal_at — когда сверен с цепочкой (0 — ещё нет).
        "CREATE TABLE IF NOT EXISTS btc_etf_addr ("
        "  address TEXT PRIMARY KEY, fund TEXT NOT NULL, how TEXT NOT NULL, at INTEGER NOT NULL DEFAULT 0,"
        "  bal_sats INTEGER NOT NULL DEFAULT 0, bal_height INTEGER NOT NULL DEFAULT 0,"
        "  bal_at INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_btc_etf_addr_fund ON btc_etf_addr(fund);"
        // kind 1 — приток, 2 — отток, 3 — плата за управление, 4 — оказалось
        // перекладкой внутри фонда. peer — второй адрес движения.
        "CREATE TABLE IF NOT EXISTS btc_etf_moves ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, txid TEXT NOT NULL, height INTEGER NOT NULL,"
        "  ts INTEGER NOT NULL, fund TEXT NOT NULL, kind INTEGER NOT NULL, sats INTEGER NOT NULL,"
        "  peer TEXT NOT NULL DEFAULT '', UNIQUE(txid, fund, peer, kind));"
        "CREATE INDEX IF NOT EXISTS idx_btc_etf_moves_height ON btc_etf_moves(height);"
        "CREATE INDEX IF NOT EXISTS idx_btc_etf_moves_peer ON btc_etf_moves(fund, peer);"
        "CREATE INDEX IF NOT EXISTS idx_btc_etf_moves_ts ON btc_etf_moves(ts);"
        // Поток по суткам UTC: его читает API.
        "CREATE TABLE IF NOT EXISTS btc_etf_flow ("
        "  day INTEGER NOT NULL, fund TEXT NOT NULL,"
        "  in_sats INTEGER NOT NULL DEFAULT 0, out_sats INTEGER NOT NULL DEFAULT 0,"
        "  fee_sats INTEGER NOT NULL DEFAULT 0, in_n INTEGER NOT NULL DEFAULT 0,"
        "  out_n INTEGER NOT NULL DEFAULT 0, fee_n INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (day, fund));"
        // Запас фонда на конец суток — по сверке с цепочкой.
        "CREATE TABLE IF NOT EXISTS btc_etf_svc (address TEXT PRIMARY KEY, at INTEGER NOT NULL DEFAULT 0);"
        // Хранилища хранителя и переводы с ними — сырьё для разбора по
        // корзинам фондов (API). read — история хранилища дочитана.
        "CREATE TABLE IF NOT EXISTS btc_cust_vault ("
        "  address TEXT PRIMARY KEY, fee_wallet TEXT NOT NULL DEFAULT '', at INTEGER NOT NULL DEFAULT 0,"
        "  height INTEGER NOT NULL DEFAULT 0, read INTEGER NOT NULL DEFAULT 0);"
        // src — главный вход перевода, dst — получатель; sv/dv — хранилище ли.
        "CREATE TABLE IF NOT EXISTS btc_cust_xfer ("
        "  txid TEXT NOT NULL, n INTEGER NOT NULL, height INTEGER NOT NULL, ts INTEGER NOT NULL,"
        "  src TEXT NOT NULL, dst TEXT NOT NULL, sats INTEGER NOT NULL, sv INTEGER NOT NULL, dv INTEGER NOT NULL,"
        "  PRIMARY KEY (txid, n));"
        "CREATE INDEX IF NOT EXISTS idx_btc_cust_xfer_ts ON btc_cust_xfer(ts);"
        // Разметка владельца (/import): адрес → фонд. Пишет любой процесс бота,
        // в разметку переносит тот, что читает блоки (etfDrainLabels).
        "CREATE TABLE IF NOT EXISTS btc_etf_import ("
        "  address TEXT PRIMARY KEY, fund TEXT NOT NULL, at INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS btc_etf_hold ("
        "  day INTEGER NOT NULL, fund TEXT NOT NULL, sats INTEGER NOT NULL, n INTEGER NOT NULL,"
        "  PRIMARY KEY (day, fund));";
    if (sqlite3_exec(db, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[ETF] схема: " << (err ? err : "?") << std::endl;
        sqlite3_free(err);
    }
    // Колонка появилась позже: на старой базе её нет, ошибку «уже есть» глотаем.
    sqlite3_exec(db, "ALTER TABLE btc_etf_svc ADD COLUMN fee INTEGER NOT NULL DEFAULT 0", nullptr, nullptr, nullptr);
    // hist = 0 — история кошелька за два месяца ещё не прочитана (размечен
    // через /import): её дочитывает etfHistQueue понемногу.
    sqlite3_exec(db, "ALTER TABLE btc_etf_addr ADD COLUMN hist INTEGER NOT NULL DEFAULT 1", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    for (const char* f : FEE_SEEDS) {
        Stmt q(db, "INSERT INTO btc_etf_svc(address, at, fee) VALUES(?,?,1) ON CONFLICT(address) DO UPDATE SET fee=1");
        q.text(1, f).num(2, now()).run();
    }
    // Кошельки фондов — тоже хранилища хранителя.
    for (const auto& [a, fund] : BTC_ETF_BOOK) {
        Stmt q(db, "INSERT OR IGNORE INTO btc_cust_vault(address, fee_wallet, at, read) VALUES(?, '', ?, 1)");
        q.text(1, a).num(2, now()).run();
    }
    for (const auto& [a, fund] : BTC_ETF_BOOK) {
        Stmt q(db, "INSERT INTO btc_etf_addr(address, fund, how, at) VALUES(?,?,'seed',?) "
                   "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, how='seed'");
        q.text(1, a).text(2, fund).num(3, now()).run();
    }
    // Кошельки из чужих списков, проверенные по цепочке (btc_etf_list.h):
    // опубликованного фондом и размеченного владельцем (/import) не трогают,
    // выученное — уточняют.
    for (const auto& [a, fund] : BTC_ETF_LIST) {
        Stmt q(db, "INSERT INTO btc_etf_addr(address, fund, how, at) VALUES(?,?,'list',?) "
                   "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, how='list' "
                   "WHERE btc_etf_addr.how NOT IN ('seed', 'list')");
        q.text(1, a).text(2, fund).num(3, now()).run();
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    std::lock_guard<std::mutex> l(g_mx);
    g_fund.clear();
    g_types.clear();
    g_service.clear();
    for (const auto& [a, fund] : BTC_ETF_BOOK) g_types[fund].insert(addrType(a));
    Stmt q(db, "SELECT address, fund, how FROM btc_etf_addr");
    while (q.row()) {
        g_fund[q.str(0)] = q.str(1);
        // Типы адресов фонда — по его спискам (свой и размеченный владельцем).
        if (q.str(2) == "list") g_types[q.str(1)].insert(addrType(q.str(0)));
    }
    Stmt v(db, "SELECT address, fee FROM btc_etf_svc");
    while (v.row()) {
        g_service.insert(v.str(0));
        if (v.col(1)) g_feeWallet.insert(v.str(0));
    }
    g_vault.clear();
    g_vaultQueue.clear();
    Stmt w(db, "SELECT address, read FROM btc_cust_vault");
    while (w.row()) {
        g_vault.insert(w.str(0));
        if (!w.col(1)) g_vaultQueue.push_back(w.str(0));
    }
    std::cout << "[ETF] кошельков фондов: " << g_fund.size() << ", хранилищ хранителя: " << g_vault.size()
              << ", кошельков комиссий: " << g_feeWallet.size() << std::endl;
}

void etfBlock(sqlite3* db, const Block& b) {
    for (const auto& tx : b.txs) scanTx(db, b, tx);
}

EtfImportResult etfImport(sqlite3* db, const std::vector<std::string>& addrs, const std::string& fund) {
    EtfImportResult r;
    Stmt was(db, "SELECT fund, how FROM btc_etf_addr WHERE address=?");
    // База китов (btc_watch) живёт в той же btc.db; в тестах её может не быть.
    sqlite3_stmt* probe = nullptr;
    const bool hasWatch = sqlite3_prepare_v2(db, "SELECT 1 FROM btc_watch LIMIT 0", -1, &probe, nullptr) == SQLITE_OK;
    sqlite3_finalize(probe);
    for (const auto& a : addrs) {
        sqlite3_reset(was.s);
        was.text(1, a);
        if (was.row()) {
            if (was.str(1) == "seed") { ++r.seed; continue; }
            if (was.str(0) == fund) { ++r.dup; continue; }
            if (!was.str(0).empty() && was.str(0)[0] == '#') ++r.named;
            else ++r.moved;
        } else if (hasWatch) {
            Stmt w(db, "SELECT 1 FROM btc_watch WHERE address=?");
            w.text(1, a);
            if (w.row()) ++r.inBase;
        }
        Stmt q(db, "INSERT INTO btc_etf_import(address, fund, at) VALUES(?,?,?) "
                   "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, at=excluded.at");
        q.text(1, a).text(2, fund).num(3, now()).run();
        ++r.queued;
    }
    return r;
}

int etfDrainLabels(sqlite3* db) {
    std::vector<std::pair<std::string, std::string>> rows;
    {
        Stmt q(db, "SELECT address, fund FROM btc_etf_import");
        while (q.row()) rows.emplace_back(q.str(0), q.str(1));
    }
    if (rows.empty()) return 0;
    int n = 0;
    for (const auto& [a, fund] : rows) {
        // Опубликованное фондом не трогаем; остальное — как сказал владелец.
        Stmt q(db, "INSERT INTO btc_etf_addr(address, fund, how, at, hist) VALUES(?,?,'list',?,0) "
                   "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, how='list', at=excluded.at, "
                   "bal_at=0, hist=0 WHERE btc_etf_addr.how != 'seed'");
        q.text(1, a).text(2, fund).num(3, now()).run();
        if (sqlite3_changes(db) == 0) continue;
        ++n;
        std::lock_guard<std::mutex> l(g_mx);
        g_fund[a] = fund;
        g_types[fund].insert(addrType(a));
    }
    sqlite3_exec(db, "DELETE FROM btc_etf_import", nullptr, nullptr, nullptr);
    std::cout << "[ETF] разметка владельца: " << n << " кошельков" << std::endl;
    return n;
}

std::vector<std::string> etfHistQueue(sqlite3* db, size_t limit) {
    std::vector<std::string> r;
    Stmt q(db, "SELECT address FROM btc_etf_addr WHERE hist = 0 ORDER BY at LIMIT ?");
    q.num(1, static_cast<long long>(limit));
    while (q.row()) r.push_back(q.str(0));
    return r;
}

void etfHistDone(sqlite3* db, const std::vector<std::string>& addrs) {
    Stmt q(db, "UPDATE btc_etf_addr SET hist = 1 WHERE address = ?");
    for (const auto& a : addrs) {
        sqlite3_reset(q.s);
        q.text(1, a).run();
    }
}

void etfRollback(sqlite3* db, long long height) {
    struct Row { long long ts, sats; int kind; std::string fund; };
    std::vector<Row> rows;
    {
        Stmt q(db, "SELECT ts, sats, kind, fund FROM btc_etf_moves WHERE height=? AND kind IN (1, 2, 3)");
        q.num(1, height);
        while (q.row()) rows.push_back({q.col(0), q.col(1), static_cast<int>(q.col(2)), q.str(3)});
    }
    for (const auto& r : rows) addFlow(db, r.ts, r.fund, r.kind, r.sats, -1);
    Stmt d(db, "DELETE FROM btc_etf_moves WHERE height=?");
    d.num(1, height).run();
    // Остатки отката не знают — все кошельки на сверку.
    Stmt u(db, "UPDATE btc_etf_addr SET bal_at=0");
    u.run();
}

void etfCleanup(sqlite3* db, long long keepFrom) {
    Stmt m(db, "DELETE FROM btc_etf_moves WHERE ts < ?");
    m.num(1, keepFrom).run();
    Stmt f(db, "DELETE FROM btc_etf_flow WHERE day < ?");
    f.num(1, keepFrom).run();
    Stmt h(db, "DELETE FROM btc_etf_hold WHERE day < ?");
    h.num(1, keepFrom).run();
    Stmt x(db, "DELETE FROM btc_cust_xfer WHERE ts < ?");
    x.num(1, keepFrom).run();
    std::lock_guard<std::mutex> l(g_mx);
    const long long cut = now() - 86400;
    g_checks.erase(std::remove_if(g_checks.begin(), g_checks.end(), [cut](const Check& c) { return c.at < cut; }),
                   g_checks.end());
}

std::vector<std::string> etfStale(sqlite3* db, size_t limit) {
    std::vector<std::string> r;
    Stmt q(db, "SELECT address FROM btc_etf_addr WHERE bal_at < ? ORDER BY bal_at LIMIT ?");
    q.num(1, now() - CHECK_SEC).num(2, static_cast<long long>(limit));
    while (q.row()) r.push_back(q.str(0));
    return r;
}

void etfSetBalance(sqlite3* db, const std::string& address, long long sats, long long height) {
    std::string fund;
    long long was = 0, wasAt = 0;
    {
        Stmt q(db, "SELECT fund, bal_sats, bal_at FROM btc_etf_addr WHERE address=?");
        q.text(1, address);
        if (!q.row()) return;
        fund = q.str(0);
        was = q.col(1);
        wasAt = q.col(2);
    }
    Stmt u(db, "UPDATE btc_etf_addr SET bal_sats=?, bal_height=?, bal_at=? WHERE address=?");
    u.num(1, sats).num(2, height).num(3, now()).text(4, address).run();
    if (wasAt > 0) {
        std::lock_guard<std::mutex> l(g_mx);
        g_checks.push_back({now(), fund, sats - was});
    }
    // Запас на сегодня — после каждой сверки: последняя за сутки и остаётся.
    Stmt h(db, "INSERT OR REPLACE INTO btc_etf_hold(day, fund, sats, n) "
               "SELECT ?, fund, COALESCE(SUM(bal_sats), 0), COUNT(*) FROM btc_etf_addr WHERE fund=?");
    h.num(1, dayOf(now())).text(2, fund).run();
}

std::string etfStats(sqlite3* db) {
    std::string out = "\n\n🏦 <b>ETF по цепочке</b> (свои метки)";
    std::vector<std::string> funds;
    {
        Stmt q(db, "SELECT DISTINCT fund FROM btc_etf_addr ORDER BY fund");
        while (q.row()) funds.push_back(q.str(0));
    }
    if (funds.empty()) return out + "\nКошельков фондов нет";
    const long long today = dayOf(now());
    for (const auto& f : funds) {
        long long seed = 0, learned = 0, unchecked = 0;
        {
            Stmt q(db, "SELECT SUM(how='seed'), SUM(how!='seed'), SUM(bal_at=0) FROM btc_etf_addr WHERE fund=?");
            q.text(1, f);
            if (q.row()) { seed = q.col(0); learned = q.col(1); unchecked = q.col(2); }
        }
        long long in = 0, outS = 0, fee = 0;
        {
            Stmt q(db, "SELECT in_sats, out_sats, fee_sats FROM btc_etf_flow WHERE day=? AND fund=?");
            q.num(1, today).text(2, f);
            if (q.row()) { in = q.col(0); outS = q.col(1); fee = q.col(2); }
        }
        long long week = 0;
        {
            Stmt q(db, "SELECT COALESCE(SUM(in_sats - out_sats), 0) FROM btc_etf_flow WHERE day>=? AND fund=?");
            q.num(1, today - 6 * 86400).text(2, f);
            if (q.row()) week = q.col(0);
        }
        long long drift = 0, checks = 0;
        {
            std::lock_guard<std::mutex> l(g_mx);
            for (const auto& c : g_checks)
                if (c.fund == f) { drift += c.drift; ++checks; }
        }
        out += "\n· <b>" + f + "</b>: кошельков " + std::to_string(seed + learned) + " (опубликовано " +
               std::to_string(seed) + ", нашёл сам " + std::to_string(learned) + ")" +
               "\n  запас " + btc(holdings(db, f)) + " BTC" +
               (unchecked ? " · ещё не сверено: " + std::to_string(unchecked) : std::string()) +
               "\n  сегодня +" + btc(in) + " / −" + btc(outS) + (fee ? " · плата фонду " + btc(fee) : std::string()) +
               " · за 7 дней " + (week >= 0 ? "+" : "−") + btc(week >= 0 ? week : -week) + " BTC" +
               "\n  сверка с цепочкой за сутки: " + std::to_string(checks) + " кошельков, расхождение " +
               btc(drift) + " BTC";
    }
    if (g_mixed) out += "\n⚠️ Траты со смешанными фондами: " + std::to_string(g_mixed);
    {
        long long vaults = 0, unread = 0, xday = 0, xall = 0;
        Stmt q(db, "SELECT COUNT(*), SUM(read = 0) FROM btc_cust_vault");
        if (q.row()) { vaults = q.col(0); unread = q.col(1); }
        Stmt x(db, "SELECT COUNT(*), SUM(ts >= ?) FROM btc_cust_xfer");
        x.num(1, now() - 86400);
        if (x.row()) { xall = x.col(0); xday = x.col(1); }
        size_t fees;
        {
            std::lock_guard<std::mutex> l(g_mx);
            fees = g_feeWallet.size();
        }
        out += "\n🗄 Хранилища хранителя: " + std::to_string(vaults) +
               (unread ? " (история дочитывается: " + std::to_string(unread) + ")" : std::string()) +
               " · кошельков комиссий: " + std::to_string(fees) +
               "\n  переводов с хранилищами: " + std::to_string(xday) + " за сутки, " + std::to_string(xall) +
               " всего — разбор по фондам в API";
    }
    return out;
}
