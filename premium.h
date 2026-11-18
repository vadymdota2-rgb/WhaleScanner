// © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
#pragma once

/*
 * Премиум: выдача подписки после оплаты — в одном месте.
 *
 * Купить Премиум можно только в приложении: счёт в звёздах создаёт API
 * (whale_api.py, createInvoiceLink), счёт в USD₮ он же кладёт в таблицу
 * ton_invoices. Бот подтверждает оплату звёздами, видит приход USD₮ и
 * продлевает срок; кончился срок — снимает отметку и пишет человеку.
 *
 * Спецификация (инварианты, состояния счёта, модель угроз, конкурентность,
 * отказы и что делать владельцу) — docs/premium.md. Правила — чистыми
 * функциями в premium_logic.h.
 *
 * Потоки: все функции можно звать из любого потока бота — база под dbMutex.
 * pollUsdtPayments зовётся из одного потока (главный цикл).
 */

#include <cstddef>
#include <set>
#include <string>
#include <vector>
#include "json.hpp"

/** Схема оплаты. serviceChatId — служебный аккаунт (Премиума у него не
 *  бывает), ownerChatId — кому писать, если деньги пришли, а выдать Премиум
 *  не вышло. false — оплата выключена до исправления. Зовётся один раз при
 *  запуске, до потоков бота. */
[[nodiscard]] bool initPremium(const std::string& serviceChatId = "", const std::string& ownerChatId = "");

/** Действует ли Премиум сейчас. Служебный аккаунт — никогда. */
[[nodiscard]] bool isPremium(const std::string& chatId);

/** Кто из списка — подписчики (для рассылки алертов). Сбой базы — пустой
 *  ответ: алерт уйдёт всем как бесплатным, но уйдёт. */
[[nodiscard]] std::set<std::string> premiumSubsetOf(const std::vector<std::string>& chatIds);

/** Истёкшим — снять отметку и написать. Пачками по 1000, письма — не чаще
 *  4 в секунду (лимит Telegram — на весь бот): при массовом окончании проход
 *  длится долго, поэтому зовётся в отдельном потоке (раз в 30 минут из
 *  главного цикла). Второй вызов, пока идёт первый, сразу возвращается. */
void cleanupExpiredPremium();

/** Опрос переводов USD₮ по счетам из приложения (раз в 20 секунд из
 *  главного цикла): занять счёт и продлить срок одной транзакцией. */
void pollUsdtPayments();

/** Прибавить дни: ручная выдача владельцем (/grant → grantCommand).
 *  Бонусы и приглашения прибавляет API сам (whale_api.py, extend_premium). */
[[nodiscard]] bool grantPremiumDays(const std::string& chatId, int days);

/** Отменить автопродление у всех, кто когда-то оформил подписку звёздами:
 *  звёзды больше не списываются сами. Каждому — сообщение, владельцу
 *  (reportTo) — отчёт; sayNone=false — молчать, если отменять нечего (так
 *  зовётся при каждом запуске бота). Возвращает, сколько подписок отменено. */
int cancelStarSubscriptions(const std::string& reportTo, bool sayNone = true);

/** Команда владельца «/grant <chat_id> <дней>»: выдать Премиум вручную.
 *  Возвращает ответ владельцу. Звать только для владельца (main.cpp). */
[[nodiscard]] std::string grantCommand(const std::string& args);

/** Сколько кошельков можно добавить: подписчику больше. */
[[nodiscard]] size_t premiumMaxWallets(const std::string& chatId);

/** Ответ Telegram на «можно ли платить» — за 10 секунд, иначе он отменит. */
void handlePreCheckoutQuery(const nlohmann::json& preCheckoutQuery);

/** Оплата звёздами прошла. true — записана (в том числе повтор). */
bool handleSuccessfulPayment(const std::string& chatId, const nlohmann::json& successfulPayment);

namespace premium {
/** Часы модуля: секунды Unix. Подменяются только в проверках (границы
 *  сроков до секунды); nullptr — системные. */
using Clock = long long (*)();
void setClock(Clock clock);
/** Пауза между сообщениями людям (лимит Telegram): миллисекунды. В
 *  проверках — без ожидания; nullptr — настоящая пауза. */
using Pause = void (*)(int ms);
void setPause(Pause pause);
}  // namespace premium
