#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>

#include "json.hpp"

using json = nlohmann::json;

// Флаг работы процесса живёт в main.cpp: слой запросов должен уметь прервать
// ожидание и повторы при остановке бота.
extern std::atomic<bool> running;

// ==================== Пул эндпоинтов (задаёт вызывающий) ===================
// Модуль не знает про конкретные сети: список узлов приходит снаружи, из
// конфигурации сети. Так добавление сети не требует правок здесь.


extern std::vector<std::string> RPC_ENDPOINTS;
extern std::atomic<size_t> rpcIndex;

// Выбор сети: заменяет пул и сбрасывает состояние здоровья узлов.
void setRpcEndpoints(const std::vector<std::string>& endpoints);

// ============================== Транспорт ==============================

// HTTP-запрос. Соединения постоянные (по одному на поток), поэтому повторные
// обращения к тому же хосту не требуют нового TLS-рукопожатия.
std::string http(const std::string& url, const std::string& post = "", int timeout = 10);

// ============================== Запросы ================================

// Основной вызов: повторы, ротация при отказе и уходе с медленного узла.
json rpc(const std::string& method, json params, int maxRetries = 3);

// Строго на указанный узел, без повторов - для сверки данных между узлами.
json rpcOnEndpoint(size_t idx, const std::string& method, json params);

// С распределением по узлам. Нужен для параллельной загрузки: если все потоки
// бьют в один эндпоинт, упираемся в его лимит задолго до нехватки времени.
json rpcSpread(size_t seed, const std::string& method, json params);

// ========================= Здоровье эндпоинтов =========================
// Список публичных узлов быстро устаревает. Вместо ручной проверки бот
// определяет живые сам: не ответивший подряд несколько раз временно
// исключается, а через паузу проверяется снова.

void initEndpointHealth();
bool endpointUsable(size_t idx);
void reportEndpoint(size_t idx, bool ok);
size_t usableEndpointFrom(size_t idx);

// =============================== Отчётность ============================

// Модуль не знает про общую структуру статистики - о сбоях он сообщает
// наружу через этот обработчик, который устанавливает main.cpp.
void setRpcFailureHandler(std::function<void()> handler);

// Сводка по медленным узлам для /stats. Пустая строка - жалоб не было.
std::string rpcSlowSummary();
