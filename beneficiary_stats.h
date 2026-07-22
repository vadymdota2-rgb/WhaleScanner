#pragma once

#include <string>
#include "alert_settings.h"  // SendResult, sendMsg
#include "tx_analyzer.h"     // TxResult, lookupRouterLabel
#include "json.hpp"

// --- Сервисы main.cpp, используемые модулем ---
extern const std::string OWNER_CHAT_ID;

// --- Точки входа ---
// Вызывать при каждой транзакции с venue == "DEX interaction", чтобы накопить
// статистику (tx.to и адреса-бенефициары) для команды /beneficiaries.
void recordBeneficiarySignal(const nlohmann::json& tx, const TxResult& res);

// Обработка команды /beneficiaries. Возвращает true, если команда распознана
// (сообщение отправлено или доступ отклонён) - тогда main.cpp не должен обрабатывать её дальше.
bool handleBeneficiaryCommand(const std::string& chatId, const std::string& text);
