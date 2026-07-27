#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ======================== Модуль перпетуалов Hyperliquid ========================
// Hyperliquid - собственный L1, а не EVM-сеть: блоков для сканирования нет,
// узлов RPC нет, логов для разбора нет. Поэтому модуль не использует ни
// анализатор транзакций, ни слой rpc_client - данные приходят готовыми через
// постоянное соединение с площадкой.
//
// Своего списка кошельков у модуля НЕТ. Он общий со спотом: пользователь
// добавляет кита один раз в "Моём аккаунте", и если тот торгует и на BSC, и на
// перпах - получает алерты по обоим. Адреса у площадки ethereum-совместимые,
// так что один и тот же 0x... годится обеим сторонам. Отсюда же бесплатно
// достаются лимиты премиума и пороги алертов - они уже посчитаны в WATCHERS_PTR.
//
// Своя база (hyperliquid.db) остаётся, но только под сделки и служебное
// состояние: главная уже обслуживает четыре потока, добавлять к ним поток
// записи перпов значит плодить конкуренцию за блокировки.
//
// Модуль поднимает ДВА потока:
//   1. фид - держит соединение, слушает сделки всех рынков, отбирает свои
//      адреса по бесплатному признаку (нотионал) и ставит их в очередь;
//   2. дозагрузчик - разбирает очередь и добирает по каждому кошельку полную
//      картину (готовый PnL, направление, плечо, маржа) через REST.
// Разделение обязательно: HTTP-запрос занимает сотни миллисекунд, а фид всё
// это время должен продолжать вычитываться, иначе буфер переполнится.
//
// Чтобы отключить перпы целиком: убрать hyperliquid.cpp из сборки и четыре
// вызова в main.cpp (init / start / stop / строка статистики).

// --- Сервисы main.cpp, используемые модулем (определены в main.cpp) ---

// Снимок отслеживаемых адресов в нижнем регистре. Модуль фильтрует по нему
// поток сделок площадки, поэтому берёт снимок редко и держит копию у себя:
// сверка идёт на каждую сделку, а их у площадки тысячи в минуту.
std::vector<std::string> hlWatchedAddresses();

// Получатели алертов по адресу. Структура своя, а не Watcher из main.cpp:
// определять Watcher в двух местах значило бы получить два расходящихся
// описания одного и того же при первой же правке полей.
struct HlRecipient {
    std::string chatId;
    std::string label;
    uint64_t thresholdNanos = 0;
};
std::vector<HlRecipient> hlWatchersFor(const std::string& addressLower);

// --- Жизненный цикл ---

// Открывает свою базу и создаёт схему. Вызывать один раз при старте.
bool initHyperliquid();

// Запускает оба фоновых потока.
void startHyperliquidLoop();

// Останавливает потоки и закрывает базу.
void stopHyperliquid();

// --- Экраны ---
// Своя структура вместо TelegramUI::UIMessage - по образцу ranking.h: модулю
// не нужен весь alert_settings.h в заголовке ради двух строк.
struct HlMessage {
    std::string text;
    std::string keyboard;
};

// Развилка при нажатии "Топ трейдеров": спот на BSC или перпы Hyperliquid.
HlMessage buildVenueMenu(const std::string& chatId);
// Меню рейтинга перпов: PnL / ROI / винрейт / активность.
HlMessage buildPerpTopMenu(const std::string& chatId);

// --- Диспетчеризация ---
// Отрисовка представления по action/param (нужна для навигации "назад").
// false - действие не наше.
bool renderHyperliquidView(const std::string& chatId, const std::string& action,
                           const std::string& param, HlMessage& out);

// Обработка callback'ов модуля: hl_menu, hl_open, hl_page. true - обработано.
bool handleHyperliquidCallback(const std::string& chatId, const std::string& action,
                               const std::string& param, const std::string& data,
                               long long messageId, const std::string& callbackQueryId);

// Дневной дайджест по перпам для канала. Пустая строка - данных пока нет.
std::string buildHlDailyDigest();

// --- Статистика для /stats ---
std::string hyperliquidStatsLine();
