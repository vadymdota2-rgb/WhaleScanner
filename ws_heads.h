#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

// ─── Старт / стоп (дефолты и роли — здесь, не в main) ───────────────────────
//
// heads primary  → wss://rpc-bsc.blockmachine.io
// heads backup   → wss://bnb.api.onfinality.io/public-ws
// assist getBlock→ wss://rpc.nodeflare.app/bnb/ws/public  (только lag > 200)
//
// WHALE_WS_URL=url1,url2     — свой список heads (пусто = выкл heads)
// WHALE_WS_ASSIST_URL=url    — свой assist (пусто = выкл assist)

/** Поднять heads + assist с дефолтами / env. */
void startWsBsc();
void stopWsBsc();

// ─── Heads: только номер блока ──────────────────────────────────────────────

bool wsHeadsOk();
int64_t wsHeadsLatest();
std::string wsHeadsActiveLabel();

// ─── Assist: full getBlock при высоком лаге ─────────────────────────────────

bool wsAssistOk();
uint64_t wsAssistBlocksOk();

/** true, если lag выше порога и assist готов — можно звать getBlock. */
bool wsAssistWanted(int64_t lagBlocks);

/** eth_getBlockByNumber(full). null при ошибке. */
nlohmann::json wsAssistGetBlock(long long blockNumber, int timeoutMs = 8000);

// порог лага для assist (main может показать в stats)
constexpr int64_t WS_ASSIST_LAG_THRESHOLD = 200;
