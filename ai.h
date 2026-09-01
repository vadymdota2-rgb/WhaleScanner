#pragma once

#include <string>
#include <vector>

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

// Доступ к Aladdin: владелец плюс те, кому он открыл командой.
bool aiHasAccess(const std::string& chatId);
bool aiGrantAccess(const std::string& chatId);
bool aiRevokeAccess(const std::string& chatId);
std::vector<std::string> aiAccessList();
