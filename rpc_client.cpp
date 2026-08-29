#include "rpc_client.h"

#include <mutex>
#include <map>
#include <thread>
#include <chrono>
#include <iostream>
#include <ctime>
#include <curl/curl.h>

std::vector<std::string> RPC_ENDPOINTS;
std::atomic<size_t> rpcIndex{0};

namespace { std::function<void()> g_failureHandler; std::function<void()> g_giveUpHandler; }

void setRpcGiveUpHandler(std::function<void()> handler) {
    g_giveUpHandler = std::move(handler);
}

void setRpcFailureHandler(std::function<void()> handler) {
    g_failureHandler = std::move(handler);
}

namespace {
inline void reportFailure() { if (g_failureHandler) g_failureHandler(); }
inline void reportGiveUp()  { if (g_giveUpHandler) g_giveUpHandler(); }
}

size_t WriteCB(void* c, size_t s, size_t n, std::string* d) {
    d->append((char*)c, s * n); return s * n;
}

struct CurlHandleHolder {
    CURL* h = nullptr;
    CurlHandleHolder() : h(curl_easy_init()) {}
    ~CurlHandleHolder() { if (h) curl_easy_cleanup(h); }
};

std::string http(const std::string& url, const std::string& post, int timeout) {
    thread_local CurlHandleHolder holder;
    CURL* curl = holder.h;
    if (!curl) return "";
    curl_easy_reset(curl);
    std::string res; struct curl_slist* h = nullptr;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCB);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return running.load(std::memory_order_relaxed) ? 0 : 1; });
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (!post.empty()) {
        h = curl_slist_append(h, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    }
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        auto su = url.find("api.telegram.org") != std::string::npos ? "telegram_api" :
                  url.length() <= 80 ? url : url.substr(0, 80);
        std::cerr << "[HTTP] " << curl_easy_strerror(rc) << " | " << su << std::endl;
    }
    if (h) curl_slist_free_all(h);
    return res;
}

constexpr int ENDPOINT_FAIL_LIMIT = 5;
constexpr long long ENDPOINT_COOLDOWN_SEC = 30;
constexpr int ROLE_ROTATE_SEC = 1800;

std::mutex g_healthMutex;
std::vector<int> g_consecFails;
std::vector<long long> g_disabledUntil;

void initEndpointHealth() {
    std::lock_guard<std::mutex> l(g_healthMutex);
    g_consecFails.assign(RPC_ENDPOINTS.size(), 0);
    g_disabledUntil.assign(RPC_ENDPOINTS.size(), 0);
}

bool endpointUsable(size_t idx) {
    std::lock_guard<std::mutex> l(g_healthMutex);
    if (idx >= g_disabledUntil.size()) return true;
    return g_disabledUntil[idx] <= static_cast<long long>(time(nullptr));
}

void reportEndpoint(size_t idx, bool ok) {
    std::lock_guard<std::mutex> l(g_healthMutex);
    if (idx >= g_consecFails.size()) return;
    if (ok) { g_consecFails[idx] = 0; g_disabledUntil[idx] = 0; return; }
    if (++g_consecFails[idx] >= ENDPOINT_FAIL_LIMIT) {
        g_consecFails[idx] = 0;
        g_disabledUntil[idx] = static_cast<long long>(time(nullptr)) + ENDPOINT_COOLDOWN_SEC;
        std::cerr << "[RPC] Endpoint disabled for " << ENDPOINT_COOLDOWN_SEC << "s: "
                  << RPC_ENDPOINTS[idx] << std::endl;
    }
}

size_t usableEndpointFrom(size_t idx) {
    const size_t n = RPC_ENDPOINTS.size();
    if (n == 0) return 0;
    for (size_t k = 0; k < n; ++k) {
        size_t cand = (idx + k) % n;
        if (endpointUsable(cand)) return cand;
    }
    return idx % n;
}

enum class RpcRole { Logs = 0, Fill = 1, WsHttp = 2 };
constexpr int kRoleCount = 3;

const char* roleName(RpcRole r) {
    switch (r) {
        case RpcRole::Logs:   return "logs";
        case RpcRole::Fill:   return "fill";
        case RpcRole::WsHttp: return "ws-http";
    }
    return "?";
}

RpcRole roleForMethod(const std::string& m) {
    if (m == "eth_getLogs" || m == "eth_getFilterLogs" || m == "eth_getFilterChanges" ||
        m == "eth_getBlockReceipts" || m == "eth_getBlockByNumber" || m == "eth_getBlockByHash")
        return RpcRole::Logs;
    if (m == "eth_blockNumber")
        return RpcRole::WsHttp;
    return RpcRole::Fill;
}

std::mutex g_roleMutex;
size_t g_holder[kRoleCount] = {0, 0, 0};
size_t g_base = 0;
time_t g_lastRotate = 0;

std::string epLabel(size_t idx) {
    if (idx >= RPC_ENDPOINTS.size()) return "?";
    const std::string& u = RPC_ENDPOINTS[idx];
    auto pos = u.find("://");
    std::string s = (pos == std::string::npos) ? u : u.substr(pos + 3);
    if (s.size() > 36) s.resize(36);
    return "#" + std::to_string(idx) + " " + s;
}

size_t pickEndpoint(size_t start, const std::vector<char>& used, bool allowShare) {
    const size_t n = RPC_ENDPOINTS.size();
    if (n == 0) return 0;
    for (size_t k = 0; k < n; ++k) {
        size_t i = (start + k) % n;
        if (!endpointUsable(i)) continue;
        if (!allowShare && i < used.size() && used[i]) continue;
        return i;
    }
    return start % n;
}

void logHolders() {
    std::cerr << "[ROLE] logs=" << epLabel(g_holder[0])
              << "  fill=" << epLabel(g_holder[1])
              << "  ws-http=" << epLabel(g_holder[2])
              << " (idle until WS falls back to rpc)" << std::endl;
}

void assignRolesLocked() {
    const size_t n = RPC_ENDPOINTS.size();
    if (n == 0) return;
    std::vector<char> used(n, 0);
    for (int r = 0; r < kRoleCount; ++r) {
        size_t start = (g_base + static_cast<size_t>(r)) % n;
        g_holder[r] = pickEndpoint(start, used, /*allowShare=*/n < 3);
        if (g_holder[r] < n) used[g_holder[r]] = 1;
    }
    rpcIndex.store(g_holder[static_cast<int>(RpcRole::Fill)], std::memory_order_relaxed);
}

void maybeRotateLocked() {
    const size_t n = RPC_ENDPOINTS.size();
    if (n == 0) return;
    const time_t now = time(nullptr);
    if (g_lastRotate == 0) g_lastRotate = now;
    if (now - g_lastRotate < ROLE_ROTATE_SEC) return;
    g_lastRotate = now;
    g_base = (g_base + 1) % n;
    assignRolesLocked();
    std::cerr << "[ROLE] 30min rotate, base=" << g_base << std::endl;
    logHolders();
}

size_t holderOf(RpcRole role) {
    std::lock_guard<std::mutex> l(g_roleMutex);
    maybeRotateLocked();
    return g_holder[static_cast<int>(role)];
}

void failoverRole(RpcRole role, size_t failed) {
    std::lock_guard<std::mutex> l(g_roleMutex);
    const size_t n = RPC_ENDPOINTS.size();
    if (n == 0) return;
    if (g_holder[static_cast<int>(role)] != failed) return;

    std::vector<char> used(n, 0);
    for (int r = 0; r < kRoleCount; ++r) {
        if (r == static_cast<int>(role)) continue;
        if (g_holder[r] < n) used[g_holder[r]] = 1;
    }

    size_t next = pickEndpoint(failed + 1, used, /*allowShare=*/false);
    if (next == failed || !endpointUsable(next))
        next = pickEndpoint(failed + 1, used, /*allowShare=*/true);
    g_holder[static_cast<int>(role)] = next;
    rpcIndex.store(g_holder[static_cast<int>(RpcRole::Fill)], std::memory_order_relaxed);
    std::cerr << "[ROLE] " << roleName(role) << " failover "
              << epLabel(failed) << " → " << epLabel(next) << std::endl;
}

void initRoles() {
    std::lock_guard<std::mutex> l(g_roleMutex);
    g_base = 0;
    g_lastRotate = time(nullptr);
    for (int r = 0; r < kRoleCount; ++r) g_holder[r] = 0;
    assignRolesLocked();
    if (!RPC_ENDPOINTS.empty()) {
        std::cerr << "[ROLE] init, " << RPC_ENDPOINTS.size() << " nodes" << std::endl;
        logHolders();
    }
}

void setRpcEndpoints(const std::vector<std::string>& endpoints) {
    RPC_ENDPOINTS = endpoints;
    rpcIndex.store(0, std::memory_order_relaxed);
    initEndpointHealth();
    initRoles();
}

json rpcOnEndpoint(size_t idx, const std::string& method, json params) {
    if (RPC_ENDPOINTS.empty()) return nullptr;
    json r; r["jsonrpc"]="2.0"; r["method"]=method; r["params"]=params; r["id"]=1;
    auto res = http(RPC_ENDPOINTS[idx % RPC_ENDPOINTS.size()], r.dump());
    try { auto p = json::parse(res); if (p.contains("result") && !p["result"].is_null()) return p["result"]; } catch (...) {}
    return nullptr;
}

const long long RPC_SLOW_THRESHOLD_MS = 1000;
const int RPC_SLOW_STREAK_LIMIT = 3;
std::mutex g_rpcLatencyMutex;
std::map<std::string, uint64_t> g_rpcSlowCount;
std::map<std::string, int64_t> g_rpcSlowMaxMs;
std::mutex g_rpcStreakMutex;
std::map<std::string, int> g_rpcSlowStreakByEp;

json rpc(const std::string& method, json params, int maxRetries) {
    if (RPC_ENDPOINTS.empty()) { reportFailure(); reportGiveUp(); return nullptr; }
    json r; r["jsonrpc"]="2.0"; r["method"]=method; r["params"]=params; r["id"]=1;
    std::string body = r.dump();
    const RpcRole role = roleForMethod(method);
    size_t lastFailed = static_cast<size_t>(-1);

    for (int a = 0; a < maxRetries && running.load(std::memory_order_relaxed); a++) {
        size_t idx = holderOf(role);
        if (lastFailed != static_cast<size_t>(-1) && idx == lastFailed)
            failoverRole(role, lastFailed);
        idx = holderOf(role);

        auto t0 = std::chrono::steady_clock::now();
        auto res = http(RPC_ENDPOINTS[idx], body);
        int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsedMs >= RPC_SLOW_THRESHOLD_MS) {
            {
                std::lock_guard<std::mutex> ll(g_rpcLatencyMutex);
                g_rpcSlowCount[RPC_ENDPOINTS[idx]]++;
                int64_t& mx = g_rpcSlowMaxMs[RPC_ENDPOINTS[idx]];
                if (elapsedMs > mx) mx = elapsedMs;
            }
            std::cerr << "[RPC-SLOW] " << method << " on " << RPC_ENDPOINTS[idx]
                      << " took " << elapsedMs << "ms" << std::endl;
            int streak = 0;
            {
                std::lock_guard<std::mutex> sl(g_rpcStreakMutex);
                streak = ++g_rpcSlowStreakByEp[RPC_ENDPOINTS[idx]];
                if (streak >= RPC_SLOW_STREAK_LIMIT) g_rpcSlowStreakByEp[RPC_ENDPOINTS[idx]] = 0;
            }
            if (streak >= RPC_SLOW_STREAK_LIMIT) {
                std::cerr << "[ROLE] " << roleName(role) << " slow-rotate away from "
                          << epLabel(idx) << std::endl;
                failoverRole(role, idx);
            }
        } else {
            std::lock_guard<std::mutex> sl(g_rpcStreakMutex);
            g_rpcSlowStreakByEp[RPC_ENDPOINTS[idx]] = 0;
        }
        try {
            auto p = json::parse(res);
            if (p.contains("result") && !p["result"].is_null()) {
                reportEndpoint(idx, true);
                return p["result"];
            }
        } catch (...) {}
        {
            reportEndpoint(idx, false);
            reportFailure();
            lastFailed = idx;
            failoverRole(role, idx);
            std::cerr << "[RPC] " << roleName(role) << " fail on " << epLabel(idx) << std::endl;
        }
        if (a < maxRetries-1) std::this_thread::sleep_for(std::chrono::milliseconds((1<<a)*500));
    }
    reportGiveUp();
    return nullptr;
}

json rpcSpread(size_t /*seed*/, const std::string& method, json params) {
    return rpc(method, std::move(params), 3);
}

std::string rpcSlowSummary() {
    std::lock_guard<std::mutex> ll(g_rpcLatencyMutex);
    if (g_rpcSlowCount.empty()) return "";
    std::string worst; uint64_t worstCount = 0;
    for (const auto& [ep, cnt] : g_rpcSlowCount)
        if (cnt > worstCount) { worstCount = cnt; worst = ep; }
    std::string s = "\n\U0001F422 Slow RPC (>=" + std::to_string(RPC_SLOW_THRESHOLD_MS) + "ms): "
                  + worst + " x" + std::to_string(worstCount)
                  + " (max " + std::to_string(g_rpcSlowMaxMs[worst]) + "ms)";
    if (g_rpcSlowCount.size() > 1)
        s += " [+" + std::to_string(g_rpcSlowCount.size() - 1) + " др.]";
    return s;
}
