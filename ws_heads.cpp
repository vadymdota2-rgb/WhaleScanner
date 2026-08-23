#include "ws_heads.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <sstream>
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
                g_wsHead.store(n, std::memory_order_relaxed);
                g_wsOk.store(true, std::memory_order_relaxed);
                g_wsLastSeen.store(time(nullptr), std::memory_order_relaxed);
            }
        };

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
        std::cerr << "[WS] connect failed (" << shortLabel(idx, url) << "): "
                  << curl_easy_strerror(rc) << " | " << url << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }
    g_activeIdx.store(idx, std::memory_order_relaxed);
    std::cout << "[WS] connected (" << shortLabel(idx, url) << "): " << url << std::endl;

    const char* sub =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_subscribe\",\"params\":[\"newHeads\"]}";
    size_t sent = 0;
    rc = curl_ws_send(curl, sub, std::strlen(sub), &sent, 0, CURLWS_TEXT);
    if (rc != CURLE_OK) {
        std::cerr << "[WS] subscribe send failed: " << curl_easy_strerror(rc) << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }

    // После долгого простоя старый lastSeen (>45с) иначе сразу рвёт сессию,
    // не дождавшись первого head. Grace от момента subscribe.
    g_wsLastSeen.store(time(nullptr), std::memory_order_relaxed);

    std::vector<char> buf(64 * 1024);
    std::string acc;
    while (running.load(std::memory_order_relaxed) && !g_wsStop.load(std::memory_order_relaxed)) {
        size_t nread = 0;
        const struct curl_ws_frame* meta = nullptr;
        rc = curl_ws_recv(curl, buf.data(), buf.size(), &nread, &meta);
        if (rc == CURLE_OK && nread > 0 && meta) {
            if (meta->flags & CURLWS_TEXT) {
                acc.append(buf.data(), nread);
                handleWsPayload(acc);
                acc.clear();
            } else if (meta->flags & CURLWS_CLOSE) {
                std::cerr << "[WS] server closed (" << shortLabel(idx, url) << ")" << std::endl;
                break;
            } else if (meta->flags & CURLWS_PING) {
                // libcurl may auto-pong
            }
        } else if (rc == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else if (rc != CURLE_OK) {
            std::cerr << "[WS] recv (" << shortLabel(idx, url) << "): "
                      << curl_easy_strerror(rc) << std::endl;
            break;
        }

        // тишина > 45с → считаем endpoint мёртвым, пробуем следующий
        time_t last = g_wsLastSeen.load(std::memory_order_relaxed);
        if (last > 0 && time(nullptr) - last > 45) {
            std::cerr << "[WS] stale >45s on " << shortLabel(idx, url)
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

        // следующий endpoint по кругу (failover)
        nextIdx = (idx + 1) % urls.size();

        if (!running.load(std::memory_order_relaxed) || g_wsStop.load(std::memory_order_relaxed))
            break;
        g_wsOk.store(false, std::memory_order_relaxed);

        // при нескольких URL после обрыва быстро пробуем backup
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

} // namespace

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

bool wsHeadsOk() {
    if (!g_wsOk.load(std::memory_order_relaxed)) return false;
    time_t last = g_wsLastSeen.load(std::memory_order_relaxed);
    return last > 0 && (time(nullptr) - last) <= 45;
}

int64_t wsHeadsLatest() {
    return g_wsHead.load(std::memory_order_relaxed);
}

std::string wsHeadsActiveLabel() {
    int idx = g_activeIdx.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> l(g_urlMutex);
    if (idx < 0 || idx >= static_cast<int>(g_wsUrls.size())) return "—";
    return shortLabel(idx, g_wsUrls[static_cast<size_t>(idx)]);
}

// ═══════════════════════════════════════════════════════════════════════════
// Assist WS — eth_getBlockByNumber при lag > 200 (разгрузка HTTP RPC)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

std::mutex g_assistMu;
CURL* g_assistCurl = nullptr;
std::string g_assistUrl;
std::atomic<bool> g_assistConfigured{false};
std::atomic<uint64_t> g_assistBlocks{0};
int g_assistNextId = 1;
time_t g_assistLastOk = 0;

void assistDisconnectLocked() {
    if (g_assistCurl) {
        curl_easy_cleanup(g_assistCurl);
        g_assistCurl = nullptr;
    }
}

bool assistConnectLocked() {
    assistDisconnectLocked();
    if (g_assistUrl.empty()) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, g_assistUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        std::cerr << "[WS-assist] connect failed: " << curl_easy_strerror(rc)
                  << " | " << g_assistUrl << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }
    g_assistCurl = curl;
    std::cout << "[WS-assist] connected: " << g_assistUrl << std::endl;
    return true;
}

nlohmann::json assistRpcLocked(const std::string& method, const nlohmann::json& params,
                                int timeoutMs) {
    if (!g_assistCurl && !assistConnectLocked()) return nullptr;

    const int id = g_assistNextId++;
    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };
    const std::string body = req.dump();
    size_t sent = 0;
    CURLcode rc = curl_ws_send(g_assistCurl, body.data(), body.size(), &sent, 0, CURLWS_TEXT);
    if (rc != CURLE_OK) {
        std::cerr << "[WS-assist] send failed: " << curl_easy_strerror(rc) << std::endl;
        assistDisconnectLocked();
        return nullptr;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    std::vector<char> buf(256 * 1024);
    std::string acc;

    while (std::chrono::steady_clock::now() < deadline) {
        if (!running.load(std::memory_order_relaxed)) break;

        size_t nread = 0;
        const struct curl_ws_frame* meta = nullptr;
        rc = curl_ws_recv(g_assistCurl, buf.data(), buf.size(), &nread, &meta);
        if (rc == CURLE_OK && nread > 0 && meta) {
            if (meta->flags & CURLWS_TEXT) {
                acc.append(buf.data(), nread);
                try {
                    auto j = nlohmann::json::parse(acc, nullptr, false);
                    acc.clear();
                    if (!j.is_object()) continue;
                    // id may be number or string
                    int rid = -1;
                    if (j.contains("id")) {
                        if (j["id"].is_number_integer()) rid = j["id"].get<int>();
                        else if (j["id"].is_string()) {
                            try { rid = std::stoi(j["id"].get<std::string>()); } catch (...) {}
                        }
                    }
                    if (rid != id) continue;
                    if (j.contains("error")) {
                        std::cerr << "[WS-assist] rpc error: " << j["error"].dump() << std::endl;
                        return nullptr;
                    }
                    if (j.contains("result")) return j["result"];
                    return nullptr;
                } catch (...) {
                    acc.clear();
                }
            } else if (meta->flags & CURLWS_CLOSE) {
                assistDisconnectLocked();
                return nullptr;
            }
        } else if (rc == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else if (rc != CURLE_OK) {
            std::cerr << "[WS-assist] recv: " << curl_easy_strerror(rc) << std::endl;
            assistDisconnectLocked();
            return nullptr;
        }
    }
    std::cerr << "[WS-assist] timeout " << method << std::endl;
    assistDisconnectLocked();
    return nullptr;
}

} // namespace

void startWsAssist(const std::string& wssUrl) {
    std::lock_guard<std::mutex> l(g_assistMu);
    g_assistUrl = wssUrl;
    g_assistConfigured.store(!wssUrl.empty(), std::memory_order_relaxed);
    if (wssUrl.empty()) {
        std::cout << "[WS-assist] disabled" << std::endl;
        return;
    }
    // connect lazily on first getBlock
    std::cout << "[WS-assist] ready (lag>200 → getBlock): " << wssUrl << std::endl;
}

void stopWsAssist() {
    std::lock_guard<std::mutex> l(g_assistMu);
    assistDisconnectLocked();
    g_assistConfigured.store(false, std::memory_order_relaxed);
    g_assistUrl.clear();
}

bool wsAssistOk() {
    if (!g_assistConfigured.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> l(g_assistMu);
    return g_assistCurl != nullptr || !g_assistUrl.empty();
}

nlohmann::json wsAssistGetBlock(long long blockNumber, int timeoutMs) {
    if (!g_assistConfigured.load(std::memory_order_relaxed)) return nullptr;
    std::stringstream ss;
    ss << "0x" << std::hex << blockNumber;
    nlohmann::json params = nlohmann::json::array({ss.str(), true});

    std::lock_guard<std::mutex> l(g_assistMu);
    auto result = assistRpcLocked("eth_getBlockByNumber", params, timeoutMs);
    if (result.is_object() && result.contains("transactions")) {
        g_assistBlocks.fetch_add(1, std::memory_order_relaxed);
        g_assistLastOk = time(nullptr);
        return result;
    }
    return nullptr;
}

uint64_t wsAssistBlocksOk() {
    return g_assistBlocks.load(std::memory_order_relaxed);
}


// ─── BSC defaults: 3 разных провайдера ─────────────────────────────────────

void startWsBsc() {
    // heads
    const char* wsEnv = std::getenv("WHALE_WS_URL");
    std::string heads;
    if (wsEnv && std::string(wsEnv).empty()) {
        // явно выкл
        std::cout << "[WS] heads disabled (WHALE_WS_URL=)" << std::endl;
    } else if (wsEnv && *wsEnv) {
        heads = wsEnv;
    } else {
        heads = "wss://rpc-bsc.blockmachine.io,"
                "wss://bnb.api.onfinality.io/public-ws";
    }
    if (!heads.empty()) startWsHeads(heads);

    // assist (третий провайдер)
    const char* assistEnv = std::getenv("WHALE_WS_ASSIST_URL");
    std::string assist;
    if (assistEnv && std::string(assistEnv).empty()) {
        std::cout << "[WS-assist] disabled (WHALE_WS_ASSIST_URL=)" << std::endl;
    } else if (assistEnv && *assistEnv) {
        assist = assistEnv;
    } else {
        assist = "wss://rpc.nodeflare.app/bnb/ws/public";
    }
    if (!assist.empty()) startWsAssist(assist);
}

void stopWsBsc() {
    stopWsHeads();
    stopWsAssist();
}

bool wsAssistWanted(int64_t lagBlocks) {
    return lagBlocks > WS_ASSIST_LAG_THRESHOLD && wsAssistOk();
}
