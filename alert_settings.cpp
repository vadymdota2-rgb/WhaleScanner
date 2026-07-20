#include "alert_settings.h"

#include <array>
#include <sstream>
#include <cstdio>
#include "json.hpp"
#include "utils.h"

std::string getUserLanguage(const std::string& chatId);

using json = nlohmann::json;

namespace {

constexpr std::array<uint64_t, 6> PRESETS_USD{ 100, 500, 1000, 5000, 10000, 50000 };
constexpr size_t PRESETS_PER_ROW = 2;

constexpr uint64_t NANOS_PER_USD = 1000000000ULL;
constexpr uint64_t NANOS_PER_CENT = NANOS_PER_USD / 100;
constexpr uint64_t MAX_THRESHOLD_USD = 1000000000ULL;
constexpr uint64_t MAX_THRESHOLD_CENTS = MAX_THRESHOLD_USD * 100;

enum class ParseResult { OK, INVALID, NOT_POSITIVE, TOO_LARGE, TOO_MANY_DECIMALS };
enum class ApplyResult { Changed, Unchanged, Error };

std::string parseError(ParseResult r, Lang lang) {
    switch (r) {
        case ParseResult::NOT_POSITIVE:      return tr(lang, "err_threshold_positive");
        case ParseResult::TOO_LARGE:         return tr(lang, "err_threshold_too_large");
        case ParseResult::TOO_MANY_DECIMALS: return tr(lang, "err_threshold_decimals");
        default:                             return tr(lang, "err_invalid_number");
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
        intDigits++;
        uint64_t digit = static_cast<uint64_t>(t[i] - '0');
        if (dollars > MAX_THRESHOLD_USD / 10 ||
            (dollars == MAX_THRESHOLD_USD / 10 && digit > MAX_THRESHOLD_USD % 10)) {
            return ParseResult::TOO_LARGE;
        }
        dollars = dollars * 10 + digit;
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
    if (!setUserThresholdNanos(chatId, nanos)) return ApplyResult::Error;
    refreshWatchers();
    return ApplyResult::Changed;
}

std::string buildStatusText(ApplyResult result, uint64_t nanos, Lang lang) {
    if (result == ApplyResult::Unchanged)
        return tr(lang, "threshold_unchanged") + " <b>" + formatUsdNanos(nanos) + "</b>.";
    return tr(lang, "threshold_updated") + "\n<b>" + formatUsdNanos(nanos) + "</b>";
}

}

TelegramUI::UIMessage TelegramUI::buildAlertThresholdMenu(uint64_t currentThresholdNanos, Lang lang) {
    std::stringstream text;
    text << tr(lang, "threshold_title") << "\n\n";
    text << tr(lang, "threshold_desc") << "\n\n";
    text << tr(lang, "threshold_current") << " <b>" << formatUsdNanos(currentThresholdNanos) << "</b>\n\n";
    text << tr(lang, "threshold_choose");

    json keyboard;
    keyboard["inline_keyboard"] = json::array();

    json row = json::array();
    size_t inRow = 0;
    for (uint64_t usd : PRESETS_USD) {
        std::string label = presetLabel(usd);
        if (usd * NANOS_PER_USD == currentThresholdNanos) label = label + " ✅";
        row.push_back({
            {"text", label},
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
        {{"text", tr(lang, "threshold_custom_btn")}, {"callback_data", "threshold:custom"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", tr(lang, "back_button")}, {"callback_data", "menu:main"}}
    }));

    return {text.str(), keyboard.dump()};
}

bool handleThresholdCallback(const std::string& chatId, const std::string& param, long long messageId) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    if (param == "custom") {
        g_sessionManager.setState(chatId, UserState::AWAITING_CUSTOM_THRESHOLD);
        replyInPlace(chatId, messageId,
            tr(lang, "threshold_custom_title"),
            TelegramUI::buildCancelButton(lang));
        return true;
    }

    uint64_t nanos = 0;
    ParseResult pr = parseThresholdNanos(param, nanos);
    if (pr != ParseResult::OK) {
        auto menu = TelegramUI::buildAlertThresholdMenu(getUserThresholdNanos(chatId), lang);
        replyInPlace(chatId, messageId, parseError(pr, lang) + "\n\n" + menu.text, menu.keyboard);
        return true;
    }

    ApplyResult ar = applyThreshold(chatId, nanos);
    if (ar == ApplyResult::Error) {
        auto menu = TelegramUI::buildAlertThresholdMenu(getUserThresholdNanos(chatId), lang);
        replyInPlace(chatId, messageId, tr(lang, "threshold_save_failed") + "\n\n" + menu.text, menu.keyboard);
        return true;
    }
    auto menu = TelegramUI::buildMainMenu(chatId);
    replyInPlace(chatId, messageId, buildStatusText(ar, nanos, lang) + "\n\n" + menu.text, menu.keyboard);
    return true;
}

bool handleThresholdText(const std::string& chatId, const std::string& text) {
    Lang lang = langFromCode(getUserLanguage(chatId));
    uint64_t nanos = 0;
    ParseResult pr = parseThresholdNanos(text, nanos);
    if (pr != ParseResult::OK) {
        sendMsg(chatId, parseError(pr, lang) + tr(lang, "threshold_retry_hint"), TelegramUI::buildCancelButton(lang));
        return true;
    }

    ApplyResult ar = applyThreshold(chatId, nanos);
    if (ar == ApplyResult::Error) {
        sendMsg(chatId, tr(lang, "threshold_save_failed"), TelegramUI::buildCancelButton(lang));
        return true;
    }
    g_sessionManager.clearSession(chatId);
    auto menu = TelegramUI::buildMainMenu(chatId);
    sendMsg(chatId, buildStatusText(ar, nanos, lang) + "\n\n" + menu.text, menu.keyboard);
    return true;
}
