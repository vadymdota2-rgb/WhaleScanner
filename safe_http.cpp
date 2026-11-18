// Запрос к чужому серверу с пределами — см. safe_http.h.
#include "safe_http.h"

#include <curl/curl.h>
#include <memory>
#include <string>

namespace {

struct Sink {
    std::string* out;
    size_t max;
    bool over;
};

size_t writeCapped(char* data, size_t size, size_t n, void* user) {
    auto* sink = static_cast<Sink*>(user);
    if (n != 0 && size > static_cast<size_t>(-1) / n) {
        sink->over = true;
        return 0;
    }
    const size_t len = size * n;
    if (!capAppend(*sink->out, data, len, sink->max)) {
        sink->over = true;
        return 0;  // curl обрывает передачу
    }
    return len;
}

struct CurlFree {
    void operator()(CURL* c) const { curl_easy_cleanup(c); }
};
struct SlistFree {
    void operator()(curl_slist* l) const { curl_slist_free_all(l); }
};

void fail(std::string* error, const std::string& why) {
    if (error != nullptr) *error = why;
}

}  // namespace

std::string httpsCapped(const std::string& url, const std::string& post, int timeoutSec, size_t maxBytes,
                        std::string* error) {
    if (!url.starts_with("https://")) {
        fail(error, "не https");
        return "";
    }
    const std::unique_ptr<CURL, CurlFree> curl(curl_easy_init());
    if (!curl) {
        fail(error, "curl_easy_init");
        return "";
    }
    CURL* c = curl.get();
    std::string body;
    Sink sink{&body, maxBytes, false};
    std::unique_ptr<curl_slist, SlistFree> headers;

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &writeCapped);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeoutSec));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(maxBytes));
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
#ifdef SAFE_HTTP_TEST_CAINFO
    // Только в сборке проверки (tests/safe_http_test.py): свой удостоверяющий
    // центр для локального https-сервера. В боте этого флага нет.
    curl_easy_setopt(c, CURLOPT_CAINFO, SAFE_HTTP_TEST_CAINFO);
#endif
    if (!post.empty()) {
        headers.reset(curl_slist_append(nullptr, "Content-Type: application/json"));
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(post.size()));
    }

    const CURLcode rc = curl_easy_perform(c);
    if (sink.over || rc == CURLE_FILESIZE_EXCEEDED) {
        fail(error, "ответ больше " + std::to_string(maxBytes) + " байт");
        return "";
    }
    if (rc != CURLE_OK) {
        fail(error, curl_easy_strerror(rc));
        return "";
    }
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 300 && status < 400) {
        fail(error, "перенаправление " + std::to_string(status) + " — не выполняем");
        return "";
    }
    // 4xx и 5xx отдаём телом: у Bot API в нём причина (description).
    return body;
}
