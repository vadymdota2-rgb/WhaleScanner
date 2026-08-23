#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

/** Только newHeads: номер блока. Цены/analyze не трогает.
 *  urls — primary, затем backup(ы). При обрыве/тишине крутит по кругу. */
void startWsHeads(const std::vector<std::string>& wssUrls);
/** Один URL или список через запятую. */
void startWsHeads(const std::string& wssUrlOrList);
void stopWsHeads();

/** true, если за последние ~30 с приходил head с WS. */
bool wsHeadsOk();

/** Последний номер блока с WS (0 если ещё не было). */
int64_t wsHeadsLatest();

/** Короткое имя активного endpoint для stats (primary/backup/…). */
std::string wsHeadsActiveLabel();

// ─── Assist WS: eth_getBlockByNumber при высоком лаге ───────────────────────

/** Третий сокет: JSON-RPC getBlock, чтобы разгрузить HTTP при lag > порога. */
void startWsAssist(const std::string& wssUrl);
void stopWsAssist();
bool wsAssistOk();

/** Запрос блока с full txs через assist-WS. null при ошибке/таймауте. */
nlohmann::json wsAssistGetBlock(long long blockNumber, int timeoutMs = 8000);

/** Сколько блоков успешно взяли через assist (для stats). */
uint64_t wsAssistBlocksOk();
