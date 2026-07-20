#include "utils.h"

#include <algorithm>
#include <cctype>
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

bool stepDoneOrLog(sqlite3* db, sqlite3_stmt* stmt, const char* operation) {
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) return true;
    std::cerr << "[DB] " << operation << " failed (rc=" << rc << "): " << sqlite3_errmsg(db) << std::endl;
    return false;
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

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\xc2\xa0");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n\xc2\xa0");
    return s.substr(a, b - a + 1);
}

bool isValidAddress(const std::string& a) {
    if (a.length() != 42 || a[0] != '0' || a[1] != 'x') return false;
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
