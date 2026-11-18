// © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
#pragma once

/*
 * Правила оплаты без базы и сети: тарифы, вводная цена, срок, приём
 * перевода USD₮, разбор адресов TON, чистка строк для журнала. Всё, что
 * решает «выдать подписку или нет», живёт здесь — чистыми функциями без
 * состояния, часов и ввода-вывода: время приходит параметром. Проверки —
 * tests/premium_test.cpp (примеры, свойства на случайных входах, база в
 * памяти) и tests/fuzz/premium_fuzz.cpp (libFuzzer). Спецификация модуля —
 * docs/premium.md. premium.cpp только читает и пишет базу и ходит в сеть.
 */

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
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
struct StarPlan {
    const char* payload;
    long long stars;
    int days;
};
inline constexpr std::array<StarPlan, 3> STAR_PLANS{{
    {"premium_30_days", 250, 30},
    {"premium_365_days", 1990, 365},
    {"premium_30_intro", 150, 30},
}};
inline constexpr std::string_view INTRO_PAYLOAD = "premium_30_intro";
// Дольше десяти лет подписку не выдаём ни за что: такой срок — ошибка.
inline constexpr int MAX_GRANT_DAYS = 3650;
inline constexpr long long DAY_SEC = 86400;

// Нагрузка бывает с хвостом после «:» — у вводной цены там покупатель и
// срок (introBinding). Тариф определяет начало.
[[nodiscard]] inline const StarPlan* starPlan(std::string_view payload) {
    const std::string_view base = payload.substr(0, payload.find(':'));
    for (const auto& p : STAR_PLANS)
        if (base == p.payload) return &p;
    return nullptr;
}

// «{n}» в тексте — число дней тарифа: месяц и год пишутся одной строкой.
[[nodiscard]] inline std::string withDays(std::string text, int days) {
    const size_t at = text.find("{n}");
    if (at != std::string::npos) text.replace(at, 3, std::to_string(days));
    return text;
}

// Целое из одних цифр (у номера чата — ещё «-» впереди), без пробелов,
// плюсов и хвостов, которые std::stoll молча пропускает: «123abc» — не 123.
// Больше 18 цифр не бывает ни у суммы, ни у срока — и переполнения нет.
[[nodiscard]] inline bool strictInt(std::string_view s, long long& out, bool allowMinus = false) {
    size_t i = 0;
    bool neg = false;
    if (allowMinus && !s.empty() && s[0] == '-') {
        neg = true;
        i = 1;
    }
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
[[nodiscard]] inline long long intField(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return -1;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return -1;
    if (it->is_number_unsigned() && it->get<unsigned long long>() > static_cast<unsigned long long>(LLONG_MAX))
        return -1;
    return it->get<long long>();
}

// Строковое поле объекта; нет поля, не строка или не объект — пусто.
// Без исключений на любом входе.
[[nodiscard]] inline std::string strField(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return "";
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

/* Вводная цена привязана к человеку и к сроку: «premium_30_intro:<chat>:<до>».
   Ссылку на счёт можно сохранить или переслать, а Telegram её не гасит —
   без привязки по ней платили бы 150 звёзд вместо 250 кто угодно и сколько
   угодно раз. Скидка — только тому, кому её выдал API, и только в её окно
   (плюс INTRO_GRACE_SEC: счёт могли открыть за минуту до конца). */
inline constexpr long long INTRO_GRACE_SEC = 900;
[[nodiscard]] inline bool introBinding(std::string_view payload, std::string& chat, long long& until) {
    const size_t a = payload.find(':');
    if (a == std::string_view::npos) return false;
    const size_t b = payload.find(':', a + 1);
    if (b == std::string_view::npos) return false;
    long long id = 0;
    chat = std::string(payload.substr(a + 1, b - a - 1));
    if (!strictInt(chat, id, true) || !strictInt(payload.substr(b + 1), until)) return false;
    return until > 0;
}

// Можно ли принять счёт в звёздах: тариф известен, валюта и сумма — его, а
// вводная цена — только своему покупателю и в её окно.
enum class Checkout : std::uint8_t { Ok, Unknown, IntroGone };
[[nodiscard]] inline Checkout checkStars(std::string_view payload, std::string_view currency, long long amount,
                                         std::string_view buyer, long long now) {
    const StarPlan* plan = starPlan(payload);
    if (plan == nullptr || currency != "XTR" || amount != plan->stars) return Checkout::Unknown;
    if (plan->payload != INTRO_PAYLOAD) return Checkout::Ok;
    std::string chat;
    long long until = 0;
    if (!introBinding(payload, chat, until) || buyer.empty() || chat != buyer || now > until + INTRO_GRACE_SEC)
        return Checkout::IntroGone;
    return Checkout::Ok;
}

/* Новый срок после оплаты на `days` дней. Действующий премиум продлевается с
   его конца (заплатил заранее — ничего не теряет), кончившийся — с текущей
   минуты. Начало действующего не меняется. Срок, который не помещается в
   число, и дни вне 1…MAX_GRANT_DAYS — отказ: лучше не выдать, чем выдать
   «навсегда» или в прошлое. */
struct Term {
    long long start;
    long long expire;
    bool wasActive;
};
[[nodiscard]] inline std::optional<Term> extendTerm(int flag, long long start, long long expire, long long now,
                                                    int days) {
    if (days <= 0 || days > MAX_GRANT_DAYS || now <= 0) return std::nullopt;
    const bool active = flag != 0 && expire > now;
    const long long base = active ? expire : now;
    const long long add = static_cast<long long>(days) * DAY_SEC;
    if (base > LLONG_MAX - add) return std::nullopt;
    return Term{active && start > 0 ? start : now, base + add, active};
}

// USD₮ в сети TON: адрес мастер-контракта в сыром виде и шесть знаков после
// запятой. Без сверки с ним подписку можно было бы купить за любой жетон,
// нарисованный кем угодно.
inline constexpr const char* USDT_MASTER = "0:B113A994B5024A16719F69139328EB759596C38A25F59028B146FECDC3621DFE";
inline constexpr long long USDT_MIN_UNITS = 1000000;  // доллар — ниже счёт не бывает

// Памятка счёта как её ищет база: без пробелов и переводов строки, заглавными
// (человек мог перебить её руками). Памятки бывают «WT-XXXXX» — длиннее 64
// знаков сравнивать незачем.
[[nodiscard]] inline std::string memoKey(std::string_view memo) {
    std::string out;
    for (const char c : memo) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        out += c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        if (out.size() > 64) return "";
    }
    return out;
}

// Сумма USD₮ для человека: «3.99» — до цента, вниз (шесть знаков после
// запятой в сети, нам хватает двух).
[[nodiscard]] inline std::string usdText(long long units) {
    if (units < 0) units = 0;
    const long long cents = units / 10000;
    const long long c = cents % 100;
    return std::to_string(cents / 100) + "." + (c < 10 ? "0" : "") + std::to_string(c);
}

/* Перевод относится к счёту, только если сделан после того, как счёт
   выставлен (запас — 10 минут на расхождение часов сервера и сети): старый
   перевод с совпавшей памяткой не оплачивает новый счёт. */
inline constexpr long long CLOCK_SKEW_SEC = 600;
[[nodiscard]] inline bool transferFitsInvoice(long long transferAt, long long invoiceCreated) {
    return transferAt >= invoiceCreated - CLOCK_SKEW_SEC;
}

/* Пора ли спрашивать обозреватель. Пока есть счёт моложе часа (столько API
   показывает его человеку) — каждый проход главного цикла (20 с): оплату
   ждут сейчас. Все живые счета старше — раз в 2 минуты: опоздавшая оплата
   редка, а запросов к toncenter в сутки по такому счёту — в 6 раз меньше. */
inline constexpr long long FRESH_INVOICE_SEC = 3600;
inline constexpr long long SLOW_POLL_SEC = 120;
[[nodiscard]] inline bool pollDue(long long now, long long newestInvoice, long long lastPoll) {
    return now - newestInvoice < FRESH_INVOICE_SEC || now - lastPoll >= SLOW_POLL_SEC;
}

// Хватает ли денег: два процента запаса — на случай, если кошелёк округлил
// сумму вниз. Так же, как got·100 ≥ need·98, но без переполнения.
[[nodiscard]] inline bool paidEnough(long long got, long long need) {
    return need > 0 && got >= need - need / 50;
}

/* Адрес TON из человеческого вида (48 знаков base64, флаг, сеть, хеш и
   контрольная сумма) в сырой «0:HEX» — в нём обозреватель пишет получателя.
   Сырой вид принимается как есть (заглавными). Неверная контрольная сумма,
   длина или знак — пустая строка: такой адрес не совпадёт ни с чем. */
[[nodiscard]] inline std::string tonRawAddress(std::string_view addr) {
    constexpr std::string_view HEX = "0123456789ABCDEF";
    const size_t colon = addr.find(':');
    if (colon != std::string_view::npos) {
        long long wc = 0;
        if (!strictInt(addr.substr(0, colon), wc, true) || wc < -128 || wc > 127) return "";
        std::string hex(addr.substr(colon + 1));
        if (hex.size() != 64) return "";
        for (char& c : hex) {
            if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
            const bool digit = c >= '0' && c <= '9';
            const bool letter = c >= 'A' && c <= 'F';
            if (!digit && !letter) return "";
        }
        return std::to_string(wc) + ":" + hex;
    }
    if (addr.size() != 48) return "";
    std::array<uint8_t, 36> b{};
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (const char c : addr) {
        uint32_t v = 0;
        if (c >= 'A' && c <= 'Z') v = static_cast<uint32_t>(c - 'A');
        else if (c >= 'a' && c <= 'z') v = static_cast<uint32_t>(c - 'a' + 26);
        else if (c >= '0' && c <= '9') v = static_cast<uint32_t>(c - '0' + 52);
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else return "";
        acc = (acc << 6U) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            b.at(n++) = static_cast<uint8_t>((acc >> static_cast<unsigned>(bits)) & 0xFFU);
        }
    }
    // CRC-16/XMODEM первых 34 байт — последние два.
    uint32_t crc = 0;
    for (size_t i = 0; i < 34; ++i) {
        crc ^= static_cast<uint32_t>(b.at(i)) << 8U;
        for (int k = 0; k < 8; ++k) crc = ((crc & 0x8000U) != 0 ? (crc << 1U) ^ 0x1021U : crc << 1U) & 0xFFFFU;
    }
    if (crc != ((static_cast<uint32_t>(b[34]) << 8U) | b[35])) return "";
    if ((b[0] & 0x7FU) != 0x11U && (b[0] & 0x7FU) != 0x51U) return "";
    std::string out = std::to_string(static_cast<int8_t>(b[1])) + ":";
    for (size_t i = 2; i < 34; ++i) {
        out += HEX[b.at(i) >> 4U];
        out += HEX[b.at(i) & 15U];
    }
    return out;
}

/* Перевод из ответа обозревателя (toncenter v3, jetton/transfers), годный к
   сверке со счётом: не сорвался, жетон — настоящий USD₮, пришёл на наш
   кошелёк (а не на чей-то ещё, если обозреватель вдруг не применил фильтр),
   сумма — целое из цифр не меньше доллара, памятка есть. Всё остальное —
   nullopt, без исключений на любом мусоре. */
struct UsdtTransfer {
    std::string memo;
    long long units;
    std::string hash;  // хеш транзакции: один перевод — один счёт
    long long at;      // время транзакции, Unix
};
[[nodiscard]] inline std::optional<UsdtTransfer> usdtTransfer(const nlohmann::json& jt, std::string_view walletRaw) {
    if (!jt.is_object() || walletRaw.empty()) return std::nullopt;
    const auto ab = jt.find("transaction_aborted");
    if (ab == jt.end() || !ab->is_boolean() || ab->get<bool>()) return std::nullopt;
    if (tonRawAddress(strField(jt, "jetton_master")) != USDT_MASTER) return std::nullopt;
    if (tonRawAddress(strField(jt, "destination")) != walletRaw) return std::nullopt;
    long long units = 0;
    if (!strictInt(strField(jt, "amount"), units) || units < USDT_MIN_UNITS) return std::nullopt;
    const auto fp = jt.find("decoded_forward_payload");
    const std::string memo = fp == jt.end() ? "" : memoKey(strField(*fp, "comment"));
    if (memo.empty()) return std::nullopt;
    // Без хеша перевод не отличить от его повтора; без времени — не сверить
    // со временем счёта.
    std::string hash = strField(jt, "transaction_hash");
    const long long at = intField(jt, "transaction_now");
    if (hash.empty() || hash.size() > 128 || at <= 0) return std::nullopt;
    return UsdtTransfer{memo, units, std::move(hash), at};
}

/* Команда владельца «/grant <chat_id> <дней>» — ручная выдача, когда деньги
   пришли, а бот выдать не смог (о таком он пишет владельцу сам). Номер —
   только человека (цифры, без «-»: группе Премиум не нужен), дни —
   1…MAX_GRANT_DAYS, между ними — пробелы. Остальное — nullopt. */
struct GrantArgs {
    std::string chat;
    int days;
};
[[nodiscard]] inline std::optional<GrantArgs> parseGrant(std::string_view args) {
    const auto skip = [&](size_t i) {
        while (i < args.size() && args[i] == ' ') ++i;
        return i;
    };
    const size_t a = skip(0);
    const size_t a_end = std::min(args.find(' ', a), args.size());
    const size_t b = skip(a_end);
    const size_t b_end = std::min(args.find(' ', b), args.size());
    if (a == a_end || b == b_end || skip(b_end) != args.size()) return std::nullopt;
    long long chat = 0;
    long long days = 0;
    if (!strictInt(args.substr(a, a_end - a), chat) || chat <= 0) return std::nullopt;
    if (!strictInt(args.substr(b, b_end - b), days) || days <= 0 || days > MAX_GRANT_DAYS) return std::nullopt;
    return GrantArgs{std::to_string(chat), static_cast<int>(days)};
}

/* Строка из чужих рук (ответ Telegram или обозревателя, ошибка SQLite) —
   в журнал одной строкой: управляющие знаки (перевод строки, ESC-коды
   терминала) — пробелом, не длиннее max байт, обрезка — по границе знака
   UTF-8 и с «…». Иначе ответ мог бы дописать в журнал поддельные строки. */
[[nodiscard]] inline std::string logSafe(std::string_view s, size_t max = 300) {
    std::string out;
    out.reserve(s.size() < max ? s.size() : max);
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        out += c < 0x20U || c == 0x7FU ? ' ' : ch;
    }
    if (out.size() <= max) return out;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0U) == 0x80U) --cut;
    out.resize(cut);
    return out + "…";
}

/* «Сказано ли уже»: одно сообщение на случай, а не каждые 20 секунд опроса.
   Память ограничена: больше cap ключей — набор начинается заново (худшее —
   одно повторное сообщение), а не растёт с каждым мусорным переводом. */
class Once {
public:
    explicit Once(size_t cap = 1000) : cap_(cap) {}
    [[nodiscard]] bool first(const std::string& key) {
        if (seen_.size() >= cap_) seen_.clear();
        return seen_.insert(key).second;
    }
    [[nodiscard]] size_t size() const { return seen_.size(); }

private:
    size_t cap_;
    std::unordered_set<std::string> seen_;
};

}  // namespace premium
