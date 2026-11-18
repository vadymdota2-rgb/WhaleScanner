#pragma once

/*
 * Правила оплаты без базы и сети: тарифы, вводная цена, срок, приём
 * перевода USD₮. Всё, что решает «выдать подписку или нет», живёт здесь —
 * чистыми функциями, которые тесты (tests/premium_test.cpp) проверяют на
 * тысячах случайных входов. premium.cpp только читает и пишет базу.
 */

#include <array>
#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include "json.hpp"

namespace premium {

/* Тарифы в звёздах. Счёт выставляет API (whale_api.py, STAR_PLANS) — там те
   же полезная нагрузка, цена и срок; здесь их сверяют перед выдачей.

   Все тарифы — разовые: автопродления (подписки Telegram) нет, продлевать
   или нет решает человек. Подписки, оформленные раньше, бот отменяет сам
   (cancelStarSubscriptions); списание, успевшее прийти, засчитывается как
   обычная оплата. Год — разовая оплата со скидкой. Вводная цена
   месяца — предложение тем, у кого только что кончилась пробная неделя;
   кому его показывать, решает API, бот лишь узнаёт нагрузку. */
struct StarPlan { const char* payload; long long stars; int days; };
inline constexpr StarPlan STAR_PLANS[] = {
    {"premium_30_days", 250, 30},
    {"premium_365_days", 1990, 365},
    {"premium_30_intro", 150, 30},
};
inline constexpr const char* INTRO_PAYLOAD = "premium_30_intro";
// Дольше десяти лет подписку не выдаём ни за что: такой срок — ошибка.
inline constexpr int MAX_GRANT_DAYS = 3650;

// Нагрузка бывает с хвостом после «:» — у вводной цены там покупатель и
// срок (introBinding). Тариф определяет начало.
inline const StarPlan* starPlan(const std::string& payload) {
    const std::string base = payload.substr(0, payload.find(':'));
    for (const auto& p : STAR_PLANS)
        if (base == p.payload) return &p;
    return nullptr;
}

// «{n}» в тексте — число дней тарифа: месяц и год пишутся одной строкой.
inline std::string withDays(std::string text, int days) {
    const size_t at = text.find("{n}");
    if (at != std::string::npos) text.replace(at, 3, std::to_string(days));
    return text;
}

// Целое из одних цифр (у номера чата — ещё «-» впереди), без пробелов,
// плюсов и хвостов, которые std::stoll молча пропускает: «123abc» — не 123.
// Больше 18 цифр не бывает ни у суммы, ни у срока — и переполнения нет.
inline bool strictInt(const std::string& s, long long& out, bool allowMinus = false) {
    size_t i = 0;
    bool neg = false;
    if (allowMinus && !s.empty() && s[0] == '-') { neg = true; i = 1; }
    if (i == s.size() || s.size() - i > 18) return false;
    long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    out = neg ? -v : v;
    return true;
}

// Целое поле ответа Telegram; дробь, строка или нет поля — -1 (ни с одной
// ценой не совпадёт). value(key, 0) тут не годится: 250.9 он молча режет до
// 250, а с int по умолчанию 4294967546 становится 250.
inline long long intField(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return -1;
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return -1;
    if (it->is_number_unsigned() && it->get<unsigned long long>() > static_cast<unsigned long long>(LLONG_MAX))
        return -1;
    return it->get<long long>();
}

/* Вводная цена привязана к человеку и к сроку: «premium_30_intro:<chat>:<до>».
   Ссылку на счёт можно сохранить или переслать, а Telegram её не гасит —
   без привязки по ней платили бы 150 звёзд вместо 250 кто угодно и сколько
   угодно раз. Скидка — только тому, кому её выдал API, и только в её окно
   (плюс INTRO_GRACE_SEC: счёт могли открыть за минуту до конца). */
inline constexpr long long INTRO_GRACE_SEC = 900;
inline bool introBinding(const std::string& payload, std::string& chat, long long& until) {
    const size_t a = payload.find(':');
    if (a == std::string::npos) return false;
    const size_t b = payload.find(':', a + 1);
    if (b == std::string::npos) return false;
    long long id = 0;
    chat = payload.substr(a + 1, b - a - 1);
    if (!strictInt(chat, id, true) || !strictInt(payload.substr(b + 1), until)) return false;
    return until > 0;
}

// Можно ли принять счёт в звёздах: тариф известен, валюта и сумма — его, а
// вводная цена — только своему покупателю и в её окно.
enum class Checkout { Ok, Unknown, IntroGone };
inline Checkout checkStars(const std::string& payload, const std::string& currency, long long amount,
                           const std::string& buyer, long long now) {
    const StarPlan* plan = starPlan(payload);
    if (!plan || currency != "XTR" || amount != plan->stars) return Checkout::Unknown;
    if (std::string(plan->payload) != INTRO_PAYLOAD) return Checkout::Ok;
    std::string chat;
    long long until = 0;
    if (!introBinding(payload, chat, until) || buyer.empty() || chat != buyer ||
        now > until + INTRO_GRACE_SEC)
        return Checkout::IntroGone;
    return Checkout::Ok;
}

/* Новый срок после оплаты на `days` дней. Действующий премиум продлевается с
   его конца (заплатил заранее — ничего не теряет), кончившийся — с текущей
   минуты. Начало действующего не меняется. Срок, который не помещается в
   число, и дни вне 1…MAX_GRANT_DAYS — отказ: лучше не выдать, чем выдать
   «навсегда» или в прошлое. */
struct Term { long long start; long long expire; bool wasActive; };
inline std::optional<Term> extendTerm(int flag, long long start, long long expire, long long now, int days) {
    if (days <= 0 || days > MAX_GRANT_DAYS || now <= 0) return std::nullopt;
    const bool active = flag != 0 && expire > now;
    const long long base = active ? expire : now;
    const long long add = static_cast<long long>(days) * 86400LL;
    if (base > LLONG_MAX - add) return std::nullopt;
    return Term{active && start > 0 ? start : now, base + add, active};
}

// USD₮ в сети TON: адрес мастер-контракта в сыром виде и шесть знаков после
// запятой. Без сверки с ним подписку можно было бы купить за любой жетон,
// нарисованный кем угодно.
inline constexpr const char* USDT_MASTER =
    "0:B113A994B5024A16719F69139328EB759596C38A25F59028B146FECDC3621DFE";
inline constexpr long long USDT_MIN_UNITS = 1000000;  // доллар — ниже счёт не бывает

// Памятка счёта как её ищет база: без пробелов и переводов строки, заглавными
// (человек мог перебить её руками). Памятки бывают «WT-XXXXX» — длиннее 64
// знаков сравнивать незачем.
inline std::string memoKey(const std::string& memo) {
    std::string out;
    for (char c : memo) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        out += static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        if (out.size() > 64) return "";
    }
    return out;
}

// Сумма USD₮ для человека: «3.99» — до цента, вниз (шесть знаков после
// запятой в сети, нам хватает двух).
inline std::string usdText(long long units) {
    if (units < 0) units = 0;
    const long long cents = units / 10000;
    const long long c = cents % 100;
    return std::to_string(cents / 100) + "." + (c < 10 ? "0" : "") + std::to_string(c);
}

// Хватает ли денег: два процента запаса — на случай, если кошелёк округлил
// сумму вниз. Так же, как got·100 ≥ need·98, но без переполнения.
inline bool paidEnough(long long got, long long need) {
    return need > 0 && got >= need - need / 50;
}

/* Адрес TON из человеческого вида (48 знаков base64, флаг, сеть, хеш и
   контрольная сумма) в сырой «0:HEX» — в нём обозреватель пишет получателя.
   Сырой вид принимается как есть (заглавными). Неверная контрольная сумма,
   длина или знак — пустая строка: такой адрес не совпадёт ни с чем. */
inline std::string tonRawAddress(const std::string& addr) {
    static const char* HEX = "0123456789ABCDEF";
    const size_t colon = addr.find(':');
    if (colon != std::string::npos) {
        long long wc = 0;
        if (!strictInt(addr.substr(0, colon), wc, true) || wc < -128 || wc > 127) return "";
        std::string hex = addr.substr(colon + 1);
        if (hex.size() != 64) return "";
        for (char& c : hex) {
            if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return "";
        }
        return std::to_string(wc) + ":" + hex;
    }
    if (addr.size() != 48) return "";
    std::array<uint8_t, 36> b{};
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (char c : addr) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else return "";
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            b[n++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
        }
    }
    // CRC-16/XMODEM первых 34 байт — последние два.
    uint16_t crc = 0;
    for (size_t i = 0; i < 34; ++i) {
        crc ^= static_cast<uint16_t>(b[i] << 8);
        for (int k = 0; k < 8; ++k)
            crc = static_cast<uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    if (crc != static_cast<uint16_t>((b[34] << 8) | b[35])) return "";
    if ((b[0] & 0x7F) != 0x11 && (b[0] & 0x7F) != 0x51) return "";
    std::string out = std::to_string(static_cast<int8_t>(b[1])) + ":";
    for (size_t i = 2; i < 34; ++i) {
        out += HEX[b[i] >> 4];
        out += HEX[b[i] & 15];
    }
    return out;
}

/* Перевод из ответа обозревателя (toncenter v3, jetton/transfers), годный к
   сверке со счётом: не сорвался, жетон — настоящий USD₮, пришёл на наш
   кошелёк (а не на чей-то ещё, если обозреватель вдруг не применил фильтр),
   сумма — целое из цифр не меньше доллара, памятка есть. Всё остальное —
   nullopt, без исключений на любом мусоре. */
struct UsdtTransfer { std::string memo; long long units; std::string hash; };
inline std::optional<UsdtTransfer> usdtTransfer(const nlohmann::json& jt, const std::string& walletRaw) {
    using nlohmann::json;
    auto str = [](const json& o, const char* key) -> std::string {
        if (!o.is_object()) return "";
        auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : "";
    };
    if (!jt.is_object() || walletRaw.empty()) return std::nullopt;
    auto ab = jt.find("transaction_aborted");
    if (ab == jt.end() || !ab->is_boolean() || ab->get<bool>()) return std::nullopt;
    if (tonRawAddress(str(jt, "jetton_master")) != USDT_MASTER) return std::nullopt;
    if (tonRawAddress(str(jt, "destination")) != walletRaw) return std::nullopt;
    long long units = 0;
    if (!strictInt(str(jt, "amount"), units) || units < USDT_MIN_UNITS) return std::nullopt;
    auto fp = jt.find("decoded_forward_payload");
    const std::string memo = fp == jt.end() ? "" : memoKey(str(*fp, "comment"));
    if (memo.empty()) return std::nullopt;
    return UsdtTransfer{memo, units, str(jt, "transaction_hash")};
}

}  // namespace premium
