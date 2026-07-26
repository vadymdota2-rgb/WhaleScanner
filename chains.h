#pragma once

#include <string>
#include "tx_analyzer.h"

// ============================ Конфигурации сетей ============================
// Всё, что известно про конкретную сеть, описано здесь и только здесь:
// обёрнутый нативный токен, стейблкоины, роутеры DEX, мосты, инфраструктура
// singleton-площадок и адреса узлов. Анализатор остаётся универсальным и
// работает с любой сетью через ChainContext, ничего про неё не зная.
//
// Чтобы добавить сеть: написать make<Name>Context() и одну строку в
// chainConfigByName(). Больше ничего править не нужно.

ChainContext makeBscContext();
ChainContext makeEthereumContext();
ChainContext makeBaseContext();
ChainContext makeArbitrumContext();

// Подбор по имени: "bsc", "ethereum"/"eth", "base", "arbitrum"/"arb".
// false - имя неизвестно.
bool chainConfigByName(const std::string& name, ChainContext& out);
