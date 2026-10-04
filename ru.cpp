#include "ru.h"
#include <iostream>
#include <cctype>
#include <vector>
#include <unordered_map>

std::string langCodeOf(Lang l) {
    switch (l) {
        case Lang::RU: return "ru";
        case Lang::ES: return "es";
        case Lang::PT: return "pt";
        case Lang::FR: return "fr";
        case Lang::TR: return "tr";
        case Lang::AR: return "ar";
        case Lang::PL: return "pl";
        case Lang::DE: return "de";
        case Lang::UK: return "uk";
        case Lang::HI: return "hi";
        case Lang::ID: return "id";
        case Lang::VI: return "vi";
        case Lang::KO: return "ko";
        case Lang::ZH: return "zh";
        case Lang::JA: return "ja";
        default:       return "en";
    }
}

Lang langFromCode(const std::string& codeArg) {
    std::string code;
    for (char c : codeArg) {
        if (c == '-' || c == '_') break;
        code += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (code == "ru") return Lang::RU;
    if (code == "es") return Lang::ES;
    if (code == "pt") return Lang::PT;
    if (code == "fr") return Lang::FR;
    if (code == "tr") return Lang::TR;
    if (code == "ar") return Lang::AR;
    if (code == "pl") return Lang::PL;
    if (code == "de") return Lang::DE;
    if (code == "uk") return Lang::UK;
    if (code == "hi") return Lang::HI;
    if (code == "id") return Lang::ID;
    if (code == "vi") return Lang::VI;
    if (code == "ko") return Lang::KO;
    if (code == "zh") return Lang::ZH;
    if (code == "ja") return Lang::JA;
    return Lang::EN;
}

namespace {
struct Entry { const char* en; const char* ru; };

const std::unordered_map<std::string, Entry>& table() {
    static const std::unordered_map<std::string, Entry> t = {
        {"alert_buy", {"BUY", "ПОКУПКА"}},
        {"alert_sell", {"SELL", "ПРОДАЖА"}},
        {"alert_transfer", {"TRANSFER", "ПЕРЕВОД"}},
        {"alert_add_liquidity", {"ADD LIQUIDITY", "ДОБАВЛЕНИЕ ЛИКВИДНОСТИ"}},
        {"alert_remove_liquidity", {"REMOVE LIQUIDITY", "ВЫВОД ЛИКВИДНОСТИ"}},
        {"alert_collect_fees", {"COLLECT FEES", "СБОР КОМИССИЙ"}},
        {"alert_wrap", {"WRAP", "ОБЁРТЫВАНИЕ"}},
        {"alert_unwrap", {"UNWRAP", "РАЗВЁРТЫВАНИЕ"}},
        {"alert_bridge_out", {"BRIDGE OUT", "МОСТ (ИСХОДЯЩИЙ)"}},
        {"alert_bridge_in", {"BRIDGE IN", "МОСТ (ВХОДЯЩИЙ)"}},
        {"alert_arbitrage", {"ARBITRAGE", "АРБИТРАЖ"}},
        {"alert_amount", {"Amount", "Сумма"}},
        {"alert_token", {"Token", "Токен"}},
        {"alert_qty", {"Qty", "Кол-во"}},
        {"alert_buy_price", {"Buy Price", "Цена покупки"}},
        {"alert_prior_buy", {"Previous buy at", "Прошлая покупка по"}},
        {"alert_prior_ago", {"ago", "назад"}},
        {"alert_avg_entry", {"Average entry", "Средняя цена покупки"}},
        {"alert_trade_pnl", {"Trade PnL", "Прибыль по сделке"}},
        {"alert_sell_price", {"Sell Price", "Цена продажи"}},
        {"alert_spent", {"Spent", "Потрачено"}},
        {"alert_received", {"Received", "Получено"}},
        {"alert_from_exchange", {"From exchange", "С биржи"}},
        {"alert_to_exchange", {"To exchange", "На биржу"}},
        {"alert_contract", {"Contract", "Контракт"}},
        {"alert_wallet", {"Wallet", "Кошелёк"}},
        {"alert_transaction", {"Transaction", "Транзакция"}},
        {"menu_open_app", {"📱 Open App", "📱 Открыть приложение"}},
        {"dg_open_btn", {"📰 Open the digest",
            "📰 Открыть дайджест"}},
        {"alert_unlock_btn", {"🔓 Entry price and PnL",
            "🔓 Цена входа и PnL"}},
        {"btn_extend", {"⭐ Extend Premium",
            "⭐ Продлить Премиум"}},
        {"btn_keep", {"⭐ Keep Premium",
            "⭐ Оставить Премиум"}},
        {"btn_plans", {"⭐ Choose a plan",
            "⭐ Выбрать тариф"}},
        {"btn_intro", {"🎁 Get the discount",
            "🎁 Забрать скидку"}},
        {"payment_renewed", {"✅ Premium renewed for another {n} days.",
            "✅ Премиум продлён ещё на {n} дней."}},
        {"dg_ready", {"📰 Today's whale digest is out: the day's signals, market, Bitcoin on exchanges, BSC whales and the ranking leaders.",
            "📰 Вышел дайджест китов за сегодня: сигналы дня, рынок, Bitcoin на биржах, киты BSC и лидеры рейтинга."}},
        {"lc_trial_d5", {"⏳ Your Premium trial ends in 2 days.",
            "⏳ Пробный Премиум закончится через 2 дня."}},
        {"lc_trial_end", {"⏳ Your Premium trial is over.",
            "⏳ Пробный Премиум закончился."}},
        {"lc_prem_end", {"⭐ Your Premium has ended.",
            "⭐ Премиум закончился."}},
        {"lc_stats", {"Trades by your wallets in that time: {n}.",
            "Сделок ваших кошельков за это время: {n}."}},
        {"lc_after", {"Free plan: alerts from your main wallet only, whale trades with a 15-minute delay and the Top 10 traders. Hyperliquid futures, long windows and your other wallets are in Premium.",
            "Бесплатный тариф: алерты только с основного кошелька, сделки китов с задержкой 15 минут и топ-10 трейдеров. Фьючерсы Hyperliquid, длинные окна и остальные кошельки — в Премиуме."}},
        {"lc_keep", {"Get a plan now and your remaining trial days won't be lost — the new period is added on top of them.",
            "Оформите тариф сейчас — оставшиеся дни пробы не сгорят, новый срок прибавится к ним."}},
        {"lc_intro", {"🎁 Just for you: the first month for 150 ⭐ instead of 250. The offer is valid for 48 hours.",
            "🎁 Только для вас: первый месяц — 150 ⭐ вместо 250. Предложение действует 48 часов."}},
        {"lc_missed", {"📉 While Premium was off, your wallets made {n} trades you never got alerts about.",
            "📉 Сделок ваших кошельков, о которых вы не узнали, пока Премиум выключен: {n}."}},
        {"lc_back", {"All alerts, Hyperliquid futures and real-time whale trades come back with Premium in the app.",
            "Все алерты, фьючерсы Hyperliquid и сделки китов в реальном времени вернутся вместе с Премиумом."}},
        {"lc_renew", {"⭐ Premium ends in {n} d. Extend it in advance so your alerts don't stop — the yearly plan is about a third cheaper.",
            "⭐ Премиум закончится через {n} дн. Продлите заранее, чтобы алерты не прервались, — год выходит примерно на треть дешевле."}},
        {"alert_locked", {"Average entry price and trade PnL are in Premium",
            "Средняя цена входа и PnL сделки — в Премиуме"}},
        {"start_open_app", {"🐋 <b>Wallet Tracker</b>\n\nEverything now lives in the app: your wallets, the trader rating, analytics, the daily digest and Premium. This chat only receives alerts for the wallets you follow.",
                            "🐋 <b>Wallet Tracker</b>\n\nВсё теперь в приложении: ваши кошельки, рейтинг трейдеров, аналитика, дайджест дня и премиум. В этот чат приходят только алерты по кошелькам, за которыми вы следите."}},
        {"unit_day", {"d", "д"}},
        {"unit_hour", {"h", "ч"}},
        {"unit_min", {"m", "м"}},
        {"unit_sec", {"s", "с"}},
        {"ton_paid_ok", {"✅ Payment received — Premium is active for {n} days.\n\nAll wallets, Hyperliquid futures and the full Top-100 are back on.",
                         "✅ Оплата получена — премиум активен {n} дней.\n\nВсе кошельки, фьючерсы Hyperliquid и полный Топ-100 снова включены."}},
        {"invoice_unknown_product", {"Unknown product. Please try again.", "Неизвестный товар. Попробуйте ещё раз."}},
        {"payments_unavailable", {"Payments are temporarily unavailable. Please try again later.",
                                  "Платежи временно недоступны. Пожалуйста, попробуйте позже."}},
        {"payment_success_title", {"✅ Payment successful!", "✅ Оплата прошла успешно!"}},
        {"payment_success_activated", {"Wallet Tracker Premium has been activated.", "Wallet Tracker Премиум активирован."}},
        {"payment_success_duration", {"Valid for {n} days.", "Действует {n} дней."}},
        {"wallet_bot_removed", {"🤖 This wallet was removed from your list — it is a trading bot. It makes hundreds of trades a day and is not in the ranking, so tracking it tells you nothing.",
                                "🤖 Кошелёк удалён из вашего списка — это торговый бот. Он совершает сотни сделок в день и не участвует в рейтинге, поэтому отслеживать его бессмысленно."}},
        {"hl_open_long",   {"OPENED LONG",   "ОТКРЫЛ ЛОНГ"}},
        {"hl_close_long",  {"CLOSED LONG",   "ЗАКРЫЛ ЛОНГ"}},
        {"hl_open_short",  {"OPENED SHORT",  "ОТКРЫЛ ШОРТ"}},
        {"hl_close_short", {"CLOSED SHORT",  "ЗАКРЫЛ ШОРТ"}},
        {"hl_add_long",      {"ADDED TO LONG",         "ДОБРАЛ ЛОНГ"}},
        {"hl_add_short",     {"ADDED TO SHORT",        "ДОБРАЛ ШОРТ"}},
        {"hl_partial_long",  {"PARTIAL CLOSE LONG",    "ЧАСТИЧНО ЗАКРЫЛ ЛОНГ"}},
        {"hl_partial_short", {"PARTIAL CLOSE SHORT",   "ЧАСТИЧНО ЗАКРЫЛ ШОРТ"}},
        {"hl_flip",        {"FLIPPED",       "РАЗВЕРНУЛ ПОЗИЦИЮ"}},
        {"hl_liquidated",  {"LIQUIDATED",    "ЛИКВИДИРОВАН"}},
        {"hl_liq_long",    {"LONG LIQUIDATED",  "ЛОНГ ЛИКВИДИРОВАН"}},
        {"hl_liq_short",   {"SHORT LIQUIDATED", "ШОРТ ЛИКВИДИРОВАН"}},
        {"hl_trade",       {"PERP TRADE",    "СДЕЛКА ПО ПЕРПАМ"}},
        {"hl_position_size", {"Position size",  "Размер позиции"}},
        {"hl_position_left", {"Left in position", "Осталось в позиции"}},
        {"hl_collateral",    {"Margin",         "Маржа"}},
        {"hl_of_account",    {"of account",     "счёта"}},
        {"hl_position_closed", {"Position fully closed", "Позиция закрыта полностью"}},
        {"hl_fills_in_series", {"Trades in this series", "Сделок в серии"}},
        {"hl_cross",       {"cross",         "кросс"}},
        {"hl_isolated",    {"isolated",      "изолированное"}},
        {"hl_price",       {"Price",         "Цена"}},
        {"hl_qty",         {"Quantity",      "Количество"}},
        {"hl_pnl",         {"Realized PnL",  "Прибыль по сделке"}},
        {"hl_liq",         {"Liquidation",   "Ликвидация"}},
        {"hl_account",     {"Account balance", "Баланс счёта"}},
    };
    return t;
}
}

namespace {
const char* external(Lang lang, const std::string& key) {
    switch (lang) {
        case Lang::ES: return trEs(key);
        case Lang::PT: return trPt(key);
        case Lang::FR: return trFr(key);
        case Lang::TR: return trTr(key);
        case Lang::AR: return trAr(key);
        case Lang::PL: return trPl(key);
        case Lang::DE: return trDe(key);
        case Lang::UK: return trUk(key);
        case Lang::HI: return trHi(key);
        case Lang::ID: return trId(key);
        case Lang::VI: return trVi(key);
        case Lang::KO: return trKo(key);
        case Lang::ZH: return trZh(key);
        case Lang::JA: return trJa(key);
        default:       return nullptr;
    }
}
}

void checkTranslations() {
    struct { Lang lang; const char* name; } langs[] = {
        {Lang::ES, "ES"}, {Lang::PT, "PT"}, {Lang::FR, "FR"},
        {Lang::TR, "TR"}, {Lang::AR, "AR"}, {Lang::PL, "PL"}, {Lang::DE, "DE"}, {Lang::UK, "UK"}, {Lang::HI, "HI"}, {Lang::ID, "ID"}, {Lang::VI, "VI"}, {Lang::KO, "KO"}, {Lang::ZH, "ZH"}, {Lang::JA, "JA"}
    };
    for (const auto& L : langs) {
        std::vector<std::string> missing;
        for (const auto& kv : table())
            if (!external(L.lang, kv.first)) missing.push_back(kv.first);
        if (missing.empty()) continue;
        std::cerr << "[I18N] " << L.name << ": без перевода " << missing.size()
                  << " из " << table().size() << ", откат на английский:";
        for (size_t i = 0; i < missing.size() && i < 5; i++) std::cerr << " " << missing[i];
        if (missing.size() > 5) std::cerr << " ...";
        std::cerr << std::endl;
    }
}

std::string tr(Lang lang, const std::string& key) {
    auto it = table().find(key);
    if (it == table().end()) return key;
    if (lang == Lang::RU) return it->second.ru;
    if (const char* v = external(lang, key)) return v;
    return it->second.en;
}
