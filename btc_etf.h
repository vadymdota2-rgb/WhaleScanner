#pragma once

/*
 * Биржевые фонды (ETF) в сети Bitcoin — своя разметка и свои потоки, по
 * цепочке, без чужих сервисов.
 *
 * С чего начинаем: кошельки, которые фонд публикует сам (btc_etf_book.h).
 * Сейчас так делает Bitwise (BITB) — 65 кошельков, сумма их остатков равна
 * запасу фонда.
 *
 * Как устроены траты фонда (видно по его кошелькам за два года): хранитель
 * присылает на кошелёк пылинку на комиссию, кошелёк тратится, сдача почти
 * всегда — на тот же адрес (199 трат из 260) или на уже известный кошелёк
 * фонда (57), пылинка возвращается в кошелёк комиссий хранителя. Клиентов в
 * одной трате хранитель не смешивает. Отсюда правила:
 *   • совместная трата: вход рядом с кошельком фонда — тоже кошелёк фонда;
 *   • приток — монеты извне на кошелёк фонда, отток — с кошельков фонда
 *     наружу; пыль (меньше 0,01 BTC) — механика комиссий хранителя, не поток;
 *   • отток на адрес, который потом тратится вместе с кошельком фонда, был
 *     перекладкой внутри фонда: строка оттока снимается задним числом;
 *   • плата фонду за управление (expense ratio / 12 в конце месяца) — не
 *     отток инвесторов, считается отдельно.
 * Запас фонда — сумма остатков его кошельков: ведётся по блокам и раз в
 * сутки сверяется с цепочкой (расхождение видно в /statsbtc).
 */

#include <sqlite3.h>

#include <functional>
#include <string>
#include <vector>

#include "btc_block.h"

// Все функции — под замком базы биткоина, на её соединении.
void etfOpen(sqlite3* db);
void etfBlock(sqlite3* db, const Block& b);
void etfRollback(sqlite3* db, long long height);
void etfCleanup(sqlite3* db, long long keepFrom);
// Кошелёк фонда — тикер, иначе пусто. Можно звать без замка базы.
std::string etfFund(const std::string& address);
// Все кошельки фондов — для догонки по истории адресов.
std::vector<std::string> etfAddresses();
// Кошельки, чей остаток пора сверить с цепочкой (новые — сразу, остальные
// раз в сутки). Сверка: остаток из цепочки на высоте `height`.
std::vector<std::string> etfStale(sqlite3* db, size_t limit);
void etfSetBalance(sqlite3* db, const std::string& address, long long sats, long long height);
// Хранилища хранителя, чью историю пора дочитать (с mempool.space): приток в
// хранилище бывает раньше, чем хранитель пришлёт ему первую пылинку.
std::vector<std::string> etfVaultsToRead(size_t limit);
void etfVaultTx(sqlite3* db, long long height, long long ts, const BTx& tx);
void etfVaultRead(sqlite3* db, const std::string& address);
// Не прочиталась (оба источника молчат) — в конец очереди, на следующий круг.
void etfVaultRetry(const std::string& address);
// Новый ли адрес: 1 — до траты `txid` (высота `height`) в цепочке его не было,
// 0 — был, -1 — не узнать (тогда не переезд, а отток). Ставит btc_chain.
using EtfFreshCheck = std::function<int(const std::string& address, const std::string& txid, long long height)>;
void etfSetFreshCheck(EtfFreshCheck f);
// Разметка владельца (/import): адреса → фонд. В очередь (её переносит в
// разметку процесс, читающий блоки, — etfDrainLabels с каждым блоком).
// Опубликованные фондом (BITB) не переписываются.
struct EtfImportResult {
    int queued = 0;  // в очереди на разметку
    int dup = 0;     // уже этого фонда
    int moved = 0;   // были другого фонда — переразмечены (входят в queued)
    int named = 0;   // были без имени (группа «#…») — помечены (входят в queued)
    int inBase = 0;  // лежали в базе китов без метки фонда — помечены (входят в queued)
    int seed = 0;    // опубликованы фондом — не тронуты
};
EtfImportResult etfImport(sqlite3* db, const std::vector<std::string>& addrs, const std::string& fund);
int etfDrainLabels(sqlite3* db);
// Размеченные, чья история за два месяца ещё не прочитана, — догонке.
std::vector<std::string> etfHistQueue(sqlite3* db, size_t limit);
void etfHistDone(sqlite3* db, const std::vector<std::string>& addrs);
// Раздел для /statsbtc.
std::string etfStats(sqlite3* db);
