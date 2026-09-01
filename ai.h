#pragma once

#include <string>

struct AiMessage {
    std::string text;
    std::string keyboard;
};

// Окна в ЧАСАХ: 1, 6 или 24. Умолчание совпадает с горизонтом, на который
// обучена модель.
AiMessage buildAiSignals(const std::string& chatId, int hours = 24, int venue = 0, int side = 0);

bool handleAiCallback(const std::string& chatId, const std::string& action,
                      const std::string& param, const std::string& data,
                      long long messageId, const std::string& callbackQueryId);

void aiTick();
