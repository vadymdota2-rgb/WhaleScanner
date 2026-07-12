#include "message_queue.h"

#include <sqlite3.h>
#include <mutex>
#include <tuple>
#include <chrono>
#include <iostream>

#include "alert_settings.h"
#include "utils.h"

extern sqlite3* db;
extern std::mutex dbMutex;

void logCritical(const std::string& msg);

namespace {
constexpr size_t MAX_QUEUE_SIZE = 100000;
}

SafeMessageQueue g_msgQueue;

void SafeMessageQueue::setDeadUserHandler(std::function<void(const std::string&)> handler) {
    deadUserHandler = std::move(handler);
}

void SafeMessageQueue::initCounters() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (prepareOrLog(db,&s,"SELECT COUNT(*) FROM deliveries WHERE status IN (0,3)")) {
        if (sqlite3_step(s)==SQLITE_ROW) queueSize.store(sqlite3_column_int64(s,0)); sqlite3_finalize(s); }
}

void SafeMessageQueue::updateStatus(int64_t id, int st, int rc, time_t nr) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"UPDATE deliveries SET status=?,retry_count=?,next_retry_at=? WHERE id=?")) return;
    sqlite3_bind_int(s,1,st); sqlite3_bind_int(s,2,rc); sqlite3_bind_int64(s,3,nr); sqlite3_bind_int64(s,4,id);
    if (sqlite3_step(s)!=SQLITE_DONE) std::cerr << "[QUEUE] status UPDATE failed: " << sqlite3_errmsg(db) << std::endl;
    sqlite3_finalize(s);
}

void SafeMessageQueue::scheduleRetry(int64_t id) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"UPDATE deliveries SET status=CASE WHEN retry_count>=4 THEN 4 ELSE 3 END, retry_count=retry_count+1, next_retry_at=?+MIN(30*(1<<retry_count),600) WHERE id=? AND status!=4")) return;
    sqlite3_bind_int64(s,1,time(nullptr)); sqlite3_bind_int64(s,2,id);
    if (sqlite3_step(s)!=SQLITE_DONE) {
        std::cerr << "[QUEUE] retry UPDATE failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(s); return;
    }
    if (sqlite3_changes(db)>0) { sqlite3_stmt* c;
        if (prepareOrLog(db,&c,"SELECT status FROM deliveries WHERE id=?")) {
            sqlite3_bind_int64(c,1,id); if (sqlite3_step(c)==SQLITE_ROW&&sqlite3_column_int(c,0)==4) {
                std::cerr << "[QUEUE] Delivery #" << id << " FAILED after 5 retries" << std::endl;
                queueSize.fetch_sub(1,std::memory_order_relaxed); } sqlite3_finalize(c); } }
    sqlite3_finalize(s);
}

void SafeMessageQueue::senderLoop() {
    initCounters(); auto rec=queueSize.load();
    if (rec>0) std::cout << "[QUEUE] Recovered " << rec << " pending deliveries" << std::endl;
    while (qRunning.load(std::memory_order_relaxed)) {
        time_t ra=globalRetryAfter.load(std::memory_order_relaxed);
        if (ra>0&&time(nullptr)<ra) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        if (queueSize.load(std::memory_order_relaxed)==0) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        std::vector<std::tuple<int64_t,std::string,std::string>> batch;
        { std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
          if (!prepareOrLog(db,&s,"SELECT d.id,d.chat_id,a.message FROM deliveries d JOIN alerts a ON a.id=d.alert_id WHERE d.status IN (0,3) AND d.next_retry_at<=? ORDER BY d.id ASC LIMIT 100")) continue;
          sqlite3_bind_int64(s,1,time(nullptr));
          while (sqlite3_step(s)==SQLITE_ROW) batch.emplace_back(sqlite3_column_int64(s,0),safeColumnText(s,1),safeColumnText(s,2));
          sqlite3_finalize(s); }
        if (batch.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        for (auto& [did,cid,msg]:batch) {
            auto res=sendMsg(cid,msg);
            if (res.ok) { updateStatus(did,1,0,0); queueSize.fetch_sub(1,std::memory_order_relaxed); }
            else if (res.retryAfterSec>0) { globalRetryAfter.store(time(nullptr)+res.retryAfterSec); scheduleRetry(did);
                std::cerr << "[TG] 429: pausing " << res.retryAfterSec << "s" << std::endl; break; }
            else if (res.deadUser) { updateStatus(did,2,0,0); if (deadUserHandler) deadUserHandler(cid); queueSize.fetch_sub(1,std::memory_order_relaxed); }
            else scheduleRetry(did);
            std::this_thread::sleep_for(std::chrono::milliseconds(SEND_MS));
        }
    }
}

void SafeMessageQueue::start() { qRunning.store(true); senderThread=std::thread(&SafeMessageQueue::senderLoop,this); }

void SafeMessageQueue::stop() { qRunning.store(false); if (senderThread.joinable()) senderThread.join(); }

size_t SafeMessageQueue::size() { return queueSize.load(std::memory_order_relaxed); }

void SafeMessageQueue::syncSize() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (prepareOrLog(db,&s,"SELECT COUNT(*) FROM deliveries WHERE status IN (0,3)")) {
        if (sqlite3_step(s)==SQLITE_ROW) { size_t real=sqlite3_column_int64(s,0), atm=queueSize.load();
            if (real!=atm) { std::cerr << "[QUEUE] Size drift: atomic="<<atm<<" real="<<real<<", correcting" << std::endl; queueSize.store(real); } } sqlite3_finalize(s); }
}

bool SafeMessageQueue::enqueueToRecipients(const std::string& text, const std::vector<std::string>& recipients) {
    if (recipients.empty()) return true;
    size_t current = queueSize.load(std::memory_order_relaxed);
    size_t batchSize = recipients.size();
    if (current + batchSize > MAX_QUEUE_SIZE) {
        logCritical("Queue OVERLOAD (" + std::to_string(current) + "+" +
                    std::to_string(batchSize) + ">" + std::to_string(MAX_QUEUE_SIZE) +
                    ") — alert rejected!");
        return false;
    }

    auto txStart=std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l(dbMutex);
    if (sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[QUEUE] BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT INTO alerts(message,created_at) VALUES(?,?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
    sqlite3_bind_text(s,1,text.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,time(nullptr));
    if (sqlite3_step(s)!=SQLITE_DONE) { std::cerr << "[QUEUE] alert insert failed: " << sqlite3_errmsg(db) << std::endl; sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
    int64_t aid=sqlite3_last_insert_rowid(db); sqlite3_finalize(s);
    if (!prepareOrLog(db,&s,"INSERT INTO deliveries(alert_id,chat_id,status,retry_count,next_retry_at) VALUES(?,?,0,0,0)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
    for (auto& c:recipients) {
        sqlite3_reset(s); sqlite3_bind_int64(s,1,aid); sqlite3_bind_text(s,2,c.c_str(),-1,SQLITE_TRANSIENT);
        if (sqlite3_step(s)!=SQLITE_DONE) {
            std::cerr << "[QUEUE] delivery insert failed: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false;
        }
    }
    sqlite3_finalize(s);
    if (sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[QUEUE] COMMIT failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
        return false;
    }
    auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-txStart).count();
    if (ms>1000) std::cerr << "[DB] ⚠️ Slow enqueue: " << ms << "ms" << std::endl;
    queueSize.fetch_add(batchSize,std::memory_order_relaxed); return true;
}
