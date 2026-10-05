#include "wallets.h"

#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <sqlite3.h>

#include "autobase.h"
#include "utils.h"
#include "premium.h"
#include "ranking.h"
#include "telegram.h"
#include "btc_chain.h"

/* Списки кошельков в базе бота. Добавляет и убирает их человек в
   приложении (whale_api.py пишет в те же таблицы); здесь — то, что нужно
   самому сканеру: импорт в сервисный аккаунт и снятие кошелька, который
   оказался ботом. */

extern sqlite3* db;
extern std::mutex dbMutex;

namespace {
// Кошелёк — адрес 0x или биткоина (в базе строчными).
bool isWalletKey(const std::string& a) { return isValidAddress(a) || (!a.empty() && isBtcKey(a)); }
}

std::string shortAddress(const std::string& a) {
    if (a.length() <= 12) return a;
    return a.substr(0, 6) + "..." + a.substr(a.length() - 4);
}

namespace {
size_t countUserWhalesLocked(const std::string& chatId) {
    sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT COUNT(*) FROM user_whales WHERE user_id=?")) return 0;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    size_t n=0; if (sqlite3_step(s)==SQLITE_ROW) n=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);
    return n;
}
}

size_t countUserWhales(const std::string& chatId) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT COUNT(*) FROM user_whales WHERE user_id=?")) return 0;
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
    size_t n=0; if (sqlite3_step(s)==SQLITE_ROW) n=sqlite3_column_int64(s,0); sqlite3_finalize(s); return n;
}

namespace {
bool reassignPrimaryLocked(const std::string& chatId) {
    sqlite3_stmt* s;
    if (!prepareOrLog(db, &s,
        "UPDATE user_whales SET is_primary=1 WHERE user_id=? AND whale_id=("
        "SELECT whale_id FROM user_whales WHERE user_id=? ORDER BY created_at ASC, rowid ASC LIMIT 1) "
        "AND NOT EXISTS (SELECT 1 FROM user_whales WHERE user_id=? AND is_primary=1)")) return false;
    for (int i = 1; i <= 3; i++) sqlite3_bind_text(s, i, chatId.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (!ok) std::cerr << "[DB] reassignPrimary failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(s);
    return ok;
}
}

void untrackWalletFromService(const std::string& wallet) {
    const std::string addr = toLower(wallet);
    std::vector<std::pair<std::string, std::string>> recipients; // chatId, label
    int removed = 0;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        sqlite3_stmt* s;
        if (sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) {
            std::cerr << "[DB] untrackWalletFromService BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
            return;
        }
        if (!prepareOrLog(db, &s,
                "SELECT uw.user_id, uw.label FROM user_whales uw "
                "JOIN whale_addresses wa ON wa.id = uw.whale_id "
                "WHERE wa.address = ?")) {
            sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
            return;
        }
        sqlite3_bind_text(s, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            std::string cid = safeColumnText(s, 0);
            std::string lab = safeColumnText(s, 1);
            if (!cid.empty()) recipients.emplace_back(std::move(cid), std::move(lab));
        }
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE) {
            std::cerr << "[DB] untrack recipients read failed: "
                      << sqlite3_errmsg(db) << std::endl;
            sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
            return;
        }
        if (!prepareOrLog(db, &s,
            "DELETE FROM user_whales WHERE whale_id=("
            "SELECT id FROM whale_addresses WHERE address=?)")) {
                sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
                return;
            }
        sqlite3_bind_text(s, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
        const bool deleted = sqlite3_step(s) == SQLITE_DONE;
        if (deleted) removed = sqlite3_changes(db);
        else std::cerr << "[WATCHERS] bot untrack failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s);

        if (!deleted) {
            sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
            return;
        }
        if (removed > 0) {
            for (const auto& r : recipients)
                if (!reassignPrimaryLocked(r.first))
                    std::cerr << "[WATCHERS] primary не переназначен: " << r.first << std::endl;
        }
        if (sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK) {
            std::cerr << "[DB] untrackWalletFromService COMMIT failed: "
                      << sqlite3_errmsg(db) << std::endl;
            sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
            return;
        }
    }
    if (removed <= 0) return;

    std::cout << "[WATCHERS] Bot wallet untracked from " << removed
              << " watchlist(s): " << addr << std::endl;
    refreshWatchers();

    // Бот из найденных поиском — в счётчик банов /autobase.
    for (const auto& [cid, label] : recipients) {
        if (cid != SERVICE_CHAT_ID) continue;
        if (label == "auto-bsc") autoBanned(AutoNet::BSC);
        else if (label == "auto-hl") autoBanned(AutoNet::HL);
    }

    for (const auto& [cid, label] : recipients) {
        if (cid == SERVICE_CHAT_ID) continue;
        Lang lang = langFromCode(getUserLanguage(cid));
        std::string shown = label.empty() || toLower(label) == addr
            ? shortAddress(addr)
            : safeString(label, 32);
        std::string msg = tr(lang, "wallet_bot_removed");
        msg += "\n\n💼 <b>" + shown + "</b>\n<code>" + safeString(addr, 42) + "</code>";
        sendMsg(cid, msg);
    }
}

AddWhaleResult addUserWhale(const std::string& chatId, const std::string& addressArg, const std::string& label) {
    const std::string address = toLower(addressArg);
    if (!isWalletKey(address)) return AddWhaleResult::BAD_ADDRESS;
    ensureUser(chatId);

    if (isPermanentlyBanned(address)) {
        return AddWhaleResult::PERMANENTLY_BANNED;
    }

    const size_t maxWallets = (chatId != SERVICE_CHAT_ID)
                            ? premiumMaxWallets(chatId) : 0;

    std::lock_guard<std::mutex> l(dbMutex);
    if (sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] addUserWhale BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return AddWhaleResult::ERROR;
    }
    if (maxWallets > 0 && countUserWhalesLocked(chatId) >= maxWallets) {
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
        return AddWhaleResult::LIMIT_REACHED;
    }
    sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT OR IGNORE INTO whale_addresses(address) VALUES(?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,address.c_str(),-1,SQLITE_TRANSIENT);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] whale_addresses INSERT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR;
    }
    sqlite3_finalize(s);
    long long whaleId=-1;
    if (!prepareOrLog(db,&s,"SELECT id FROM whale_addresses WHERE address=?")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,address.c_str(),-1,SQLITE_TRANSIENT);
    if (sqlite3_step(s)==SQLITE_ROW) whaleId=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);
    if (whaleId<0) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }

    if (!prepareOrLog(db,&s,"SELECT 1 FROM user_whales WHERE user_id=? AND whale_id=?")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,whaleId);
    bool exists = sqlite3_step(s)==SQLITE_ROW; sqlite3_finalize(s);
    if (exists) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ALREADY_EXISTS; }

    bool firstWallet = false;
    {
        sqlite3_stmt* c;
        if (!prepareOrLog(db,&c,"SELECT 1 FROM user_whales WHERE user_id=? LIMIT 1")) {
            sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
            return AddWhaleResult::ERROR;
        }
        sqlite3_bind_text(c,1,chatId.c_str(),-1,SQLITE_TRANSIENT);
        const int rc = sqlite3_step(c);
        sqlite3_finalize(c);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            std::cerr << "[DB] addUserWhale primary check failed: "
                      << sqlite3_errmsg(db) << std::endl;
            sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
            return AddWhaleResult::ERROR;
        }
        firstWallet = rc != SQLITE_ROW;
    }
    if (!prepareOrLog(db,&s,"INSERT INTO user_whales(user_id,whale_id,label,created_at,is_primary) VALUES(?,?,?,?,?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR; }
    sqlite3_bind_text(s,1,chatId.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,whaleId);
    sqlite3_bind_text(s,3,label.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,4,time(nullptr));
    sqlite3_bind_int(s,5,firstWallet ? 1 : 0);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[DB] user_whales INSERT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return AddWhaleResult::ERROR;
    }
    sqlite3_finalize(s);
    if (sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[DB] addUserWhale COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
        return AddWhaleResult::ERROR;
    }
    return AddWhaleResult::OK;
}

