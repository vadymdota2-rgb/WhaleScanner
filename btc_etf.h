#pragma once

/*
 * Метки кошельков биржевых фондов (ETF) в сети Bitcoin — и только они.
 *
 * Откуда метки: кошельки, которые фонд публикует сам (btc_etf_book.h —
 * Bitwise BITB), и разметка владельца через /import (кнопка фонда или тикер
 * в строке CSV). Бот сам кошельки фондов не ищет и потоки фондов по цепочке
 * не считает: «Институциональные потоки» приложения берут их из открытого
 * набора TFTC.
 *
 * Зачем метки: кошелёк фонда — не кит и не биржа. Он не попадает в базу
 * китов поиском, не становится адресом биржи, а в списках сделок и в
 * рейтинге подписан тикером фонда (API читает btc_etf_addr).
 */

#include <sqlite3.h>

#include <string>
#include <utility>
#include <vector>

// Все функции, кроме etfFund, — под замком базы биткоина, на её соединении.
void etfOpen(sqlite3* db);
// Кошелёк фонда — тикер, иначе пусто. Можно звать без замка базы.
std::string etfFund(const std::string& address);
// Разметка владельца (/import): адреса → фонд. В очередь: её переносит в
// метки процесс, читающий блоки (etfDrainLabels — с каждым блоком), — у
// него карта меток в памяти. Опубликованные фондом (BITB) не переписываются.
struct EtfImportResult {
    int queued = 0;  // в очереди на разметку
    int dup = 0;     // уже этого фонда
    int moved = 0;   // были другого фонда — переразмечены (входят в queued)
    int inBase = 0;  // лежали в базе китов — помечены (входят в queued)
    int seed = 0;    // опубликованы фондом — не тронуты
    bool failed = false;  // не тикер фонда или запись не прошла — импорт откатить
};
EtfImportResult etfImport(sqlite3* db, const std::vector<std::string>& addrs, const std::string& fund);
// Очередь разметки владельца — в метки, внутри транзакции блока. applied —
// что легло (адрес, фонд); сколько — возвращается; -1 — запись не прошла
// (транзакцию откатить). Карту в памяти меняет etfRemember(applied) — после
// COMMIT: откатят блок, и карта не разойдётся с базой.
using EtfLabels = std::vector<std::pair<std::string, std::string>>;
int etfDrainLabels(sqlite3* db, EtfLabels& applied);
void etfRemember(const EtfLabels& applied);
// Раздел для /statsbtc.
std::string etfStats(sqlite3* db);
