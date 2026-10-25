#include "btc_etf.h"

#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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

bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[ETF] " << (err ? err : "?") << std::endl;
        sqlite3_free(err);
        return false;
    }
    return true;
}

// Тикер фонда: заглавные и цифры, до восьми знаков. Список кнопок /import
// (IMPORT_FUNDS в main.cpp) — такие; всё прочее сюда не пишется.
bool validFund(const std::string& f) {
    if (f.empty() || f.size() > 8) return false;
    for (char ch : f)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) return false;
    return true;
}

std::string esc(const std::string& v) {
    std::string r;
    for (char ch : v) {
        if (ch == '&') r += "&amp;";
        else if (ch == '<') r += "&lt;";
        else if (ch == '>') r += "&gt;";
        else r += ch;
    }
    return r;
}

// Миграция и список фонда. Зовётся внутри транзакции; false — что-то не
// записалось, транзакцию откатить.
bool refreshLabels(sqlite3* db) {
    // Слежка за фондами по цепочке (15–18 октября) снята: потоки фондов, их
    // остатки, хранилища хранителя и сверка с «Институциональными потоками»
    // шли в цикле блоков и останавливали его. Её таблицы — вон.
    bool ok = exec(db,
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
    ok = exec(db, "DELETE FROM btc_etf_addr WHERE how NOT IN ('seed', 'list', 'import')") && ok;
    {
        Stmt q(db, "DELETE FROM btc_etf_addr WHERE address=? AND how='list'");
        ok = q.s && ok;
        for (const auto& [a, fund] : BTC_ETF_LIST) {
            (void)fund;
            q.reset();
            ok = q.text(1, a).run() && ok;
        }
    }
    ok = exec(db, "UPDATE btc_etf_addr SET how='import' WHERE how='list'") && ok;
    // Список фонда — как он есть сейчас. Запись меняется, только если в ней
    // что-то другое: раньше каждый запуск переписывал все строки впустую.
    {
        Stmt q(db, "INSERT INTO btc_etf_addr(address, fund, how, at) VALUES(?,?,'seed',?) "
                   "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, how='seed' "
                   "WHERE btc_etf_addr.fund != excluded.fund OR btc_etf_addr.how != 'seed'");
        ok = q.s && ok;
        for (const auto& [a, fund] : BTC_ETF_BOOK) {
            q.reset();
            ok = q.text(1, a).text(2, fund).num(3, now()).run() && ok;
        }
    }
    // Адрес, который фонд убрал со своей страницы, — больше не его метка.
    // Раньше такие оставались фондом навсегда. Пустой список (сборка
    // tools/btc_etf_seeds.py сломалась) не стирает ничего.
    if (!BTC_ETF_BOOK.empty()) {
        std::unordered_set<std::string> book;
        for (const auto& [a, fund] : BTC_ETF_BOOK) {
            (void)fund;
            book.insert(a);
        }
        std::vector<std::string> gone;
        {
            Stmt q(db, "SELECT address FROM btc_etf_addr WHERE how='seed'");
            ok = q.s && ok;
            while (q.row()) if (!book.count(q.str(0))) gone.push_back(q.str(0));
        }
        Stmt d(db, "DELETE FROM btc_etf_addr WHERE address=? AND how='seed'");
        ok = d.s && ok;
        for (const auto& a : gone) {
            d.reset();
            ok = d.text(1, a).run() && ok;
        }
        if (!gone.empty()) std::cout << "[ETF] сняты метки, которых нет в списке фонда: " << gone.size() << std::endl;
    }
    // Кошелёк фонда — не биржа: выученную сканером метку биржи (совместная
    // трата, сбор, перекладка) с него снимаем. Проверенные вручную — нет.
    ok = exec(db, "DELETE FROM btc_labels WHERE how != 'seed' "
                  "AND address IN (SELECT address FROM btc_etf_addr)") && ok;
    return ok;
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
    // Всё ниже — одной транзакцией, всё или ничего. IMMEDIATE — блокировка
    // записи сразу: обычный BEGIN, получив её только на первой записи, ловил
    // «database is locked» от чужой записи посреди транзакции, и её
    // незавершённый запрос не давал закрыть транзакцию (так 23 октября встала
    // запись блоков). Каждый шаг проверяется; сбой — откат, метки — как были.
    if (!exec(db, "BEGIN IMMEDIATE")) {
        std::cerr << "[ETF] метки не обновлены: база занята" << std::endl;
    } else if (!refreshLabels(db) || !exec(db, "COMMIT")) {
        // Сбой любого шага или COMMIT — откат: открытая транзакция остановила
        // бы запись блоков (как 23 октября).
        std::cerr << "[ETF] метки фондов не обновлены — откат" << std::endl;
        if (!sqlite3_get_autocommit(db)) exec(db, "ROLLBACK");
    }
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
    if (!validFund(fund)) {
        std::cerr << "[ETF] /import: не тикер фонда: " << fund.substr(0, 16) << std::endl;
        r.failed = true;
        return r;
    }
    Stmt was(db, "SELECT fund, how FROM btc_etf_addr WHERE address=?");
    Stmt base(db, "SELECT 1 FROM btc_watch WHERE address=?");
    Stmt q(db, "INSERT INTO btc_etf_import(address, fund, at) VALUES(?,?,?) "
               "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, at=excluded.at");
    if (!was.s || !base.s || !q.s) {
        r.failed = true;
        return r;
    }
    // Один адрес дважды в списке — одна разметка, а не две в отчёте.
    std::unordered_set<std::string> seen;
    for (const auto& a : addrs) {
        if (a.empty() || !seen.insert(a).second) continue;
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
        // Не записалось — весь импорт откатят (вызывающий видит failed), и
        // отчёт не скажет «в разметку», когда в базе ничего нет.
        if (!q.text(1, a).text(2, fund).num(3, now()).run()) {
            std::cerr << "[ETF] /import " << a << ": " << sqlite3_errmsg(db) << std::endl;
            q.reset();
            r.failed = true;
            return r;
        }
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
    // Карта в памяти меняется только после записи всей очереди: не
    // записалось — блок откатят, очередь останется и придёт со следующим.
    std::vector<std::pair<std::string, std::string>> done;
    Stmt q(db, "INSERT INTO btc_etf_addr(address, fund, how, at) VALUES(?,?,'import',?) "
               "ON CONFLICT(address) DO UPDATE SET fund=excluded.fund, how='import', at=excluded.at "
               "WHERE btc_etf_addr.how != 'seed'");
    // Снимаются из очереди ровно перенесённые строки — не «всё, что есть»:
    // строку, поставленную в очередь после чтения, не теряем.
    Stmt del(db, "DELETE FROM btc_etf_import WHERE address=? AND fund=?");
    // Кошелёк фонда — не биржа: выученную метку биржи с него снимаем.
    Stmt unlab(db, "DELETE FROM btc_labels WHERE address=? AND how != 'seed'");
    if (!q.s || !del.s || !unlab.s) return -1;
    for (const auto& [a, fund] : rows) {
        // Опубликованное фондом не трогаем; остальное — как сказал владелец.
        q.reset();
        if (!q.text(1, a).text(2, fund).num(3, now()).run()) {
            std::cerr << "[ETF] разметка " << a << ": " << sqlite3_errmsg(db) << std::endl;
            q.reset();
            return -1;
        }
        if (sqlite3_changes(db) > 0) {
            done.emplace_back(a, fund);
            unlab.reset();
            if (!unlab.text(1, a).run()) {
                unlab.reset();
                return -1;
            }
        }
        del.reset();
        if (!del.text(1, a).text(2, fund).run()) {
            del.reset();
            return -1;
        }
    }
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (const auto& [a, fund] : done) g_fund[a] = fund;
    }
    std::cout << "[ETF] разметка владельца: " << done.size() << " кошельков" << std::endl;
    return static_cast<int>(done.size());
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
        out += "\n· <b>" + esc(fund) + "</b>: " + std::to_string(n.first + n.second) + " (опубликовано " +
               std::to_string(n.first) + ", /import " + std::to_string(n.second) + ")";
    long long queued = 0;
    {
        Stmt q(db, "SELECT COUNT(*) FROM btc_etf_import");
        if (q.row()) queued = q.col(0);
    }
    if (queued) out += "\nВ очереди на разметку: " + std::to_string(queued) + " (с ближайшим блоком)";
    return out;
}
