#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include <sqlite3.h>

std::string trim(const std::string& s);
std::string toLower(std::string s);
std::string escapeHtml(const std::string& s);
std::string truncateUtf8(const std::string& s, size_t maxLen);
std::string safeString(const std::string& s, size_t maxLen = 64);
std::string formatThousands(uint64_t v);
bool hexToLL(const std::string& hex, long long& out);
bool isValidAddress(const std::string& a);
std::string safeColumnText(sqlite3_stmt* stmt, int col);
bool prepareOrLog(sqlite3* db, sqlite3_stmt** stmt, const char* sql);
bool stepDoneOrLog(sqlite3* db, sqlite3_stmt* stmt, const char* operation);
