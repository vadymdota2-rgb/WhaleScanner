#pragma once

#include <cstdint>
#include <string>

/** Только newHeads: номер блока. Цены/analyze не трогает.
 *
 * primary  → wss://rpc-bsc.blockmachine.io
 * backup   → wss://bnb.api.onfinality.io/public-ws
 *
 * WHALE_WS_URL=url1,url2  — свой список (пусто = выкл)
 */

/** Поднять heads с дефолтами / env. */
void startWsBsc();
void stopWsBsc();

bool wsHeadsOk();
int64_t wsHeadsLatest();
std::string wsHeadsActiveLabel();

/** Смен/простоев/отказов подписки/отказов связи — через слэш. */
std::string wsHeadsStats();
