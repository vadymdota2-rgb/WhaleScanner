#pragma once

#include <string>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include "ru.h"

namespace TelegramUI {

struct UIMessage {
    std::string text;
    std::string keyboard;
};

UIMessage buildMainMenu(const std::string& chatId);
std::string buildCancelButton(Lang lang = Lang::EN);
// Вариант с быстрым переходом в Топ трейдеров - для экрана ввода адреса.
std::string buildCancelWithTopTraders(Lang lang = Lang::EN);
std::string buildCancelWithSpotTop(Lang lang = Lang::EN);
UIMessage buildAlertThresholdMenu(uint64_t currentThresholdNanos, Lang lang = Lang::EN);

}

// Порог алертов по умолчанию - домен этого модуля.
constexpr uint64_t DEFAULT_THRESHOLD_NANOS = 100ULL * 1000000000ULL;

struct SendResult { bool ok; bool deadUser; int retryAfterSec; };

SendResult sendMsg(const std::string& chatId, const std::string& text,
                   const std::string& reply_markup = "");
void replyInPlace(const std::string& chatId, long long messageId,
                  const std::string& text, const std::string& keyboard = "");
void deleteMsg(const std::string& chatId, long long messageId);

bool setUserThresholdNanos(const std::string& chatId, uint64_t nanos);
uint64_t getUserThresholdNanos(const std::string& chatId);
void refreshWatchers();

enum class UserState {
    IDLE,
    AWAITING_WALLET_ADDRESS,
    AWAITING_WALLET_NAME,
    AWAITING_RENAME,
    AWAITING_TRACK_NAME,
    AWAITING_CUSTOM_THRESHOLD,
    AWAITING_TOPTRADER_TOKEN,
    AWAITING_HOLD_ADDRESS
};

struct UserSession {
    UserState state = UserState::IDLE;
    std::string pendingAddress;
    // id сообщения-приглашения ("введите адрес..."), чтобы удалить его после
    // завершения диалога и не оставлять в чате второе висящее меню.
    long long promptMessageId = 0;
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
                  long long promptMessageId = 0) {
        std::lock_guard<std::mutex> l(mtx);
        sessions[chatId] = UserSession{state, pendingAddress, promptMessageId};
    }

    void clearSession(const std::string& chatId) {
        std::lock_guard<std::mutex> l(mtx);
        sessions.erase(chatId);
    }
};

extern UserSessionManager g_sessionManager;

bool handleThresholdCallback(const std::string& chatId, const std::string& param, long long messageId);
bool handleThresholdText(const std::string& chatId, const std::string& text);
