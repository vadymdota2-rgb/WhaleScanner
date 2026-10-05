#pragma once

/*
 * Автопополнение базы китов сервисного аккаунта — общие правила для трёх
 * сетей: BSC (крупный вывод с биржи), Hyperliquid (крупная сделка и рейтинг
 * трейдеров), Bitcoin (крупный вывод с биржи).
 *
 *   • Владелец включает и выключает поиск по каждой сети отдельно:
 *     /autobase в боте. Выбор переживает перезапуск (таблица state).
 *   • Не больше autoLimit() новых кошельков в сутки (UTC) на сеть (500) — чтобы
 *     не нагружать систему: каждый кошелёк в базе — это слежка за ним.
 *   • Пороги — здесь же, одним местом: BSC и Hyperliquid в долларах,
 *     Bitcoin — в биткоинах (btcAutoMinBtc, его считает btc_chain.cpp).
 */

#include <string>

enum class AutoNet { BSC = 0, HL = 1, BTC = 2 };

// Прочитать включённость из базы и сколько добавлено сегодня (BSC и
// Hyperliquid — по user_whales; Bitcoin досчитывает свой модуль, autoSeed).
void initAutobase();
// Bitcoin живёт в своей базе: его модуль сообщает, сколько добавил сегодня.
void autoSeed(AutoNet n, int addedToday);

bool autoEnabled(AutoNet n);
// Можно ли добавить ещё один кошелёк сейчас: поиск включён и сегодня
// лимит не выбран.
bool autoRoom(AutoNet n);
// Кошелёк добавлен — учесть в сегодняшнем лимите.
void autoCounted(AutoNet n);
int autoToday(AutoNet n);
// Убраны из базы за бездействие (pruneAutoWallets) — учесть. Счётчик за
// сегодня и за всё время хранится в таблице state и переживает перезапуск.
void autoPruned(AutoNet n, int count);
int autoLimit(AutoNet n);

// Порог в долларах для BSC и Hyperliquid (WHALE_BSC_AUTO_MIN,
// WHALE_HL_AUTO_MIN; по умолчанию $10 тыс.).
double autoMinUsd(AutoNet n);
// Порог Bitcoin в биткоинах (WHALE_BTC_AUTO_MIN, по умолчанию 1).
double btcAutoMinBtc();

// /autobase — состояние; /autobase bsc|hl|btc|all on|off — включить/выключить.
void autobaseCommand(const std::string& owner, const std::string& arg);
