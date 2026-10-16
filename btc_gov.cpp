// Биткоины государств: см. btc_gov.h.
#include "btc_gov.h"

#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace {

// Адреса, раскрытые государствами (источник — в btc_gov.h). Список
// Сальвадора здесь — на день написания; дальше бот берёт его с bitcoin.gob.sv.
struct Seed { const char* addr; const char* gov; };
constexpr Seed SEEDS[] = {
    {"bc1qa5wkgaew2dkv56kfvj49j0av5nml45x9ek9hz6", "US"},
    {"32ixEdVJWo3kmvJGMTZq5jAQVZZeuwnqzo", "SV"},
    {"3Fh6QjPzfu2LWYxVYqRZ45QPTja1PrFmNC", "SV"},
    {"38BSq1sUaGFxDVdqhCraRMg1Kgh4wpTbfG", "SV"},
    {"3KzeoYiSFaRxPktmdoehuXRNmE1hZQM2Cp", "SV"},
    {"3DrHiHzfAKnne3zn8pqJdxJQhRFtW1jnMG", "SV"},
    {"33heTZ5DNKVeCDrknBvZnk95f7rzVYC6X4", "SV"},
    {"38czTLv6ufGFjqLgiRdaQpDduErqszgfDX", "SV"},
    {"32N6ugHCwxQ7esqSY6dwmanDx2onLHCdyp", "SV"},
    {"3McfTktodPWHwigfg7e9pQti4wXYKP47LW", "SV"},
    {"37jS98twXbuDHMVDvZCEaAyqBngD2XmKTr", "SV"},
    {"3544w8kqR1SFKwysmidDz4LHrLJv58rP4o", "SV"},
    {"39bEbJWGAYjpRvoJfDUEY4cQaFtJgDgdvH", "SV"},
    {"38PKBmzVrmuiGnSMzSsLGXUXP61LCf1t9T", "SV"},
    {"3Ky5QojgyfdfaQ7hoPQfio4BCLtavsPqsB", "SV"},
    {"3QhNpAuMJjN6WJNWmHuZQHyBcTxrrWpS5g", "SV"},
    {"3ExoWb4ZwU5F8rRHGfQdeqLtK2AgP4jjwt", "SV"},
    {"3QUddmvT36K4edS9AsKGHydygM8MavXSbc", "SV"},
    {"3ARCzX8kjALXjfnAhEbyfqXLiRUpNJJfa3", "SV"},
    {"3KXKHH5hhiYkx9nfJdUxZL3ivrVSnQNfKz", "SV"},
    {"359jBt2XuTdgk6YEz1WwhwMwrzTd7Qb3Gw", "SV"},
};
// Остаток адреса без движений сверяется раз в сутки.
constexpr long long RECHECK_SEC = 86400;
// Совместная трата учит не больше стольких адресов за транзакцию: больше —
// уже не похоже на кошелёк государства (пакетная выплата сервиса).
constexpr size_t LEARN_MAX = 20;

std::mutex g_mx;
std::unordered_map<std::string, std::string> g_gov;

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
            std::cerr << "[GOV] prepare: " << sqlite3_errmsg(db) << " | " << sql << std::endl;
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
    bool row() { return s && sqlite3_step(s) == SQLITE_ROW; }
    bool run() { return s && sqlite3_step(s) == SQLITE_DONE; }
    long long col(int i) { return sqlite3_column_int64(s, i); }
    std::string str(int i) {
        const unsigned char* t = sqlite3_column_text(s, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
};

long long now() { return static_cast<long long>(std::time(nullptr)); }

std::string govOf(const std::string& a) {
    if (a.empty()) return "";
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_gov.find(a);
    return it == g_gov.end() ? "" : it->second;
}

// Новый адрес — в память и в базу, остаток неизвестен (checked_at = 0 —
// первым на сверку). Уже известный — без изменений.
bool add(sqlite3* db, const std::string& a, const std::string& gov, const char* how) {
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_gov.emplace(a, gov).second) return false;
    }
    Stmt q(db, "INSERT OR IGNORE INTO btc_gov_addr(address, gov, how, at) VALUES(?,?,?,?)");
    q.text(1, a).text(2, gov).text(3, how).num(4, now()).run();
    return true;
}

void recheck(sqlite3* db, const std::string& a) {
    Stmt q(db, "UPDATE btc_gov_addr SET checked_at = 0 WHERE address = ?");
    q.text(1, a).run();
}

}  // namespace

bool govAddrOk(const std::string& a) {
    if (a.size() < 26 || a.size() > 90) return false;
    for (char c : a)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    return a[0] == '1' || a[0] == '3' || a.rfind("bc1", 0) == 0;
}

void govOpen(sqlite3* db) {
    char* err = nullptr;
    const char* schema =
        // how: seed — из списка в коде, official — из публикации государства,
        // cospend — потрачен вместе с его адресом. checked_at: 0 — пора сверить.
        "CREATE TABLE IF NOT EXISTS btc_gov_addr ("
        "  address TEXT PRIMARY KEY, gov TEXT NOT NULL, how TEXT NOT NULL, at INTEGER NOT NULL DEFAULT 0,"
        "  bal_sats INTEGER NOT NULL DEFAULT 0, bal_height INTEGER NOT NULL DEFAULT 0,"
        "  checked_at INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_btc_gov_addr_gov ON btc_gov_addr(gov);"
        // Запас государства на конец суток — для изменения за неделю.
        "CREATE TABLE IF NOT EXISTS btc_gov_day ("
        "  day INTEGER NOT NULL, gov TEXT NOT NULL, sats INTEGER NOT NULL, n INTEGER NOT NULL,"
        "  PRIMARY KEY (day, gov));";
    if (sqlite3_exec(db, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[GOV] схема: " << (err ? err : "?") << std::endl;
        sqlite3_free(err);
        return;
    }
    {
        Stmt q(db, "SELECT address, gov FROM btc_gov_addr");
        std::lock_guard<std::mutex> l(g_mx);
        while (q.row()) g_gov[q.str(0)] = q.str(1);
    }
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    for (const auto& s : SEEDS) add(db, s.addr, s.gov, "seed");
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

void govBlock(sqlite3* db, const Block& b) {
    for (const auto& tx : b.txs) {
        std::string gov;
        for (const auto& i : tx.in) {
            const std::string g = govOf(i.addr);
            if (g.empty()) continue;
            gov = g;
            recheck(db, i.addr);
        }
        for (const auto& o : tx.out)
            if (!govOf(o.addr).empty()) recheck(db, o.addr);
        if (gov.empty() || tx.in.size() > LEARN_MAX) continue;
        for (const auto& i : tx.in)
            if (!i.addr.empty() && govOf(i.addr).empty() && add(db, i.addr, gov, "cospend"))
                std::cout << "[GOV] " << gov << ": новый адрес по совместной трате " << i.addr << std::endl;
    }
}

void govRollback(sqlite3* db) {
    sqlite3_exec(db, "UPDATE btc_gov_addr SET checked_at = 0", nullptr, nullptr, nullptr);
}

int govOfficial(sqlite3* db, const std::string& gov, const std::vector<std::string>& addrs) {
    int added = 0;
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    for (const auto& a : addrs)
        if (govAddrOk(a) && add(db, a, gov, "official")) ++added;
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    if (added) std::cout << "[GOV] " << gov << ": новых адресов из публикации — " << added << std::endl;
    return added;
}

std::vector<std::string> govStale(sqlite3* db, size_t limit) {
    std::vector<std::string> out;
    Stmt q(db, "SELECT address FROM btc_gov_addr WHERE checked_at < ? ORDER BY checked_at LIMIT ?");
    q.num(1, now() - RECHECK_SEC).num(2, static_cast<long long>(limit));
    while (q.row()) out.push_back(q.str(0));
    return out;
}

void govSetBalance(sqlite3* db, const std::string& address, long long sats, long long height) {
    Stmt q(db, "UPDATE btc_gov_addr SET bal_sats = ?, bal_height = ?, checked_at = ? WHERE address = ?");
    q.num(1, sats).num(2, height).num(3, now()).text(4, address).run();
}

void govSnapshot(sqlite3* db, long long t, long long keepFrom) {
    // Снимок — только когда сверены все адреса государства: иначе неделя
    // назад и сейчас считались бы по разным спискам.
    Stmt q(db, "INSERT OR REPLACE INTO btc_gov_day(day, gov, sats, n) "
               "SELECT ?, gov, SUM(bal_sats), COUNT(*) FROM btc_gov_addr GROUP BY gov "
               "HAVING MIN(checked_at) > 0");
    q.num(1, t - t % 86400).run();
    Stmt d(db, "DELETE FROM btc_gov_day WHERE day < ?");
    d.num(1, keepFrom).run();
}

std::string govStats(sqlite3* db) {
    std::string out = "\n\n🏛 <b>Государства по цепочке</b> (адреса, раскрытые ими)";
    Stmt q(db, "SELECT gov, COUNT(*), SUM(bal_sats), SUM(checked_at > 0), SUM(how = 'cospend') FROM btc_gov_addr "
               "GROUP BY gov ORDER BY SUM(bal_sats) DESC");
    bool any = false;
    while (q.row()) {
        any = true;
        char buf[160];
        std::snprintf(buf, sizeof buf, "\n· %s: %.2f BTC · адресов %lld (сверено %lld, нашёл сам %lld)",
                      q.str(0).c_str(), static_cast<double>(q.col(2)) / 1e8, q.col(1), q.col(3), q.col(4));
        out += buf;
    }
    if (!any) out += "\nАдресов нет";
    return out;
}
