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
// Единые форматтеры денег и процентов. До их появления каждая из этих задач
// была реализована по 3-4 раза в разных модулях, из-за чего одно и то же число
// выглядело по-разному на разных экранах, а исправление одной ошибки требовало
// правки во всех копиях.
std::string formatUsdNanosSigned(long long nanos, bool withPlus = true);
std::string formatPercent(double pct, bool withPlus = true);
// Мелкие суммы (комиссия сети): дороже цента - два знака ($5.25), дешевле -
// четыре ($0.0045), иначе всё превращалось бы в бессмысленный $0.00.
std::string formatUsdSmall(long long nanos);
bool hexToLL(const std::string& hex, long long& out);
bool isValidAddress(const std::string& a);
std::string safeColumnText(sqlite3_stmt* stmt, int col);
bool prepareOrLog(sqlite3* db, sqlite3_stmt** stmt, const char* sql);
