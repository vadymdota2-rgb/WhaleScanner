#include "alert_settings.h"

#include <sstream>
#include <cmath>
#include <cstdio>
#include "json.hpp"

using json = nlohmann::json;

namespace {

constexpr double MAX_THRESHOLD_USD = 1000000000.0;

std::string formatUsdAmount(double usd) {
    if (usd < 0) usd = 0;
    uint64_t cents = static_cast<uint64_t>(usd * 100.0 + 0.5);
    uint64_t dollars = cents / 100;
    int rem = static_cast<int>(cents % 100);
    std::string s = "$" + formatThousands(dollars);
    if (rem != 0) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), ".%02d", rem);
        s += buf;
    }
    return s;
}

std::string buildUpdatedText(double usd) {
    return "✅ <b>Alert threshold updated</b>\n\n"
           "Current threshold:\n<b>" + formatUsdAmount(usd) + "</b>";
}

bool parseUsd(const std::string& raw, double& out) {
    std::string t = trim(raw);
    if (t.empty()) return false;
    try {
        size_t pos = 0;
        out = std::stod(t, &pos);
        return pos == t.size() && std::isfinite(out);
    } catch (...) {
        return false;
    }
}

} // namespace

TelegramUI::UIMessage TelegramUI::buildAlertThresholdMenu(uint64_t currentThresholdNanos) {
    double currentUsd = nanosToUsd(currentThresholdNanos);

    std::stringstream text;
    text << "💰 <b>Alert Threshold</b>\n\n";
    text << "You'll only be alerted for transactions at or above this amount.\n\n";
    text << "Current threshold: <b>" << formatUsdAmount(currentUsd) << "</b>\n\n";
    text << "Choose a preset or enter a custom amount:";

    json keyboard;
    keyboard["inline_keyboard"] = json::array();
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "$100"}, {"callback_data", "threshold:100"}},
        {{"text", "$500"}, {"callback_data", "threshold:500"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "$1K"}, {"callback_data", "threshold:1000"}},
        {{"text", "$5K"}, {"callback_data", "threshold:5000"}}
    }));
    keyboard["inline_keyboard"].push_back(json::array({
        {{"text", "$10K"}, {"callback_data", "threshold:10000"}},
        {{"text", "$50K"}, {"callback_data", "threshold:50000"}}
    }));
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

    double usd = 0;
    if (!parseUsd(param, usd)) {
        replyInPlace(chatId, messageId, "❌ Invalid threshold value.");
        return true;
    }
    if (usd <= 0) {
        replyInPlace(chatId, messageId, "❌ Threshold must be positive.");
        return true;
    }
    usd = std::round(usd * 100.0) / 100.0;

    setUserThreshold(chatId, usd);
    refreshWatchers();

    auto menu = TelegramUI::buildMainMenu(chatId);
    replyInPlace(chatId, messageId, buildUpdatedText(usd) + "\n\n" + menu.text, menu.keyboard);
    return true;
}

bool handleThresholdText(const std::string& chatId, const std::string& text) {
    double usd = 0;
    if (!parseUsd(text, usd)) {
        sendMsg(chatId,
            "❌ Invalid number.\n\nPlease enter a valid amount (e.g., 7500 or 7500.50) or press Cancel.",
            TelegramUI::buildCancelButton());
        return true;
    }
    if (usd <= 0) {
        sendMsg(chatId,
            "❌ Threshold must be positive.\n\nPlease enter a valid amount or press Cancel.",
            TelegramUI::buildCancelButton());
        return true;
    }
    if (usd > MAX_THRESHOLD_USD) {
        sendMsg(chatId,
            "❌ Threshold is too large.\n\nPlease enter a smaller amount or press Cancel.",
            TelegramUI::buildCancelButton());
        return true;
    }

    usd = std::round(usd * 100.0) / 100.0;
    if (usd < 0.01) {
        sendMsg(chatId,
            "❌ Threshold must be at least $0.01.\n\nPlease enter a valid amount or press Cancel.",
            TelegramUI::buildCancelButton());
        return true;
    }

    setUserThreshold(chatId, usd);
    refreshWatchers();
    g_sessionManager.clearSession(chatId);

    auto menu = TelegramUI::buildMainMenu(chatId);
    sendMsg(chatId, buildUpdatedText(usd) + "\n\n" + menu.text, menu.keyboard);
    return true;
}
