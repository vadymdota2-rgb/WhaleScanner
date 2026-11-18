// Запуск httpsCapped из проверки (tests/safe_http_test.py):
//   safe_http_driver <url> <maxBytes> <timeoutSec> [post]
// Печатает одну строку JSON: длина тела, тело (hex) и причина сбоя.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../json.hpp"
#include "../safe_http.h"

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    std::string error;
    const std::string body = httpsCapped(argv[1], argc > 4 ? argv[4] : "",
                                         std::atoi(argv[3]), static_cast<size_t>(std::atoll(argv[2])), &error);
    std::string hex;
    static const char* H = "0123456789abcdef";
    for (const unsigned char c : body) {
        hex += H[c >> 4U];
        hex += H[c & 15U];
    }
    const nlohmann::json out = {{"len", body.size()}, {"hex", hex}, {"error", error}};
    std::puts(out.dump().c_str());
    return 0;
}
