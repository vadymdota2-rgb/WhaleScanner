#pragma once

#include <cstdint>
#include <string>

/** Только newHeads: номер блока. Цены/analyze не трогает. */
void startWsHeads(const std::string& wssUrl);
void stopWsHeads();

/** true, если за последние ~30 с приходил head с WS. */
bool wsHeadsOk();

/** Последний номер блока с WS (0 если ещё не было). */
int64_t wsHeadsLatest();
