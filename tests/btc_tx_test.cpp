// Транзакции базы биткоина на настоящей SQLite: сбой записи посреди блока,
// чужой писатель посреди транзакции (так 23 октября встала запись блоков),
// кривой вклад блока при откате цепочки. Запуск: tests/run.sh
#include "../btc_chain.cpp"
#include "../btc_etf_book.h"
std::atomic<bool> running{true};
void autoSeed(AutoNet, int) {}
bool autoRoom(AutoNet) { return false; }
void autoCounted(AutoNet) {}
void autoBanned(AutoNet, int) {}
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL " #c " line " << __LINE__ << std::endl; ++fails; } } while (0)
static long long count(const char* sql) {
    sqlite3_stmt* s = nullptr; long long n = -1;
    if (sqlite3_prepare_v2(g_btcDb, sql, -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s); return n;
}
int main() {
    char dir[] = "/tmp/btc-tx-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string db = std::string(dir) + "/t.db";
    setenv("WHALE_BTC_DB_FILE", db.c_str(), 1);
    CHECK(openDb());
    // 1. Ошибка записи посреди транзакции — откат всего, false, база свободна.
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        bool ok = inTx([] {
            execSql("INSERT INTO btc_state(k, v) VALUES('a', '1')");
            sqlite3_stmt* s = nullptr;
            prep(&s, "INSERT INTO btc_labels(address, ex, how) VALUES(?, NULL, 'x')");  // NOT NULL
            bindText(s, 1, "x");
            stepW(s);
            sqlite3_finalize(s);
        });
        CHECK(!ok);
        CHECK(sqlite3_get_autocommit(g_btcDb));
        CHECK(count("SELECT COUNT(*) FROM btc_state WHERE k='a'") == 0);
        // 2. Следующая транзакция проходит.
        ok = inTx([] { execSql("INSERT INTO btc_state(k, v) VALUES('b', '1')"); });
        CHECK(ok);
        CHECK(count("SELECT COUNT(*) FROM btc_state WHERE k='b'") == 1);
    }
    // 3. Чужой писатель посреди нашей транзакции (как whale_api 23.10) —
    //    ждёт, а не ломает её; COMMIT проходит.
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        sqlite3* other = nullptr;
        sqlite3_open(db.c_str(), &other);
        sqlite3_busy_timeout(other, 200);
        int otherRc = -1;
        bool ok = inTx([&] {
            sqlite3_stmt* r = nullptr;
            prep(&r, "SELECT COUNT(*) FROM btc_labels");
            sqlite3_step(r);
            otherRc = sqlite3_exec(other, "INSERT INTO btc_case(lower, addr) VALUES('q', 'Q')", nullptr, nullptr, nullptr);
            sqlite3_stmt* w = nullptr;
            prep(&w, "INSERT INTO btc_state(k, v) VALUES('c', '1')");
            stepW(w);
            sqlite3_finalize(w);
            sqlite3_finalize(r);
        });
        CHECK(otherRc == SQLITE_BUSY);
        CHECK(ok);
        CHECK(count("SELECT COUNT(*) FROM btc_state WHERE k='c'") == 1);
        sqlite3_close(other);
    }
    // 4. Кривой вклад блока в потоки при откате цепочки не бросает исключение.
    {
        Scanner sc;
        { std::lock_guard<std::mutex> l(g_btcDbMutex); CHECK(sc.init()); }
        {
            std::lock_guard<std::mutex> l(g_btcDbMutex);
            execSql("INSERT INTO btc_blocks(height, hash, ts, flow) VALUES(99, 'old', 1000, '{\"Binance\":[\"x\",1,2,3]}')");
        }
        Block b; b.height = 100; b.prev = std::string(64, 'a');
        bool threw = false;
        try { (void)sc.process(b); } catch (...) { threw = true; }
        CHECK(!threw);
        CHECK(count("SELECT COUNT(*) FROM btc_blocks WHERE height=99") == 0);
    }
    // ── Метки фондов ETF (btc_etf.cpp) ──
    auto exec1 = [](const char* sql) {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        CHECK(sqlite3_exec(g_btcDb, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    const std::string book0 = BTC_ETF_BOOK[0].first, book1 = BTC_ETF_BOOK[1].first;
    // 5. Список фонда — метки при запуске.
    CHECK(etfFund(book0) == "BITB");
    // 6. Адрес, которого больше нет в списке фонда, снимается; выученная
    //    метка биржи с кошелька фонда — тоже; проверенная вручную — нет.
    exec1("INSERT INTO btc_etf_addr(address, fund, how) VALUES('bc1qstale', 'BITB', 'seed')");
    {
        const std::string sql = "INSERT INTO btc_labels(address, ex, how) VALUES('" + book0 + "', 'Coinbase', 'cospend'),"
                                "('" + book1 + "', 'Coinbase', 'seed')";
        exec1(sql.c_str());
    }
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        etfOpen(g_btcDb);
        CHECK(sqlite3_get_autocommit(g_btcDb));
    }
    CHECK(etfFund("bc1qstale").empty());
    CHECK(count("SELECT COUNT(*) FROM btc_etf_addr WHERE address='bc1qstale'") == 0);
    CHECK(count(("SELECT COUNT(*) FROM btc_labels WHERE address='" + book0 + "'").c_str()) == 0);
    CHECK(count(("SELECT COUNT(*) FROM btc_labels WHERE address='" + book1 + "'").c_str()) == 1);
    CHECK(count("SELECT COUNT(*) FROM btc_etf_addr WHERE how='seed'") == static_cast<long long>(BTC_ETF_BOOK.size()));
    // 7. /import: повтор адреса — одна разметка; опубликованное фондом — не
    //    тронуто; кошелёк из базы китов — помечен.
    exec1("INSERT INTO btc_watch(address, src) VALUES('bc1qwhale', 'import')");
    {
        EtfImportResult r = btcEtfImport({"bc1qnew", "bc1qnew", book0, "bc1qwhale"}, "IBIT");
        CHECK(!r.failed);
        CHECK(r.queued == 2 && r.seed == 1 && r.inBase == 1 && r.dup == 0);
        CHECK(count("SELECT COUNT(*) FROM btc_etf_import") == 2);
    }
    // 8. Не тикер фонда — сбой, очередь не тронута.
    {
        EtfImportResult r = btcEtfImport({"bc1qother"}, "<b>x");
        CHECK(r.failed);
        CHECK(count("SELECT COUNT(*) FROM btc_etf_import") == 2);
    }
    // 9. Сбой записи посреди списка — откат всего списка, в отчёте сбой.
    exec1("CREATE TRIGGER boom BEFORE INSERT ON btc_etf_import WHEN NEW.address = 'bc1qboom' "
          "BEGIN SELECT RAISE(ABORT, 'boom'); END");
    {
        EtfImportResult r = btcEtfImport({"bc1qfirst", "bc1qboom"}, "FBTC");
        CHECK(r.failed);
        CHECK(count("SELECT COUNT(*) FROM btc_etf_import WHERE address='bc1qfirst'") == 0);
        CHECK(sqlite3_get_autocommit(g_btcDb));
    }
    exec1("DROP TRIGGER boom");
    // 10. Разметка с блоком: метки встают, выученная метка биржи снимается,
    //     опубликованное фондом не переписывается, очередь — пустая.
    exec1("INSERT INTO btc_labels(address, ex, how) VALUES('bc1qnew', 'Kraken', 'sweep')");
    exec1(("INSERT INTO btc_etf_import(address, fund) VALUES('" + book1 + "', 'IBIT')").c_str());
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        int n = -2;
        CHECK(inTx([&] { n = etfDrainLabels(g_btcDb); }));
        CHECK(n == 2);
    }
    CHECK(etfFund("bc1qnew") == "IBIT" && etfFund("bc1qwhale") == "IBIT");
    CHECK(etfFund(book1) == "BITB");
    CHECK(count("SELECT COUNT(*) FROM btc_labels WHERE address='bc1qnew'") == 0);
    CHECK(count("SELECT COUNT(*) FROM btc_etf_import") == 0);
    // 11. Сбой записи разметки — -1, карта в памяти не меняется.
    exec1("INSERT INTO btc_etf_import(address, fund) VALUES('bc1qlate', 'ARKB')");
    exec1("CREATE TRIGGER boom2 BEFORE INSERT ON btc_etf_addr BEGIN SELECT RAISE(ABORT, 'boom'); END");
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        int n = 0;
        CHECK(!inTx([&] {
            n = etfDrainLabels(g_btcDb);
            if (n < 0) ++g_txErrs;
        }));
        CHECK(n == -1);
    }
    CHECK(etfFund("bc1qlate").empty());
    CHECK(count("SELECT COUNT(*) FROM btc_etf_import") == 1);
    exec1("DROP TRIGGER boom2");
    // 12. /statsbtc: имя фонда — экранировано.
    exec1("INSERT INTO btc_etf_addr(address, fund, how) VALUES('bc1qodd', '<i>', 'import')");
    {
        std::lock_guard<std::mutex> l(g_btcDbMutex);
        const std::string st = etfStats(g_btcDb);
        CHECK(st.find("&lt;i&gt;") != std::string::npos && st.find("<i>") == std::string::npos);
        CHECK(st.find("В очереди на разметку: 1") != std::string::npos);
    }
    stopBtc();
    for (const char* suf : {"", "-wal", "-shm"}) std::remove((db + suf).c_str());
    rmdir(dir);
    std::cout << (fails ? "FAILED " : "OK ") << fails << std::endl;
    return fails ? 1 : 0;
}
