#pragma once

#include <string>

struct AiMessage {
    std::string text;
    std::string keyboard;
};

AiMessage buildAiSignals(const std::string& chatId, int days = 1, int venue = 0, int side = 0);

bool handleAiCallback(const std::string& chatId, const std::string& action,
                      const std::string& param, const std::string& data,
                      long long messageId, const std::string& callbackQueryId);

void aiTick();
