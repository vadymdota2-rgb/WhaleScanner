// Биржевые фонды в сети Bitcoin. Что и почему — в btc_etf.h.
#include "btc_etf.h"
#include "btc_etf_book.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace {

constexpr long long SAT = 100000000LL;
// Пыль — меньше 0,01 BTC: так хранитель платит комиссию сети за трату
// кошелька (присылает пылинку и забирает остаток). Это не деньги фонда.
constexpr long long DUST = SAT / 100;
// Сверка остатка кошелька с цепочкой — раз в сутки.
constexpr long long CHECK_SEC = 86400;

// Плата за управление, базисные пункты в год (проспект фонда). Нужна, чтобы
// отличить её от оттока инвесторов.
const std::map<std::string, int> FEE_BPS = {{"BITB", 20}};

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

void scanTx(sqlite3* db, const Block& b, const BTx& tx) {
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
        "CREATE TABLE IF NOT EXISTS btc_etf_hold ("
        "  day INTEGER NOT NULL, fund TEXT NOT NULL, sats INTEGER NOT NULL, n INTEGER NOT NULL,"
        "  PRIMARY KEY (day, fund));";
    if (sqlite3_exec(db, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[ETF] схема: " << (err ? err : "?") << std::endl;
        sqlite3_free(err);
    }
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    for (const auto& [a, fund] : BTC_ETF_BOOK) {
        Stmt q(db, "INSERT INTO btc_etf_addr(address, fund, how, at) VALUES(?,?,'seed',?) "
                   "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, how='seed'");
        q.text(1, a).text(2, fund).num(3, now()).run();
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    std::lock_guard<std::mutex> l(g_mx);
    g_fund.clear();
    g_types.clear();
    g_service.clear();
    for (const auto& [a, fund] : BTC_ETF_BOOK) g_types[fund].insert(addrType(a));
    Stmt q(db, "SELECT address, fund FROM btc_etf_addr");
    while (q.row()) g_fund[q.str(0)] = q.str(1);
    Stmt v(db, "SELECT address FROM btc_etf_svc");
    while (v.row()) g_service.insert(v.str(0));
    std::cout << "[ETF] кошельков фондов: " << g_fund.size() << std::endl;
}

void etfBlock(sqlite3* db, const Block& b) {
    for (const auto& tx : b.txs) scanTx(db, b, tx);
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
    return out;
}
