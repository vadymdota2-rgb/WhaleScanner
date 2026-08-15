#pragma once

#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <functional>

enum class RetryResult { Scheduled, PermanentlyFailed, Error };

class SafeMessageQueue {
    std::atomic<size_t> queueSize{0};
    std::atomic<int> busyThreads{0};
    std::atomic<unsigned long long> sentTotal{0};
    std::atomic<time_t> globalRetryAfter{0};
    std::atomic<bool> qRunning{false};
    std::vector<std::thread> senderThreads;
    static constexpr int SENDER_THREADS = 10;
    std::function<void(const std::string&)> deadUserHandler;
    static constexpr int TG_RATE_LIMIT = 28;

    void initCounters();
    void throttleSend();
    void releaseClaimed(int64_t id);
    bool markTerminal(int64_t id, int st, int rc, time_t nr);
    RetryResult scheduleRetry(int64_t id);
    void senderLoop();
public:
    SafeMessageQueue() = default;
    ~SafeMessageQueue();
    SafeMessageQueue(const SafeMessageQueue&) = delete;
    SafeMessageQueue& operator=(const SafeMessageQueue&) = delete;

    void setDeadUserHandler(std::function<void(const std::string&)> handler);
    void start();
    void stop();
    size_t size();
    int busy() const { return busyThreads.load(std::memory_order_relaxed); }
    int threads() const { return SENDER_THREADS; }
    unsigned long long sent() const { return sentTotal.load(std::memory_order_relaxed); }
    void syncSize();
    bool enqueueToRecipients(const std::string& text, const std::vector<std::string>& recipients);
};

extern SafeMessageQueue g_msgQueue;
