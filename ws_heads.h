#pragma once

#include <cstdint>
#include <string>

/** Только newHeads: номер блока. Цены и разбор сделок не трогает.
 *
 * Адреса по умолчанию заданы в ws_heads.cpp — здесь их нет намеренно,
 * чтобы список не разъезжался с кодом.
 * WHALE_WS_URL=url1,url2 — свой список (пусто = выключено).
 */

/** Поднять heads с дефолтами / env. */
void startWsBsc();
void stopWsBsc();

bool wsHeadsOk();
int64_t wsHeadsLatest();
std::string wsHeadsActiveLabel();

/** Смен/простоев/отказов подписки/отказов связи — через слэш. */
std::string wsHeadsStats();
