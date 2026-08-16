#include "message_queue.h"
#include <set>
#include "premium.h"

#include <sqlite3.h>
#include <mutex>
#include <tuple>
#include <chrono>
#include <algorithm>
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
    if (!senderThreads.empty()) {
        std::cerr << "[QUEUE] setDeadUserHandler called after start() — ignored" << std::endl;
        return;
    }
    deadUserHandler = std::move(handler);
}

void SafeMessageQueue::initCounters() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (prepareOrLog(db,&s,"UPDATE deliveries SET status=0 WHERE status=5")) {
        if (sqlite3_step(s)==SQLITE_DONE) {
            const int n = sqlite3_changes(db);
            if (n > 0) std::cout << "[QUEUE] Вернули в очередь после перезапуска: " << n << std::endl;
        }
        sqlite3_finalize(s);
    }
    if (prepareOrLog(db,&s,"SELECT COUNT(*) FROM deliveries WHERE status IN (0,3)")) {
        if (sqlite3_step(s)==SQLITE_ROW) queueSize.store(sqlite3_column_int64(s,0));
        sqlite3_finalize(s);
    }
}

void SafeMessageQueue::throttleSend() {
    static std::mutex m;
    static std::chrono::steady_clock::time_point last;
    constexpr int MIN_GAP_MS = 1000 / TG_RATE_LIMIT;

    std::lock_guard<std::mutex> l(m);
    const auto now = std::chrono::steady_clock::now();
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    if (since < MIN_GAP_MS)
        std::this_thread::sleep_for(std::chrono::milliseconds(MIN_GAP_MS - since));
    last = std::chrono::steady_clock::now();
}

void SafeMessageQueue::releaseClaimed(int64_t id) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"UPDATE deliveries SET status=0 WHERE id=? AND status=5")) return;
    sqlite3_bind_int64(s,1,id);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

bool SafeMessageQueue::markTerminal(int64_t id, int st, int rc, time_t nr) {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"UPDATE deliveries SET status=?,retry_count=?,next_retry_at=? WHERE id=? AND status IN (0,3,5)")) return false;
    sqlite3_bind_int(s,1,st); sqlite3_bind_int(s,2,rc); sqlite3_bind_int64(s,3,nr); sqlite3_bind_int64(s,4,id);
    int stepRc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (stepRc!=SQLITE_DONE) { std::cerr << "[QUEUE] terminal UPDATE failed: " << sqlite3_errmsg(db) << std::endl; return false; }
    if (sqlite3_changes(db)!=1) { std::cerr << "[QUEUE] terminal UPDATE changed no row for delivery #" << id << std::endl; return false; }
    queueSize.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

RetryResult SafeMessageQueue::scheduleRetry(int64_t id) {
    std::lock_guard<std::mutex> l(dbMutex);
    sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"SELECT retry_count FROM deliveries WHERE id=? AND status IN (0,3,5)")) return RetryResult::Error;
    sqlite3_bind_int64(s,1,id);
    if (sqlite3_step(s)!=SQLITE_ROW) { sqlite3_finalize(s); return RetryResult::Error; }
    int curRetry = sqlite3_column_int(s,0);
    sqlite3_finalize(s);

    int safeRetry = std::clamp(curRetry, 0, 4);
    int newStatus = (safeRetry>=4) ? 4 : 3;
    time_t nextRetry = time(nullptr) + std::min(30*(1<<safeRetry), 600);

    sqlite3_stmt* u;
    if (!prepareOrLog(db,&u,"UPDATE deliveries SET status=?,retry_count=?,next_retry_at=? WHERE id=? AND status IN (0,3,5)")) return RetryResult::Error;
    sqlite3_bind_int(u,1,newStatus); sqlite3_bind_int(u,2,safeRetry+1); sqlite3_bind_int64(u,3,nextRetry); sqlite3_bind_int64(u,4,id);
    int rc = sqlite3_step(u);
    sqlite3_finalize(u);
    if (rc!=SQLITE_DONE) { std::cerr << "[QUEUE] retry UPDATE failed: " << sqlite3_errmsg(db) << std::endl; return RetryResult::Error; }
    if (sqlite3_changes(db)==0) return RetryResult::Error;

    if (newStatus==4) {
        std::cerr << "[QUEUE] Delivery #" << id << " FAILED after 5 retries" << std::endl;
        queueSize.fetch_sub(1, std::memory_order_relaxed);
        return RetryResult::PermanentlyFailed;
    }
    return RetryResult::Scheduled;
}

void SafeMessageQueue::senderLoop() {

    while (qRunning.load(std::memory_order_acquire)) {
        time_t ra=globalRetryAfter.load(std::memory_order_relaxed);
        if (ra>0&&time(nullptr)<ra) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        if (queueSize.load(std::memory_order_relaxed)==0) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        std::vector<std::tuple<int64_t,std::string,std::string>> batch;
        bool prepFailed=false;
        { std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
          if (!prepareOrLog(db,&s,"SELECT d.id,d.chat_id,a.message FROM deliveries d JOIN alerts a ON a.id=d.alert_id WHERE d.status IN (0,3) AND d.next_retry_at<=? ORDER BY d.priority DESC, d.id ASC LIMIT 40")) { prepFailed=true; }
          else {
              sqlite3_bind_int64(s,1,time(nullptr));
              while (sqlite3_step(s)==SQLITE_ROW) batch.emplace_back(sqlite3_column_int64(s,0),safeColumnText(s,1),safeColumnText(s,2));
              sqlite3_finalize(s);
          }
          if (!prepFailed && !batch.empty()) {
              if (prepareOrLog(db,&s,"UPDATE deliveries SET status=5 WHERE id=? AND status IN (0,3)")) {
                  std::vector<std::tuple<int64_t,std::string,std::string>> claimed;
                  claimed.reserve(batch.size());
                  for (auto& t : batch) {
                      sqlite3_bind_int64(s,1,std::get<0>(t));
                      const bool ok = sqlite3_step(s)==SQLITE_DONE && sqlite3_changes(db)==1;
                      sqlite3_reset(s);
                      sqlite3_clear_bindings(s);
                      if (ok) claimed.push_back(std::move(t));
                  }
                  sqlite3_finalize(s);
                  batch.swap(claimed);
              } else {
                  batch.clear();
              }
          }
        }
        if (prepFailed) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        if (batch.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        busyThreads.fetch_add(1, std::memory_order_relaxed);
        bool aborted=false;
        size_t sent=0;
        for (auto& [did,cid,msg]:batch) {
            ++sent;
            try {
                auto res=sendMsg(cid,msg);
                if (res.ok) {
                    markTerminal(did,1,0,0);
                    sentTotal.fetch_add(1, std::memory_order_relaxed);
                }
                else if (res.retryAfterSec>0) {
                    globalRetryAfter.store(time(nullptr)+res.retryAfterSec, std::memory_order_relaxed);
                    std::cerr << "[TG] 429: pausing " << res.retryAfterSec << "s (message left pending, not counted as a retry)" << std::endl;
                    releaseClaimed(did);
                    aborted=true;
                    break;
                }
                else if (res.deadUser) {
                    markTerminal(did,2,0,0);
                    try {
                        if (deadUserHandler) deadUserHandler(cid);
                    } catch (const std::exception& e) {
                        std::cerr << "[QUEUE] deadUserHandler threw for " << cid << ": " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "[QUEUE] deadUserHandler threw unknown exception for " << cid << std::endl;
                    }
                }
                else {
                    scheduleRetry(did);
                }
            } catch (const std::exception& e) {
                std::cerr << "[QUEUE] Delivery #" << did << " threw: " << e.what() << std::endl;
                scheduleRetry(did);
            } catch (...) {
                std::cerr << "[QUEUE] Delivery #" << did << " threw unknown exception" << std::endl;
                scheduleRetry(did);
            }
            throttleSend();
        }
        if (aborted) {
            size_t k=0;
            for (auto& t : batch) { if (++k <= sent) continue; releaseClaimed(std::get<0>(t)); }
        }
        busyThreads.fetch_sub(1, std::memory_order_relaxed);
    }
}

void SafeMessageQueue::start() {
    if (!senderThreads.empty()) return;
    qRunning.store(true, std::memory_order_release);
    initCounters();
    const auto rec = queueSize.load();
    if (rec > 0) std::cout << "[QUEUE] Recovered " << rec << " pending deliveries" << std::endl;

    for (int i = 0; i < SENDER_THREADS; i++)
        senderThreads.emplace_back(&SafeMessageQueue::senderLoop, this);
}

SafeMessageQueue::~SafeMessageQueue() { stop(); }

void SafeMessageQueue::stop() {
    qRunning.store(false, std::memory_order_release);
    for (auto& t : senderThreads) if (t.joinable()) t.join();
    senderThreads.clear();
}

size_t SafeMessageQueue::size() { return queueSize.load(std::memory_order_relaxed); }

void SafeMessageQueue::syncSize() {
    std::lock_guard<std::mutex> l(dbMutex); sqlite3_stmt* s;
    if (prepareOrLog(db,&s,"SELECT COUNT(*) FROM deliveries WHERE status IN (0,3,5)")) {
        if (sqlite3_step(s)==SQLITE_ROW) { size_t real=sqlite3_column_int64(s,0), atm=queueSize.load();
            if (real!=atm) { std::cerr << "[QUEUE] Size drift: atomic="<<atm<<" real="<<real<<", correcting" << std::endl; queueSize.store(real); } } sqlite3_finalize(s); }
}

bool SafeMessageQueue::enqueueToRecipients(const std::string& text, const std::vector<std::string>& recipients) {
    if (recipients.empty()) return true;
    if (text.empty()) {
        std::cerr << "[QUEUE] empty message rejected for " << recipients.size()
                  << " recipient(s)" << std::endl;
        return false;
    }
    size_t batchSize = recipients.size();

    const std::set<std::string> premium = premiumSubsetOf(recipients);
    std::vector<int> prio;
    prio.reserve(recipients.size());
    for (const auto& c : recipients) prio.push_back(premium.count(c) ? 1 : 0);

    auto txStart=std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l(dbMutex);

    size_t current = queueSize.load(std::memory_order_relaxed);
    if (current >= MAX_QUEUE_SIZE || batchSize > MAX_QUEUE_SIZE - current) {
        logCritical("Queue OVERLOAD (" + std::to_string(current) + "+" +
                    std::to_string(batchSize) + ">" + std::to_string(MAX_QUEUE_SIZE) +
                    ") — alert rejected!");
        return false;
    }

    if (sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        std::cerr << "[QUEUE] BEGIN failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_stmt* s;
    if (!prepareOrLog(db,&s,"INSERT INTO alerts(message,created_at) VALUES(?,?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
    sqlite3_bind_text(s,1,text.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,time(nullptr));
    if (sqlite3_step(s)!=SQLITE_DONE) { std::cerr << "[QUEUE] alert insert failed: " << sqlite3_errmsg(db) << std::endl; sqlite3_finalize(s); sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
    int64_t aid=sqlite3_last_insert_rowid(db); sqlite3_finalize(s);
    if (!prepareOrLog(db,&s,"INSERT INTO deliveries(alert_id,chat_id,status,retry_count,next_retry_at,priority) VALUES(?,?,0,0,0,?)")) { sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); return false; }
    for (size_t i=0;i<recipients.size();i++) {
        const std::string& c = recipients[i];
        sqlite3_reset(s); sqlite3_bind_int64(s,1,aid); sqlite3_bind_text(s,2,c.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(s,3,prio[i]);
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
