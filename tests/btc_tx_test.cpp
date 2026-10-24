// Транзакции базы биткоина на настоящей SQLite: сбой записи посреди блока,
// чужой писатель посреди транзакции (так 23 октября встала запись блоков),
// кривой вклад блока при откате цепочки. Запуск: tests/run.sh
#include "../btc_chain.cpp"
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
    stopBtc();
    for (const char* suf : {"", "-wal", "-shm"}) std::remove((db + suf).c_str());
    rmdir(dir);
    std::cout << (fails ? "FAILED " : "OK ") << fails << std::endl;
    return fails ? 1 : 0;
}
