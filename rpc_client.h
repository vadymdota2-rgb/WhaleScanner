#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>

#include "json.hpp"

using json = nlohmann::json;

extern std::atomic<bool> running;

extern std::vector<std::string> RPC_ENDPOINTS;
extern std::atomic<size_t> rpcIndex;

void setRpcEndpoints(const std::vector<std::string>& endpoints);

std::string http(const std::string& url, const std::string& post = "", int timeout = 10);

json rpc(const std::string& method, json params, int maxRetries = 3);

json rpcOnEndpoint(size_t idx, const std::string& method, json params);

json rpcSpread(size_t seed, const std::string& method, json params);

void initEndpointHealth();
bool endpointUsable(size_t idx);
void reportEndpoint(size_t idx, bool ok);
size_t usableEndpointFrom(size_t idx);

void setRpcFailureHandler(std::function<void()> handler);
void setRpcGiveUpHandler(std::function<void()> handler);

std::string rpcSlowSummary();
