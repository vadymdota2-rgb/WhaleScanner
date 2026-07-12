#include "alert_settings.h"

#include <array>
#include <sstream>
#include <cstdio>
#include "json.hpp"
#include "utils.h"

using json = nlohmann::json;

namespace {

constexpr std::array<uint64_t, 6> PRESETS_USD{ 100, 500, 1000, 5000, 10000, 50000 };
constexpr size_t PRESETS_PER_ROW = 2;

constexpr uint64_t NANOS_PER_USD = 1000000000ULL;
constexpr uint64_t NANOS_PER_CENT = NANOS_PER_USD / 100;
constexpr uint64_t MAX_THRESHOLD_USD = 1000000000ULL;
constexpr uint64_t MAX_THRESHOLD_CENTS = MAX_THRESHOLD_USD * 100;
constexpr size_t MAX_INT_DIGITS = 10;

constexpr char ERR_INVALID[]   = "❌ Invalid number.";
constexpr char ERR_POSITIVE[]  = "❌ Threshold must be positive.";
constexpr char ERR_TOO_LARGE[] = "❌ Threshold is too large.";
constexpr char ERR_DECIMALS[]  = "❌ Use at most 2 decimal places (e.g., 7500.50).";
constexpr char RETRY_HINT[] = "\n\nPlease enter a valid amount (e.g., 7500 or 7500.50) or press Cancel.";

enum class ParseResult { OK, INVALID, NOT_POSITIVE, TOO_LARGE, TOO_MANY_DECIMALS };
enum class ApplyResult { Changed, Unchanged };

const char* parseError(ParseResult r) {
    switch (r) {
        case ParseResult::NOT_POSITIVE:      return ERR_POSITIVE;
        case ParseResult::TOO_LARGE:         return ERR_TOO_LARGE;
        case ParseResult::TOO_MANY_DECIMALS: return ERR_DECIMALS;
        default:                             return ERR_INVALID;
    }
}

ParseResult parseThresholdNanos(const std::string& raw, uint64_t& outNanos) {
    std::string t = trim(raw);
    if (t.empty()) return ParseResult::INVALID;
    if (t[0] == '-') return ParseResult::NOT_POSITIVE;

    uint64_t dollars = 0;
    uint64_t cents = 0;
    size_t i = 0;

    size_t intDigits = 0;
    for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; i++) {
        if (++intDigits > MAX_INT_DIGITS) return ParseResult::TOO_LARGE;
        dollars = dollars * 10 + static_cast<uint64_t>(t[i] - '0');
    }
    if (intDigits == 0) return ParseResult::INVALID;

    if (i < t.size()) {
        if (t[i] != '.') return ParseResult::INVALID;
        i++;
        size_t fracDigits = 0;
        for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; i++) {
            if (++fracDigits > 2) return ParseResult::TOO_MANY_DECIMALS;
            cents = cents * 10 + static_cast<uint64_t>(t[i] - '0');
        }
        if (fracDigits == 0) return ParseResult::INVALID;
        if (i != t.size()) return ParseResult::INVALID;
        if (fracDigits == 1) cents *= 10;
    }

    uint64_t totalCents = dollars * 100 + cents;
    if (totalCents == 0) return ParseResult::NOT_POSITIVE;
    if (totalCents > MAX_THRESHOLD_CENTS) return ParseResult::TOO_LARGE;

    outNanos = totalCents * NANOS_PER_CENT;
    return ParseResult::OK;
}

std::string formatUsdNanos(uint64_t nanos) {
    uint64_t dollars = nanos / NANOS_PER_USD;
    int cents = static_cast<int>((nanos % NANOS_PER_USD) / NANOS_PER_CENT);
    std::string s = "$" + formatThousands(dollars);
    if (cents != 0) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), ".%02d", cents);
        s += buf;
    }
    return s;
}

std::string presetLabel(uint64_t usd) {
    if (usd >= 1000 && usd % 1000 == 0) return "$" + std::to_string(usd / 1000) + "K";
    return "$" + formatThousands(usd);
}

ApplyResult applyThreshold(const std::string& chatId, uint64_t nanos) {
    if (getUserThresholdNanos(chatId) == nanos) return ApplyResult::Unchanged;
    setUserThresholdNanos(chatId, nanos);
    refreshWatchers();
    return ApplyResult::Changed;
}

std::string buildStatusText(ApplyResult result, uint64_t nanos) {
    if (result == ApplyResult::Unchanged)
        return "ℹ️ Current threshold is already <b>" + formatUsdNanos(nanos) + "</b>.";
    return "✅ <b>Alert threshold updated</b>\n\n"
           "Current threshold:\n<b>" + formatUsdNanos(nanos) + "</b>";
}

}

TelegramUI::UIMessage TelegramUI::buildAlertThresholdMenu(uint64_t currentThresholdNanos) {
    std::stringstream text;
    text << "💰 <b>Alert Threshold</b>\n\n";
    text << "You'll only be alerted for transactions at or above this amount.\n\n";
    text << "Current threshold: <b>" << formatUsdNanos(currentThresholdNanos) << "</b>\n\n";
    text << "Choose a preset or enter a custom amount:";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    json row = json::array();
    size_t inRow = 0;
    for (uint64_t usd : PRESETS_USD) {
        row.push_back({
            {"text", presetLabel(usd)},
            {"callback_data", "threshold:" + std::to_string(usd)}
        });
        if (++inRow == PRESETS_PER_ROW) {
            keyboard["inline_keyboard"].push_back(row);
            row = json::array();
            inRow = 0;
        }
    }
    if (inRow > 0) keyboard["inline_keyboard"].push_back(row);

    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "✏️ Custom amount"}, {"callback_data", "threshold:custom"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "← Back"}, {"callback_data", "menu:main"}}
    }));

    return {text.str(), keyboard.dump()};
}

bool handleThresholdCallback(const std::string& chatId, const std::string& param, long long messageId) {
    if (param == "custom") {
        g_sessionManager.setState(chatId, UserState::AWAITING_CUSTOM_THRESHOLD);
        replyInPlace(chatId, messageId,
            "💰 <b>Custom Threshold</b>\n\n"
            "Please enter the minimum alert amount in USD (e.g., 7500 or 7500.50):",
            TelegramUI::buildCancelButton());
        return true;
    }

    uint64_t nanos = 0;
    ParseResult pr = parseThresholdNanos(param, nanos);
    if (pr != ParseResult::OK) {
        replyInPlace(chatId, messageId, parseError(pr));
        return true;
    }

    ApplyResult ar = applyThreshold(chatId, nanos);
    auto menu = TelegramUI::buildMainMenu(chatId);
    replyInPlace(chatId, messageId, buildStatusText(ar, nanos) + "\n\n" + menu.text, menu.keyboard);
    return true;
}

bool handleThresholdText(const std::string& chatId, const std::string& text) {
    uint64_t nanos = 0;
    ParseResult pr = parseThresholdNanos(text, nanos);
    if (pr != ParseResult::OK) {
        sendMsg(chatId, std::string(parseError(pr)) + RETRY_HINT, TelegramUI::buildCancelButton());
        return true;
    }

    g_sessionManager.clearSession(chatId);
    ApplyResult ar = applyThreshold(chatId, nanos);
    auto menu = TelegramUI::buildMainMenu(chatId);
    sendMsg(chatId, buildStatusText(ar, nanos) + "\n\n" + menu.text, menu.keyboard);
    return true;
}
