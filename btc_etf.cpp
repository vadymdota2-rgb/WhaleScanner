#include "btc_etf.h"

#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <unordered_map>

#include "btc_etf_book.h"
#include "btc_etf_list.h"

namespace {

std::mutex g_mx;
std::unordered_map<std::string, std::string> g_fund;  // адрес → фонд

long long now() { return static_cast<long long>(std::time(nullptr)); }

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
            std::cerr << "[ETF] prepare: " << sqlite3_errmsg(db) << " | " << sql << std::endl;
            s = nullptr;
        }
    }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt& text(int i, const std::string& v) {
        if (s) sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& num(int i, long long v) {
        if (s) sqlite3_bind_int64(s, i, v);
        return *this;
    }
    void reset() { if (s) sqlite3_reset(s); }
    bool row() { return s && sqlite3_step(s) == SQLITE_ROW; }
    bool run() { return s && sqlite3_step(s) == SQLITE_DONE; }
    long long col(int i) { return sqlite3_column_int64(s, i); }
    std::string str(int i) {
        const unsigned char* t = sqlite3_column_text(s, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
};

void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[ETF] " << (err ? err : "?") << std::endl;
        sqlite3_free(err);
    }
}

}  // namespace

void etfOpen(sqlite3* db) {
    // how: seed — опубликован фондом (btc_etf_book.h), import — разметка
    // владельца через /import. Колонки остатков у старой базы остаются — их
    // никто больше не пишет.
    exec(db,
         "CREATE TABLE IF NOT EXISTS btc_etf_addr ("
         "  address TEXT PRIMARY KEY, fund TEXT NOT NULL, how TEXT NOT NULL, at INTEGER NOT NULL DEFAULT 0);"
         "CREATE INDEX IF NOT EXISTS idx_btc_etf_addr_fund ON btc_etf_addr(fund);"
         "CREATE TABLE IF NOT EXISTS btc_etf_import ("
         "  address TEXT PRIMARY KEY, fund TEXT NOT NULL, at INTEGER NOT NULL DEFAULT 0);");
    exec(db, "BEGIN");
    // Слежка за фондами по цепочке (15–18 октября) снята: потоки фондов, их
    // остатки, хранилища хранителя и сверка с «Институциональными потоками»
    // шли в цикле блоков и останавливали его. Её таблицы — вон.
    exec(db,
         "DROP TABLE IF EXISTS btc_etf_moves; DROP TABLE IF EXISTS btc_etf_flow;"
         "DROP TABLE IF EXISTS btc_etf_svc; DROP TABLE IF EXISTS btc_etf_hold;"
         "DROP TABLE IF EXISTS btc_etf_check; DROP TABLE IF EXISTS btc_etf_learned;"
         "DROP TABLE IF EXISTS btc_cust_vault; DROP TABLE IF EXISTS btc_cust_xfer;");
    // Метки — только опубликованные фондом и разметка владельца. Найденные
    // сканером сами (совместная трата, переезд запаса, корзины хранителя) —
    // вон. Кошельки из чужих списков (btc_etf_list.h) тоже: у них how='list',
    // как и у разметки владельца до этой версии, — поэтому снимаем ровно
    // адреса списка, а остальные 'list' — это /import, они становятся import.
    // После первого запуска строк 'list' нет, и это ничего не делает.
    exec(db, "DELETE FROM btc_etf_addr WHERE how NOT IN ('seed', 'list', 'import')");
    {
        Stmt q(db, "DELETE FROM btc_etf_addr WHERE address=? AND how='list'");
        for (const auto& [a, fund] : BTC_ETF_LIST) {
            (void)fund;
            q.reset();
            q.text(1, a).run();
        }
    }
    exec(db, "UPDATE btc_etf_addr SET how='import' WHERE how='list'");
    {
        Stmt q(db, "INSERT INTO btc_etf_addr(address, fund, how, at) VALUES(?,?,'seed',?) "
                   "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, how='seed'");
        for (const auto& [a, fund] : BTC_ETF_BOOK) {
            q.reset();
            q.text(1, a).text(2, fund).num(3, now()).run();
        }
    }
    exec(db, "COMMIT");
    std::unordered_map<std::string, std::string> all;
    {
        Stmt q(db, "SELECT address, fund FROM btc_etf_addr");
        while (q.row()) all.emplace(q.str(0), q.str(1));
    }
    std::cout << "[ETF] меток кошельков фондов: " << all.size() << std::endl;
    std::lock_guard<std::mutex> l(g_mx);
    g_fund = std::move(all);
}

std::string etfFund(const std::string& address) {
    if (address.empty()) return "";
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_fund.find(address);
    return it == g_fund.end() ? "" : it->second;
}

EtfImportResult etfImport(sqlite3* db, const std::vector<std::string>& addrs, const std::string& fund) {
    EtfImportResult r;
    Stmt was(db, "SELECT fund, how FROM btc_etf_addr WHERE address=?");
    Stmt base(db, "SELECT 1 FROM btc_watch WHERE address=?");
    Stmt q(db, "INSERT INTO btc_etf_import(address, fund, at) VALUES(?,?,?) "
               "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, at=excluded.at");
    for (const auto& a : addrs) {
        was.reset();
        was.text(1, a);
        if (was.row()) {
            if (was.str(1) == "seed") { ++r.seed; continue; }
            if (was.str(0) == fund) { ++r.dup; continue; }
            ++r.moved;
        } else {
            base.reset();
            base.text(1, a);
            if (base.row()) ++r.inBase;
        }
        q.reset();
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
    Stmt q(db, "INSERT INTO btc_etf_addr(address, fund, how, at) VALUES(?,?,'import',?) "
               "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, how='import', at=excluded.at "
               "WHERE btc_etf_addr.how != 'seed'");
    for (const auto& [a, fund] : rows) {
        // Опубликованное фондом не трогаем; остальное — как сказал владелец.
        q.reset();
        q.text(1, a).text(2, fund).num(3, now()).run();
        if (sqlite3_changes(db) == 0) continue;
        ++n;
        std::lock_guard<std::mutex> l(g_mx);
        g_fund[a] = fund;
    }
    exec(db, "DELETE FROM btc_etf_import");
    std::cout << "[ETF] разметка владельца: " << n << " кошельков" << std::endl;
    return n;
}

std::string etfStats(sqlite3* db) {
    std::string out = "\n\n🏷 <b>Метки фондов ETF</b> (список фонда и /import)";
    std::map<std::string, std::pair<long long, long long>> by;  // фонд → (опубликовано, /import)
    {
        Stmt q(db, "SELECT fund, SUM(how='seed'), SUM(how!='seed') FROM btc_etf_addr GROUP BY fund ORDER BY fund");
        while (q.row()) by[q.str(0)] = {q.col(1), q.col(2)};
    }
    if (by.empty()) return out + "\nМеток нет";
    for (const auto& [fund, n] : by)
        out += "\n· <b>" + fund + "</b>: " + std::to_string(n.first + n.second) + " (опубликовано " +
               std::to_string(n.first) + ", /import " + std::to_string(n.second) + ")";
    long long queued = 0;
    {
        Stmt q(db, "SELECT COUNT(*) FROM btc_etf_import");
        if (q.row()) queued = q.col(0);
    }
    if (queued) out += "\nВ очереди на разметку: " + std::to_string(queued) + " (с ближайшим блоком)";
    return out;
}
