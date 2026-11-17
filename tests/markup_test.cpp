// Кнопка «Открыть приложение» (web_app) — только в личном чате: в группе и
// канале Telegram отклоняет всё сообщение (BUTTON_TYPE_INVALID).
#include <cassert>
#include <iostream>
#include "../telegram.h"

int main() {
    const std::string app = R"({"inline_keyboard":[[{"text":"Open","web_app":{"url":"https://x"}}]]})";
    const std::string link = R"({"inline_keyboard":[[{"text":"Tx","url":"https://x"}]]})";
    assert(markupForChat("123456", app) == app);         // личный чат — с кнопкой
    assert(markupForChat("-100123456", app).empty());    // супергруппа и канал — без неё
    assert(markupForChat("-123456", app).empty());       // группа — без неё
    assert(markupForChat("-100123456", link) == link);   // обычная ссылка в группе можно
    assert(markupForChat("-100123456", "").empty());
    assert(markupForChat("", app) == app);
    std::cout << "markup: ok" << std::endl;
    return 0;
}
