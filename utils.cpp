#include "utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iostream>

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string safeColumnText(sqlite3_stmt* stmt, int col) {
    const unsigned char* p = sqlite3_column_text(stmt, col);
    if (!p) return "";
    int size = sqlite3_column_bytes(stmt, col);
    return std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(size));
}

bool prepareOrLog(sqlite3* db, sqlite3_stmt** stmt, const char* sql) {
    int rc = sqlite3_prepare_v2(db, sql, -1, stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] prepare failed: " << sqlite3_errmsg(db) << " | SQL: " << sql << std::endl;
        if (stmt) *stmt = nullptr;
        return false;
    }
    return true;
}

std::string escapeHtml(const std::string& s) {
    std::string r; r.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  r += "&amp;";  break;
            case '<':  r += "&lt;";   break;
            case '>':  r += "&gt;";   break;
            case '"':  r += "&quot;"; break;
            case '\'': r += "&#39;";  break;
            default:   r += c;        break;
        }
    }
    return r;
}

std::string truncateUtf8(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    size_t end = maxLen;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) end--;
    return s.substr(0, end);
}

std::string safeString(const std::string& s, size_t maxLen) {
    return escapeHtml(truncateUtf8(s, maxLen));
}

bool hexToLL(const std::string& hex, long long& out) {
    if (hex.empty()) return false;
    size_t start = 0;
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) start = 2;
    if (start >= hex.size()) return false;
    for (size_t i = start; i < hex.size(); i++) {
        char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    try {
        size_t pos = 0;
        out = std::stoll(hex, &pos, 16);
        return pos == hex.length();
    } catch (...) {
        return false;
    }
}

namespace {
size_t whitespaceLenAt(const std::string& s, size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') return 1;
    // Неразрывный пробел U+00A0 в UTF-8 занимает ДВА байта: C2 A0. Проверять
    // байты по отдельности нельзя - 0xA0 встречается внутри кириллицы, и такая
    // проверка резала бы слова посередине.
    if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) return 2;
    return 0;
}
}

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size()) {
        size_t w = whitespaceLenAt(s, a);
        if (w == 0) break;
        a += w;
    }
    if (a >= s.size()) return "";

    size_t end = a, i = a;
    while (i < s.size()) {
        size_t w = whitespaceLenAt(s, i);
        if (w) { i += w; }
        else    { ++i; end = i; }
    }
    return s.substr(a, end - a);
}

bool isValidAddress(const std::string& a) {
    if (a.length() != 42 || a[0] != '0' || (a[1] != 'x' && a[1] != 'X')) return false;
    for (size_t i = 2; i < a.length(); i++) {
        char c = a[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

std::string formatThousands(uint64_t v) {
    std::string s = std::to_string(v);
    std::string out;
    int cnt = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (cnt != 0 && cnt % 3 == 0) out.push_back(',');
        out.push_back(*it);
        cnt++;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string formatUsdNanosSigned(long long nanos, bool withPlus) {
    const bool neg = nanos < 0;
    // Модуль берём через unsigned, а не через -nanos: у LLONG_MIN нет
    // положительного двойника в знаковом типе, и обычное отрицание там -
    // неопределённое поведение.
    unsigned long long mag = neg
        ? (~static_cast<unsigned long long>(nanos) + 1ULL)
        : static_cast<unsigned long long>(nanos);

    unsigned long long dollars = mag / 1000000000ULL;
    int cents = static_cast<int>((mag % 1000000000ULL + 5000000ULL) / 10000000ULL);
    if (cents == 100) { dollars++; cents = 0; }

    std::string s = formatThousands(dollars);
    if (cents != 0) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), ".%02d", cents);
        s += buf;
    }
    const char* sign = neg ? "-" : (withPlus ? "+" : "");
    return std::string(sign) + "$" + s;
}

std::string formatPercent(double pct, bool withPlus) {
    if (!std::isfinite(pct)) return "n/a";
    const bool neg = pct < 0;
    double a = neg ? -pct : pct;
    int decimals = a < 1.0 ? 1 : (a < 1000.0 ? 2 : 0);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, a);
    const char* sign = neg ? "-" : (withPlus ? "+" : "");
    return std::string(sign) + buf + "%";
}

std::string formatUsdSmall(long long nanos) {
    if (nanos <= 0) return "$0";
    const double usd = static_cast<double>(nanos) / 1e9;
    char buf[48];
    if (usd >= 0.01) {
        std::snprintf(buf, sizeof(buf), "%.2f", usd);
    } else if (usd >= 0.0001) {
        std::snprintf(buf, sizeof(buf), "%.4f", usd);
    } else if (usd >= 0.00000001) {
        std::snprintf(buf, sizeof(buf), "%.8f", usd);
    } else {
        return "<$0.00000001";
    }
    return std::string("$") + buf;
}
