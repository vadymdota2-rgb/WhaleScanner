#include "ws_heads.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <mutex>
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

std::mutex g_urlMutex;
std::vector<std::string> g_wsUrls;
constexpr time_t WS_STALE_SEC = 15;
constexpr time_t WS_SUBSCRIBE_WAIT_SEC = 5;
std::atomic<uint64_t> g_wsSwitches{0};
std::atomic<uint64_t> g_wsStales{0};
std::atomic<uint64_t> g_wsRejects{0};
std::atomic<uint64_t> g_wsConnFails{0};
std::atomic<bool> g_wsEverConnected{false};
std::atomic<bool> g_subConfirmed{false};
std::atomic<bool> g_subRejected{false};
std::atomic<int> g_activeIdx{0};

std::string shortLabel(int idx, const std::string& /*url*/) {
    if (idx == 0) return "primary";
    if (idx == 1) return "backup";
    return "ws" + std::to_string(idx + 1);
}

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

        auto takeNumber = [](const json& result) {
            if (!result.is_object() || !result.contains("number") || !result["number"].is_string())
                return;
            int64_t n = 0;
            if (hexToI64(result["number"].get<std::string>(), n) && n > 0) {
                int64_t prev = g_wsHead.load(std::memory_order_relaxed);
                while (n > prev &&
                       !g_wsHead.compare_exchange_weak(prev, n, std::memory_order_relaxed)) {}
                g_wsOk.store(true, std::memory_order_relaxed);
                g_wsLastSeen.store(time(nullptr), std::memory_order_relaxed);
            }
        };

        if (j.contains("id") && j["id"].is_number_integer() && j["id"].get<int>() == 1 &&
            j.contains("result") && j["result"].is_string()) {
            g_subConfirmed.store(true, std::memory_order_relaxed);
            g_wsLastSeen.store(time(nullptr), std::memory_order_relaxed);
            return;
        }
        if (j.contains("error") && j["error"].is_object()) {
            std::string msg;
            if (j["error"].contains("message") && j["error"]["message"].is_string())
                msg = j["error"]["message"].get<std::string>();
            std::cerr << "[WS] subscribe rejected: " << (msg.empty() ? "unknown" : msg) << std::endl;
            g_subRejected.store(true, std::memory_order_relaxed);
            g_wsRejects.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (j.contains("method") && j["method"].is_string() &&
            j["method"].get<std::string>() == "eth_subscription" &&
            j.contains("params") && j["params"].is_object()) {
            const auto& p = j["params"];
            if (p.contains("result")) takeNumber(p["result"]);
            return;
        }
        if (j.contains("params") && j["params"].is_object()) {
            const auto& p = j["params"];
            if (p.contains("result")) takeNumber(p["result"]);
        }
    } catch (...) {}
}

bool wsSession(const std::string& url, int idx) {
    g_subRejected.store(false, std::memory_order_relaxed);
    g_subConfirmed.store(false, std::memory_order_relaxed);
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        g_wsConnFails.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[WS] connect failed (" << shortLabel(idx, url) << "): "
                  << curl_easy_strerror(rc) << " | " << url << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }
    const int prevIdx = g_activeIdx.exchange(idx, std::memory_order_relaxed);
    if (g_wsEverConnected.exchange(true, std::memory_order_relaxed) && prevIdx != idx)
        g_wsSwitches.fetch_add(1, std::memory_order_relaxed);
    std::cout << "[WS] connected (" << shortLabel(idx, url) << "): " << url << std::endl;

    const char* sub =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_subscribe\",\"params\":[\"newHeads\"]}";
    size_t sent = 0;
    rc = curl_ws_send(curl, sub, std::strlen(sub), &sent, 0, CURLWS_TEXT);
    if (rc != CURLE_OK) {
        g_wsConnFails.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[WS] subscribe send failed: " << curl_easy_strerror(rc) << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }

    g_wsLastSeen.store(time(nullptr), std::memory_order_relaxed);

    std::vector<char> buf(64 * 1024);
    std::string acc;
    while (running.load(std::memory_order_relaxed) && !g_wsStop.load(std::memory_order_relaxed)) {
        size_t nread = 0;
        const struct curl_ws_frame* meta = nullptr;
        rc = curl_ws_recv(curl, buf.data(), buf.size(), &nread, &meta);
        if (rc == CURLE_OK && nread > 0 && meta) {
            if (meta->flags & (CURLWS_TEXT | CURLWS_CONT)) {
                acc.append(buf.data(), nread);
                if (meta->bytesleft == 0) {
                    handleWsPayload(acc);
                    acc.clear();
                } else if (acc.size() > 1024u * 1024u) {
                    std::cerr << "[WS] payload too large, dropping ("
                              << shortLabel(idx, url) << ")" << std::endl;
                    acc.clear();
                }
            } else if (meta->flags & CURLWS_CLOSE) {
                std::cerr << "[WS] server closed (" << shortLabel(idx, url) << ")" << std::endl;
                break;
            } else if (meta->flags & CURLWS_PING) {
                size_t sent = 0;
                curl_ws_send(curl, buf.data(), nread, &sent, 0, CURLWS_PONG);
            }
        } else if (rc == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else if (rc != CURLE_OK) {
            std::cerr << "[WS] recv (" << shortLabel(idx, url) << "): "
                      << curl_easy_strerror(rc) << std::endl;
            break;
        }

        if (g_subRejected.load(std::memory_order_relaxed)) {
            std::cerr << "[WS] switching after rejected subscribe ("
                      << shortLabel(idx, url) << ")" << std::endl;
            break;
        }

        time_t last = g_wsLastSeen.load(std::memory_order_relaxed);
        const time_t staleLimit = g_subConfirmed.load(std::memory_order_relaxed)
                                ? WS_STALE_SEC : WS_SUBSCRIBE_WAIT_SEC;
        if (last > 0 && time(nullptr) - last > staleLimit) {
            g_wsStales.fetch_add(1, std::memory_order_relaxed);
            std::cerr << "[WS] stale >" << staleLimit << "s on " << shortLabel(idx, url)
                      << " — switching" << std::endl;
            g_wsOk.store(false, std::memory_order_relaxed);
            break;
        }
    }

    g_wsOk.store(false, std::memory_order_relaxed);
    curl_easy_cleanup(curl);
    std::cout << "[WS] disconnected (" << shortLabel(idx, url) << ")" << std::endl;
    return true;
}

void wsLoop() {
    int backoffSec = 1;
    size_t nextIdx = 0;
    while (running.load(std::memory_order_relaxed) && !g_wsStop.load(std::memory_order_relaxed)) {
        std::vector<std::string> urls;
        {
            std::lock_guard<std::mutex> l(g_urlMutex);
            urls = g_wsUrls;
        }
        if (urls.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        if (nextIdx >= urls.size()) nextIdx = 0;
        const size_t idx = nextIdx;
        const std::string& url = urls[idx];
        const bool ok = wsSession(url, static_cast<int>(idx));

        nextIdx = (idx + 1) % urls.size();

        if (!running.load(std::memory_order_relaxed) || g_wsStop.load(std::memory_order_relaxed))
            break;
        g_wsOk.store(false, std::memory_order_relaxed);

        const int waitSec = (urls.size() > 1) ? std::min(backoffSec, 3) : backoffSec;
        std::cerr << "[WS] next try in " << waitSec << "s → "
                  << shortLabel(static_cast<int>(nextIdx),
                                nextIdx < urls.size() ? urls[nextIdx] : "")
                  << (ok ? "" : " (after failure)") << std::endl;
        for (int i = 0; i < waitSec && running.load(std::memory_order_relaxed) &&
                        !g_wsStop.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        if (ok) backoffSec = 1;
        else backoffSec = std::min(backoffSec * 2, 30);
    }
}

std::vector<std::string> splitUrls(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void startWsHeads(const std::vector<std::string>& wssUrls) {
    std::vector<std::string> cleaned;
    for (const auto& u : wssUrls) {
        if (!u.empty()) cleaned.push_back(u);
    }
    if (cleaned.empty()) {
        std::cout << "[WS] disabled (empty URL list)" << std::endl;
        return;
    }
    if (g_wsThread.joinable()) return;
    const size_t n = cleaned.size();
    {
        std::lock_guard<std::mutex> l(g_urlMutex);
        g_wsUrls = cleaned;
    }
    g_wsStop.store(false, std::memory_order_relaxed);
    g_wsThread = std::thread(wsLoop);
    std::cout << "[WS] heads thread started, " << n << " endpoint(s):" << std::endl;
    for (size_t i = 0; i < cleaned.size(); ++i)
        std::cout << "  [" << shortLabel(static_cast<int>(i), cleaned[i]) << "] "
                  << cleaned[i] << std::endl;
}

void startWsHeads(const std::string& wssUrlOrList) {
    if (wssUrlOrList.empty()) {
        std::cout << "[WS] disabled (empty URL)" << std::endl;
        return;
    }
    startWsHeads(splitUrls(wssUrlOrList));
}

void stopWsHeads() {
    g_wsStop.store(true, std::memory_order_relaxed);
    if (g_wsThread.joinable()) g_wsThread.join();
    g_wsOk.store(false, std::memory_order_relaxed);
}

}

bool wsHeadsOk() {
    if (!g_wsOk.load(std::memory_order_relaxed)) return false;
    time_t last = g_wsLastSeen.load(std::memory_order_relaxed);
    return last > 0 && (time(nullptr) - last) <= WS_STALE_SEC;
}

int64_t wsHeadsLatest() {
    return g_wsHead.load(std::memory_order_relaxed);
}

std::string wsHeadsStats() {
    return std::to_string(g_wsSwitches.load(std::memory_order_relaxed)) + "/" +
           std::to_string(g_wsStales.load(std::memory_order_relaxed)) + "/" +
           std::to_string(g_wsRejects.load(std::memory_order_relaxed)) + "/" +
           std::to_string(g_wsConnFails.load(std::memory_order_relaxed));
}

std::string wsHeadsActiveLabel() {
    int idx = g_activeIdx.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> l(g_urlMutex);
    if (idx < 0 || idx >= static_cast<int>(g_wsUrls.size())) return "—";
    return shortLabel(idx, g_wsUrls[static_cast<size_t>(idx)]);
}

void startWsBsc() {
    const char* wsEnv = std::getenv("WHALE_WS_URL");
    std::string heads;
    if (wsEnv && std::string(wsEnv).empty()) {
        std::cout << "[WS] heads disabled (WHALE_WS_URL=)" << std::endl;
        return;
    } else if (wsEnv && *wsEnv) {
        heads = wsEnv;
    } else {
        heads = "wss://rpc-bsc.blockmachine.io,"
                "wss://bnb.api.onfinality.io/public-ws";
    }
    if (!heads.empty()) startWsHeads(heads);
}

void stopWsBsc() {
    stopWsHeads();
}
