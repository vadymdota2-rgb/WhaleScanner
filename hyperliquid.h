#pragma once

#include <cstdint>
#include <string>
#include <vector>

std::vector<std::string> hlWatchedAddresses();

struct HlRecipient {
    std::string chatId;
    std::string label;
    uint64_t thresholdNanos = 0;
};
std::vector<HlRecipient> hlWatchersFor(const std::string& addressLower);

size_t hlAlertRecipientCount();

bool initHyperliquid();

void invalidateRankCache();
void startHyperliquidLoop();

void stopHyperliquid();

// Автокошелёк убран из базы за бездействие — поиск должен снова его
// заметить, когда он вернётся к торговле.
void hlAutoForget(const std::string& addressLower);

// Ответ на /stats владельца.
std::string hyperliquidStatsLine();
