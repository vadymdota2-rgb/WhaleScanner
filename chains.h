#pragma once

#include <string>
#include "tx_analyzer.h"

ChainContext makeBscContext();
ChainContext makeEthereumContext();
ChainContext makeBaseContext();
ChainContext makeArbitrumContext();

bool chainConfigByName(const std::string& name, ChainContext& out);
