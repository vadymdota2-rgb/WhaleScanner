#pragma once

#include <string>
#include "ru.h"

struct BigTradesMessage {
    std::string text;
    std::string keyboard;
};

void refreshFundingCache();

BigTradesMessage buildBigMenu(const std::string& chatId);
BigTradesMessage buildFlowSearch(const std::string& chatId, const std::string& window, const std::string& query);
BigTradesMessage buildBigList(const std::string& chatId, const std::string& venue,
                              const std::string& window, int page);

bool handleBigTradesCallback(const std::string& chatId, const std::string& action,
                             const std::string& param, const std::string& data,
                             long long messageId, const std::string& callbackQueryId);

bool renderBigTradesView(const std::string& chatId, const std::string& action,
                         const std::string& param, BigTradesMessage& out);
