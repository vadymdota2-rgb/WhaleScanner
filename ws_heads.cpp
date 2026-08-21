#include "ws_heads.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include "json.hpp"

using json = nlohmann::json;

extern std::atomic<bool> running;

namespace {

std::atomic<int64_t> g_wsHead{0};
std::atomic<bool> g_wsOk{false};
std::atomic<time_t> g_wsLastSeen{0};
std::thread g_wsThread;
std::atomic<bool> g_wsStop{false};
std::string g_wsUrl;

bool hexToI64(const std::string& h, int64_t& out) {
    std::string s = h;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty()) return false;
    try {
        out = static_cast<int64_t>(std::stoull(s, nullptr, 16));
        return true;
    } catch (...) {
        return false;
    }
}

void handleWsPayload(const std::string& payload) {
    try {
        auto j = json::parse(payload, nullptr, false);
        if (!j.is_object()) return;

        // subscription event: {"method":"eth_subscription","params":{"result":{"number":"0x..."},...}}
        if (j.contains("method") && j["method"].is_string() &&
            j["method"].get<std::string>() == "eth_subscription" &&
            j.contains("params") && j["params"].is_object()) {
            const auto& p = j["params"];
            if (p.contains("result") && p["result"].is_object() &&
                p["result"].contains("number") && p["result"]["number"].is_string()) {
                int64_t n = 0;
                if (hexToI64(p["result"]["number"].get<std::string>(), n) && n > 0) {
                    g_wsHead.store(n, std::memory_order_relaxed);
                    g_wsOk.store(true, std::memory_order_relaxed);
                    g_wsLastSeen.store(time(nullptr), std::memory_order_relaxed);
                }
            }
            return;
        }

        // some nodes wrap differently
        if (j.contains("params") && j["params"].is_object()) {
            const auto& p = j["params"];
            if (p.contains("result") && p["result"].is_object() &&
                p["result"].contains("number") && p["result"]["number"].is_string()) {
                int64_t n = 0;
                if (hexToI64(p["result"]["number"].get<std::string>(), n) && n > 0) {
                    g_wsHead.store(n, std::memory_order_relaxed);
                    g_wsOk.store(true, std::memory_order_relaxed);
                    g_wsLastSeen.store(time(nullptr), std::memory_order_relaxed);
                }
            }
        }
    } catch (...) {}
}

bool wsSession(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L); // WebSocket
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        std::cerr << "[WS] connect failed: " << curl_easy_strerror(rc) << " | " << url << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }
    std::cout << "[WS] connected: " << url << std::endl;

    const char* sub =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_subscribe\",\"params\":[\"newHeads\"]}";
    size_t sent = 0;
    rc = curl_ws_send(curl, sub, std::strlen(sub), &sent, 0, CURLWS_TEXT);
    if (rc != CURLE_OK) {
        std::cerr << "[WS] subscribe send failed: " << curl_easy_strerror(rc) << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }

    std::vector<char> buf(64 * 1024);
    std::string acc;
    while (running.load(std::memory_order_relaxed) && !g_wsStop.load(std::memory_order_relaxed)) {
        size_t nread = 0;
        const struct curl_ws_frame* meta = nullptr;
        rc = curl_ws_recv(curl, buf.data(), buf.size(), &nread, &meta);
        if (rc == CURLE_OK && nread > 0 && meta) {
            if (meta->flags & CURLWS_TEXT) {
                acc.append(buf.data(), nread);
                // simple: one JSON object per message (typical for RPC WS)
                handleWsPayload(acc);
                acc.clear();
            } else if (meta->flags & CURLWS_CLOSE) {
                std::cerr << "[WS] server closed" << std::endl;
                break;
            } else if (meta->flags & CURLWS_PING) {
                // libcurl may auto-pong; ignore
            }
        } else if (rc == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else if (rc != CURLE_OK) {
            std::cerr << "[WS] recv: " << curl_easy_strerror(rc) << std::endl;
            break;
        }

        // stale: no head > 45s → mark not ok (HTTP fallback in main)
        time_t last = g_wsLastSeen.load(std::memory_order_relaxed);
        if (last > 0 && time(nullptr) - last > 45) {
            g_wsOk.store(false, std::memory_order_relaxed);
        }
    }

    g_wsOk.store(false, std::memory_order_relaxed);
    curl_easy_cleanup(curl);
    std::cout << "[WS] disconnected" << std::endl;
    return true;
}

void wsLoop() {
    int backoffSec = 1;
    while (running.load(std::memory_order_relaxed) && !g_wsStop.load(std::memory_order_relaxed)) {
        if (g_wsUrl.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        const bool ok = wsSession(g_wsUrl);
        if (!running.load(std::memory_order_relaxed) || g_wsStop.load(std::memory_order_relaxed))
            break;
        g_wsOk.store(false, std::memory_order_relaxed);
        std::cerr << "[WS] reconnect in " << backoffSec << "s"
                  << (ok ? "" : " (after failure)") << std::endl;
        for (int i = 0; i < backoffSec && running.load(std::memory_order_relaxed) &&
                        !g_wsStop.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        backoffSec = std::min(backoffSec * 2, 30);
        // after successful session reset backoff on next success path — reset when connect works
        if (ok) backoffSec = 1;
    }
}

} // namespace

void startWsHeads(const std::string& wssUrl) {
    if (wssUrl.empty()) {
        std::cout << "[WS] disabled (empty URL)" << std::endl;
        return;
    }
    if (g_wsThread.joinable()) return;
    g_wsUrl = wssUrl;
    g_wsStop.store(false, std::memory_order_relaxed);
    g_wsThread = std::thread(wsLoop);
    std::cout << "[WS] heads thread started → " << wssUrl << std::endl;
}

void stopWsHeads() {
    g_wsStop.store(true, std::memory_order_relaxed);
    if (g_wsThread.joinable()) g_wsThread.join();
    g_wsOk.store(false, std::memory_order_relaxed);
}

bool wsHeadsOk() {
    if (!g_wsOk.load(std::memory_order_relaxed)) return false;
    time_t last = g_wsLastSeen.load(std::memory_order_relaxed);
    return last > 0 && (time(nullptr) - last) <= 45;
}

int64_t wsHeadsLatest() {
    return g_wsHead.load(std::memory_order_relaxed);
}
