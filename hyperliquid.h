#pragma once

#include <string>
#include <vector>
#include <cstdint>

std::vector<std::string> hlWatchedAddresses();

struct HlRecipient {
    std::string chatId;
    std::string label;
    uint64_t thresholdNanos = 0;
};
std::vector<HlRecipient> hlWatchersFor(const std::string& addressLower);

struct HlUserWallet {
    std::string address;
    std::string label;
};
std::vector<HlUserWallet> hlUserWallets(const std::string& chatId);

bool initHyperliquid();

void startHyperliquidLoop();

void stopHyperliquid();

struct HlMessage {
    std::string text;
    std::string keyboard;
};

HlMessage buildPositionsPicker(const std::string& chatId);
HlMessage buildWalletPositions(const std::string& chatId, const std::string& address);

HlMessage buildVenueMenu(const std::string& chatId);

bool renderHyperliquidView(const std::string& chatId, const std::string& action,
                           const std::string& param, HlMessage& out);

bool handleHyperliquidCallback(const std::string& chatId, const std::string& action,
                               const std::string& param, const std::string& data,
                               long long messageId, const std::string& callbackQueryId);

std::string hyperliquidStatsLine();
