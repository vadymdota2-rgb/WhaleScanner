#pragma once

/*
 * Что осталось от бота в Telegram. Меню, кнопок и диалогов в чате больше
 * нет — всё это в приложении. Бот отвечает на любое сообщение кнопкой
 * «Открыть приложение», принимает оплату звёздами, рассылает алерты и
 * отвечает владельцу на служебные команды.
 */

#include <cstdint>
#include <string>
#include "ru.h"

// Порог алерта новому человеку; дальше его меняют в приложении.
constexpr uint64_t DEFAULT_THRESHOLD_NANOS = 100ULL * 1000000000ULL;

// Полный адрес метода Bot API (WHALE_TG_API, по умолчанию api.telegram.org).
std::string tgApi(const std::string& method);

struct SendResult { bool ok; bool deadUser; int retryAfterSec; };
SendResult sendMsg(const std::string& chatId, const std::string& text,
                   const std::string& reply_markup = "");

// Клавиатура из одной кнопки, открывающей приложение (WHALE_MINIAPP_URL).
// Пустая строка, если адрес приложения не задан.
std::string openAppKeyboard(Lang lang);

std::string getUserLanguage(const std::string& chatId);
void ensureUser(const std::string& chatId, const std::string& tgLangCode = "");
void refreshWatchers();
