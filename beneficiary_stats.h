#pragma once

#include <string>
#include "alert_settings.h"
#include "tx_analyzer.h"
#include "json.hpp"

extern const std::string OWNER_CHAT_ID;

void recordBeneficiarySignal(const nlohmann::json& tx, const TxResult& res);

bool handleBeneficiaryCommand(const std::string& chatId, const std::string& text);
