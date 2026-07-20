#pragma once

#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <functional>

enum class RetryResult { Scheduled, PermanentlyFailed, Error };

class SafeMessageQueue {
    std::atomic<size_t> queueSize{0};
    std::atomic<time_t> globalRetryAfter{0};
    std::atomic<bool> qRunning{false};
    std::thread senderThread;
    std::function<void(const std::string&)> deadUserHandler;
    static constexpr int SEND_MS = 33;

    void initCounters();
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
    void syncSize();
    bool enqueueToRecipients(const std::string& text, const std::vector<std::string>& recipients);
};

extern SafeMessageQueue g_msgQueue;
