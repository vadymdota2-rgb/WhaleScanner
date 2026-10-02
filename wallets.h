#pragma once
#include <cstddef>
#include <string>

extern const std::string SERVICE_CHAT_ID;

std::string shortAddress(const std::string& a);

enum class AddWhaleResult { OK, ALREADY_EXISTS, LIMIT_REACHED, BAD_ADDRESS, PERMANENTLY_BANNED, ERROR };
size_t countUserWhales(const std::string& chatId);
AddWhaleResult addUserWhale(const std::string& chatId, const std::string& address, const std::string& label);
// Кошелёк оказался ботом: снять его у всех и сказать об этом людям.
void untrackWalletFromService(const std::string& wallet);
