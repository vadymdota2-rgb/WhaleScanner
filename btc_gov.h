#pragma once

/*
 * Биткоины государств — по адресам, которые государство раскрыло само, и по
 * своей цепочке, без чужих списков.
 *
 * Откуда адреса:
 *   • Сальвадор — Национальный биткоин-офис публикует весь список своих
 *     кошельков (bitcoin.gob.sv, кошелёк ONBTC); бот перечитывает его дважды
 *     в сутки, новые адреса берёт сам;
 *   • США — адрес, на который 3 ноября 2020 г. ушли 69 370 BTC с кошелька
 *     1HQ3Go3ggs8pFnXuHVHRytPCq5fGG8Hbhx из иска «United States v.
 *     Approximately 69,370 Bitcoin» (дело Silk Road, конфискация завершена
 *     в 2023 г.): в тот день «Individual X» подписал согласие на конфискацию.
 * Совместная трата: вход, потраченный вместе с адресом государства, — тоже
 * его (подписывает один владелец).
 *
 * Остатки — сверкой с цепочкой: адресов десятки, а сверка надёжнее, чем вести
 * их по блокам (перестройка цепочки не задваивает). Адрес, который двигался
 * в блоке, сверяется заново сразу; остальные — раз в сутки. Снимок суток —
 * для «изменение за 7 дней».
 */

#include <sqlite3.h>

#include <string>
#include <vector>

#include "btc_block.h"

// Все функции — под замком базы биткоина, на её соединении.
void govOpen(sqlite3* db);
// Блок: адреса государств, которые в нём двигались, — на сверку; входы,
// потраченные вместе с ними, — тоже адреса государства.
void govBlock(sqlite3* db, const Block& b);
// Перестройка цепочки: всё — на сверку заново.
void govRollback(sqlite3* db);
// Список адресов из публикации государства (код страны ISO, адреса).
int govOfficial(sqlite3* db, const std::string& gov, const std::vector<std::string>& addrs);
// Адреса, чей остаток пора сверить; сверка — остаток на высоте `height`.
std::vector<std::string> govStale(sqlite3* db, size_t limit);
void govSetBalance(sqlite3* db, const std::string& address, long long sats, long long height);
// Снимок суток (самоочистка, раз в час) и старое — прочь.
void govSnapshot(sqlite3* db, long long now, long long keepFrom);
// Адрес биткоина по виду (base58 или bech32) — для чужих списков.
bool govAddrOk(const std::string& a);
// Раздел для /statsbtc.
std::string govStats(sqlite3* db);
