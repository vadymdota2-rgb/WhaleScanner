#pragma once
// ==================== ALERT SETTINGS MODULE ====================
// Вся логика настройки порога алертов (меню, пресеты, кастомный ввод)
// вынесена сюда из main.cpp.
//
// ВАЖНО: следующие типы теперь определены ЗДЕСЬ и должны быть УДАЛЕНЫ
// из main.cpp (иначе будет redefinition):
//   - TelegramUI::UIMessage
//   - struct SendResult
//   - enum class UserState, struct UserSession, class UserSessionManager
// Определения функций (sendMsg, replyInPlace, buildMainMenu и т.д.)
// остаются в main.cpp — здесь только объявления.

#include <string>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// ---------- Общие типы (перенесены из main.cpp без изменений) ----------

namespace TelegramUI {

struct UIMessage {
    std::string text;
    std::string keyboard;
};

// Определены в main.cpp
UIMessage buildMainMenu(const std::string& chatId);
std::string buildCancelButton();

// ===== Alert Settings: меню порога (реализация в alert_settings.cpp) =====
UIMessage buildAlertThresholdMenu(uint64_t currentThresholdNanos);

} // namespace TelegramUI

struct SendResult { bool ok; bool deadUser; int retryAfterSec; };

// Определены в main.cpp (у определений убрать дефолтные аргументы —
// они объявлены здесь, повторять их в .cpp нельзя).
SendResult sendMsg(const std::string& chatId, const std::string& text,
                   const std::string& reply_markup = "");
void replyInPlace(const std::string& chatId, long long messageId,
                  const std::string& text, const std::string& keyboard = "");

void setUserThreshold(const std::string& chatId, double usd);
void refreshWatchers();
double nanosToUsd(uint64_t nanos);
std::string formatThousands(uint64_t v);
std::string trim(const std::string& s);

// ---------- Состояние пользовательского ввода (перенесено из main.cpp) ----------

enum class UserState {
    IDLE,
    AWAITING_WALLET_ADDRESS,
    AWAITING_WALLET_NAME,
    AWAITING_RENAME,
    AWAITING_CUSTOM_THRESHOLD,
    AWAITING_TOPTRADER_TOKEN
};

struct UserSession {
    UserState state = UserState::IDLE;
    std::string pendingAddress;
    std::string pendingLabel;
};

class UserSessionManager {
    std::mutex mtx;
    std::unordered_map<std::string, UserSession> sessions;
public:
    UserSession getSession(const std::string& chatId) {
        std::lock_guard<std::mutex> l(mtx);
        auto it = sessions.find(chatId);
        return it != sessions.end() ? it->second : UserSession{};
    }

    void setState(const std::string& chatId, UserState state,
                  const std::string& pendingAddress = "",
                  const std::string& pendingLabel = "") {
        std::lock_guard<std::mutex> l(mtx);
        sessions[chatId] = UserSession{state, pendingAddress, pendingLabel};
    }

    void clearSession(const std::string& chatId) {
        std::lock_guard<std::mutex> l(mtx);
        sessions.erase(chatId);
    }
};

// Экземпляр определён в main.cpp: `UserSessionManager g_sessionManager;`
extern UserSessionManager g_sessionManager;

// ---------- Точки входа модуля ----------

// Обрабатывает callback_data вида "threshold:<param>"
// (param: "100", "500", "1000", "5000", "10000", "50000", "custom").
// Возвращает true, если callback обработан.
bool handleThresholdCallback(const std::string& chatId,
                             const std::string& param,
                             long long messageId);

// Обрабатывает текстовый ввод в состоянии AWAITING_CUSTOM_THRESHOLD.
// Возвращает true, если ввод обработан (в т.ч. с сообщением об ошибке).
bool handleThresholdText(const std::string& chatId,
                         const std::string& text);
