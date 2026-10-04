#pragma once

/*
 * Что осталось от бота в Telegram. Меню, кнопок и диалогов в чате больше
 * нет — всё это в приложении. Бот отвечает на любое сообщение кнопкой
 * «Открыть приложение», принимает оплату звёздами, рассылает алерты и
 * отвечает владельцу на служебные команды.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include "ru.h"

// Бесплатный тариф: алерты только с основного кошелька (порядок — основной, потом по дате
// добавления). Тот же порядок и то же число — FREE_ALERT_WALLETS в API.
constexpr size_t FREE_ALERT_WALLETS = 1;

// Порог алерта новому человеку; дальше его меняют в приложении.
constexpr uint64_t DEFAULT_THRESHOLD_NANOS = 100ULL * 1000000000ULL;

// Полный адрес метода Bot API (WHALE_TG_API, по умолчанию api.telegram.org).
std::string tgApi(const std::string& method);

struct SendResult { bool ok; bool deadUser; int retryAfterSec; };
SendResult sendMsg(const std::string& chatId, const std::string& text,
                   const std::string& reply_markup = "");

// Клавиатура из одной кнопки, открывающей приложение (WHALE_MINIAPP_URL).
// Пустая строка, если адрес приложения не задан. `go` — куда приложению
// открыться сразу («premium-intro»: экран Премиума, пришли за скидкой);
// `btn` — ключ подписи кнопки. Письмо про скидку должно вести к скидке, а
// не на главный экран, где её ещё надо искать.
std::string openAppKeyboard(Lang lang, const std::string& go = "", const char* btn = "menu_open_app");

std::string getUserLanguage(const std::string& chatId);
void ensureUser(const std::string& chatId, const std::string& tgLangCode = "");
void refreshWatchers();
