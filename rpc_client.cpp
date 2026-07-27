#include "rpc_client.h"

#include <mutex>
#include <map>
#include <thread>
#include <chrono>
#include <iostream>
#include <ctime>
#include <curl/curl.h>

// ======================== Пул эндпоинтов по сетям ========================


std::vector<std::string> RPC_ENDPOINTS;   // задаётся через setRpcEndpoints()
std::atomic<size_t> rpcIndex{0};

// Обработчик, которым слой сообщает наружу о сбое запроса. По умолчанию пуст:
// модуль ничего не знает про структуру статистики бота.
namespace { std::function<void()> g_failureHandler; }

void setRpcFailureHandler(std::function<void()> handler) {
    g_failureHandler = std::move(handler);
}

namespace { inline void reportFailure() { if (g_failureHandler) g_failureHandler(); } }

void setRpcEndpoints(const std::vector<std::string>& endpoints) {
    RPC_ENDPOINTS = endpoints;
    rpcIndex.store(0, std::memory_order_relaxed);
    initEndpointHealth();
}

// ============================== Транспорт ==============================

size_t WriteCB(void* c, size_t s, size_t n, std::string* d) {
    d->append((char*)c, s * n); return s * n;
}

// Постоянное соединение на каждый поток. Раньше здесь были curl_easy_init/cleanup
// на КАЖДЫЙ запрос - это заново открывало TCP-соединение и проводило полное
// TLS-рукопожатие (~100-300мс) при каждом вызове RPC. curl_easy_reset сбрасывает
// параметры запроса, но сохраняет живое соединение, DNS-кэш и TLS-сессии, поэтому
// повторные обращения к тому же узлу переиспользуют уже открытый канал.
// thread_local обязателен: easy-хэндл нельзя разделять между потоками.
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


// ---- Здоровье эндпоинтов ----
// Список публичных узлов быстро устаревает: одни закрываются, другие меняют
// адрес. Вместо того чтобы гадать, какие живы, бот определяет это сам:
// узел, подряд не ответивший несколько раз, временно исключается из ротации,
// а через паузу проверяется снова - вдруг ожил. Так можно смело держать
// длинный список, не боясь, что мёртвые адреса будут тормозить работу.
constexpr int ENDPOINT_FAIL_LIMIT = 5;        // подряд неудач до отключения
constexpr long long ENDPOINT_COOLDOWN_SEC = 600;  // на сколько отключаем

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

// Ближайший рабочий узел начиная с idx. Если живых нет вообще - вернём
// исходный, чтобы бот всё равно попытался, а не встал намертво.
size_t usableEndpointFrom(size_t idx) {
    const size_t n = RPC_ENDPOINTS.size();
    for (size_t k = 0; k < n; ++k) {
        size_t cand = (idx + k) % n;
        if (endpointUsable(cand)) return cand;
    }
    return idx % n;
}

json rpcOnEndpoint(size_t idx, const std::string& method, json params) {
    json r; r["jsonrpc"]="2.0"; r["method"]=method; r["params"]=params; r["id"]=1;
    auto res = http(RPC_ENDPOINTS[idx % RPC_ENDPOINTS.size()], r.dump());
    try { auto p = json::parse(res); if (p.contains("result") && !p["result"].is_null()) return p["result"]; } catch (...) {}
    return nullptr;
}

// Запрос с РАСПРЕДЕЛЕНИЕМ по узлам. Нужен для параллельной загрузки чеков:
// если все потоки бьют в один эндпоинт, мы упираемся в его лимит (~33 запроса
// в секунду) задолго до нехватки времени. Раскладывая запросы по 12 узлам,
// поднимаем суммарную ёмкость примерно до 400/сек. При неудаче пробуем
// следующие узлы, поэтому отказ одного не роняет загрузку блока.
json rpcSpread(size_t seed, const std::string& method, json params) {
    for (size_t attempt = 0; attempt < 3 && running.load(std::memory_order_relaxed); ++attempt) {
        size_t idx = usableEndpointFrom(seed + attempt);
        auto r = rpcOnEndpoint(idx, method, params);
        reportEndpoint(idx, !r.is_null());
        if (!r.is_null()) return r;
        reportFailure();
    }
    return nullptr;
}

// Диагностика: если конкретный публичный RPC периодически "подвисает" на секунды
// без явного сбоя (timeout/ошибка), rpc_failures это не заметит - а лаг растёт
// точно так же. Накопление по каждому эндпоинту отдельно, чтобы увидеть именно
// КАКОЙ узел тормозит, а не гадать. Порогов два, и это не избыточность:
// замечать медленное и уходить с узла - разные решения с разной ценой ошибки.
//
// Порог для СТАТИСТИКИ: с него ответ попадает в счётчик по узлу. Держим низким
// намеренно - именно по этим цифрам видно, тормозит конкретный узел или не
// хватает мощности всем сразу. Поднимешь порог - ослепнешь.
const long long RPC_SLOW_THRESHOLD_MS = 1000;
// Порог для РОТАЦИИ: уходить с узла из-за ответа в секунду смысла нет. Под
// нагрузкой в сотни запросов в секунду хвост распределения вылезает за секунду
// даже у здорового узла, а смена узла стоит нового TLS-рукопожатия. Уходим
// только с того, кто тормозит по-настоящему.
const long long RPC_ROTATE_THRESHOLD_MS = 2500;
const int RPC_SLOW_STREAK_LIMIT = 3;
std::mutex g_rpcLatencyMutex;
std::map<std::string, uint64_t> g_rpcSlowCount;
std::map<std::string, int64_t> g_rpcSlowMaxMs;
std::atomic<int> g_rpcSlowStreak{0};

json rpc(const std::string& method, json params, int maxRetries) {
    json r; r["jsonrpc"]="2.0"; r["method"]=method; r["params"]=params; r["id"]=1;
    std::string body = r.dump();
    for (int a = 0; a < maxRetries && running.load(std::memory_order_relaxed); a++) {
        size_t idx = usableEndpointFrom(rpcIndex.load(std::memory_order_relaxed));
        auto t0 = std::chrono::steady_clock::now();
        auto res = http(RPC_ENDPOINTS[idx], body);
        int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (elapsedMs >= RPC_SLOW_THRESHOLD_MS) {
            {
                std::lock_guard<std::mutex> ll(g_rpcLatencyMutex);
                g_rpcSlowCount[RPC_ENDPOINTS[idx]]++;
                int64_t& mx = g_rpcSlowMaxMs[RPC_ENDPOINTS[idx]];
                if (elapsedMs > mx) mx = elapsedMs;
            }
            std::cerr << "[RPC-SLOW] " << method << " on " << RPC_ENDPOINTS[idx] << " took " << elapsedMs << "ms" << std::endl;
        }
        // Ротация считается по своему, более высокому порогу. Счётчик подряд -
        // чтобы не скакать из-за одиночной заминки: при постоянных соединениях
        // смена узла стоит нового TLS-рукопожатия.
        if (elapsedMs >= RPC_ROTATE_THRESHOLD_MS) {
            if (g_rpcSlowStreak.fetch_add(1, std::memory_order_relaxed) + 1 >= RPC_SLOW_STREAK_LIMIT) {
                g_rpcSlowStreak.store(0, std::memory_order_relaxed);
                size_t cur = rpcIndex.load(std::memory_order_relaxed);
                rpcIndex.store((cur+1) % RPC_ENDPOINTS.size(), std::memory_order_relaxed);
                std::cerr << "[RPC] Rotating away from slow endpoint " << RPC_ENDPOINTS[cur]
                          << " (" << elapsedMs << "ms)" << std::endl;
            }
        } else {
            g_rpcSlowStreak.store(0, std::memory_order_relaxed);
        }
        bool valid = false;
        try {
            auto p = json::parse(res);
            if (p.contains("result") && !p["result"].is_null()) {
                reportEndpoint(idx, true);
                return p["result"];
            }
        } catch (...) {}
        if (!valid) {
            reportEndpoint(idx, false);
            reportFailure();
            size_t cur = rpcIndex.load(std::memory_order_relaxed);
            rpcIndex.store((cur+1) % RPC_ENDPOINTS.size(), std::memory_order_relaxed);
            std::cerr << "[RPC] Switching to " << ((cur+1)%RPC_ENDPOINTS.size()) << " after failure on " << RPC_ENDPOINTS[cur] << std::endl;
        }
        if (a < maxRetries-1) std::this_thread::sleep_for(std::chrono::milliseconds((1<<a)*500));
    }
    return nullptr;
}

// Сводка по медленным узлам для /stats.
std::string rpcSlowSummary() {
    std::lock_guard<std::mutex> ll(g_rpcLatencyMutex);
    if (g_rpcSlowCount.empty()) return "";
    std::string worst; uint64_t worstCount = 0;
    for (const auto& [ep, cnt] : g_rpcSlowCount)
        if (cnt > worstCount) { worstCount = cnt; worst = ep; }
    std::string s = "\n\U0001F422 Slow RPC (>=" + std::to_string(RPC_SLOW_THRESHOLD_MS)
                  + "ms, rotate >=" + std::to_string(RPC_ROTATE_THRESHOLD_MS) + "ms): "
                  + worst + " x" + std::to_string(worstCount)
                  + " (max " + std::to_string(g_rpcSlowMaxMs[worst]) + "ms)";
    if (g_rpcSlowCount.size() > 1)
        s += " [+" + std::to_string(g_rpcSlowCount.size() - 1) + " др.]";
    return s;
}
