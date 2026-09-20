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
        {"alert_contract", {"Contract", "Контракт"}},
        {"alert_wallet", {"Wallet", "Кошелёк"}},
        {"alert_transaction", {"Transaction", "Транзакция"}},

        {"menu_title", {"🚨 <b>Wallet Tracker</b>", "🚨 <b>Wallet Tracker</b>"}},
        {"menu_add_wallet", {"➕ Add Wallet", "➕ Добавить кошелёк"}},
        {"menu_open_app", {"📱 Open App", "📱 Открыть приложение"}},
        {"menu_my_wallets", {"💼 My Wallets", "💼 Мои кошельки"}},
        {"menu_alert_threshold", {"💰 Alert Threshold", "💰 Порог алертов"}},
        {"menu_account", {"👤 My Account", "👤 Мой аккаунт"}},
        {"account_title", {"👤 <b>My Account</b>", "👤 <b>Мой аккаунт</b>"}},
        {"account_desc", {"Your tracked wallets and what they are holding right now.",
                          "Ваши отслеживаемые кошельки и то, что они держат прямо сейчас."}},
        {"menu_top_traders", {"🏆 Top Traders", "🏆 Топ трейдеров"}},
        {"menu_premium", {"⭐ Premium", "⭐ Премиум"}},
        {"menu_big_trades", {"📊 Analytics", "📊 Аналитика"}},
        {"big_title", {"Analytics", "Аналитика"}},
        {"big_menu_hint", {"The largest single trades of tracked wallets. Pick a venue:",
                           "Самые крупные разовые сделки отслеживаемых кошельков. Выберите площадку:"}},
        {"flow_btn", {"🔥 What whales are buying", "🔥 Что покупают киты"}},
        {"flow_title", {"What whales are buying", "Что покупают киты"}},
        {"flow_hint", {"Net flow: buys minus sells", "Чистый приток: покупки минус продажи"}},
        {"flow_bought", {"bought", "куплено"}},
        {"flow_sold", {"sold", "продано"}},
        {"flow_total_buys", {"buys", "покупок"}},
        {"flow_total_sells", {"sells", "продаж"}},
        {"flow_coins", {"coins", "монет"}},
        {"flow_search_btn", {"🔍 Find a coin", "🔍 Найти монету"}},
        {"flow_search_prompt", {"Send the ticker — for example PEPE", "Отправьте тикер — например PEPE"}},
        {"flow_search_none", {"Nothing found for this period.", "За этот период ничего не найдено."}},
        {"flow_buys", {"buys", "покупок"}},
        {"flow_sells", {"sells", "продаж"}},
        {"flow_wallets", {"wallets", "кошельков"}},
        {"flow_empty", {"No data for this window yet.", "За это окно данных пока нет."}},
        {"ai_btn", {"🧠 Cortex", "🧠 Cortex"}},
        {"ai_title", {"Cortex", "Cortex"}},
        {"ai_hint", {"Cortex is an oracle: a trained model that estimates the chance the price follows the wallets you track. When many independent wallets move the same way, it puts a number on it.\n\nIt looks at 41 signals, not just the flow: volumes and wallet counts, returns over an hour, six hours and a day, volatility and its spikes, ATR, RSI, trend, MACD, funding, open interest, liquidations, leverage, liquidity, the Bitcoin regime and market breadth.\n\nEvents it reads from its own data: how long ago the coin was listed, how far the volume broke out of its own week, and how hard the price was shaken in the last hours. A listing, a hack or a piece of news arrives as a trace in the numbers — and that trace it can learn from. The news text itself it does not read: there is none in its history, so there is nothing there to learn.\n\nThe model decides for itself: the horizon — six hours or a day, the stop and the targets, and the share of the deposit worth risking.\n\nUntil it beats a coin flip on data it has never seen, it is not accepted: the formula works instead, and the model screen says so. This is an estimate, not investment advice.", "Cortex — оракул: обученная модель, которая оценивает шанс, что цена пойдёт туда же, куда пошли кошельки, за которыми вы следите. Когда много независимых кошельков идут в одну сторону, она считает вероятность.\n\nСмотрит не только на поток: 41 признак — объёмы и число кошельков, доходности за час, шесть часов и сутки, волатильность и её всплески, ATR, RSI, тренд, MACD, фандинг, открытый интерес, ликвидации, плечо, ликвидность, режим биткоина и ширина рынка.\n\nСобытия он читает по своим же данным: давно ли монета появилась на бирже, насколько объём выбился из собственной недели и как сильно её тряхнуло за последние часы. Листинг, взлом или новость приходят следом в числах — и этому следу он умеет учиться. Текст новостей он не читает: в его истории их нет, а значит, и учиться там нечему.\n\nМодель решает сама: горизонт — шесть часов или сутки, стоп и цели, и долю депозита, которой стоит рисковать.\n\nПока она не обыграет монетку на данных, которых не видела, её не принимают: работает формула, и на экране состояния так и написано. Это оценка, а не инвестиционный совет."}},
        {"ai_buy", {"Buy", "Покупка"}},
        {"ai_sell", {"Sell", "Продажа"}},
        {"ai_avoid", {"Avoid", "Избегать"}},
        {"ai_empty", {"Not enough independent wallets in this window.",
                      "В этом окне мало независимых кошельков."}},
        {"ai_spot", {"Spot", "Спот"}},
        {"ai_perp", {"Perps", "Перпы"}},
        /* Подписи окон собраны из unit_hour и unit_day того же словаря:
           «1г» по-русски читалось как год, а «24 часа» рядом с «1ч» —
           разнобой. Пять кнопок в ряд помещаются только короткими. */
        {"ai_w1h", {"1h", "1ч"}},
        {"ai_w6h", {"6h", "6ч"}},
        {"ai_w24", {"24h", "24ч"}},
        {"ai_hist_btn", {"📜 Signal history", "📜 История сигналов"}},
        {"ai_st_btn", {"🧠 Model", "🧠 Модель"}},
        {"ai_st_title", {"Model status", "Состояние модели"}},
        {"ai_st_ready", {"Outcomes ready:", "Исходов готово:"}},
        {"ai_st_untrained", {"Not trained yet — using formula.", "Ещё не обучена — работает формула."}},
        {"ai_st_acc", {"Accuracy:", "Точность:"}},
        {"ai_st_samples", {"samples", "примеров"}},
        {"ai_st_cal", {"Calibration:", "Калибровка:"}},
        {"ai_st_top", {"Top weights:", "Главные признаки:"}},
        {"ai_st_trees", {"trees", "деревьев"}},
        {"ai_st_loss", {"Loss:", "Потери:"}},
        {"ai_st_wf", {"Rolling check:", "Скользящая проверка:"}},
        {"ai_st_base", {"baseline", "база"}},
        {"ai_st_coin", {"coin flip", "монетка"}},
        {"ai_st_gate", {"threshold", "порог"}},
        {"ai_p_up", {"chance of a rise in {h}", "шанс роста за {h}"}},
        {"ai_p_dn", {"chance of a fall in {h}", "шанс падения за {h}"}},
        {"ai_wait_bot", {"The bot has not sent signals yet", "Бот ещё не присылал сигналов"}},
        {"ai_none_now", {"No coin passed the filter right now", "Сейчас ни одна монета не прошла отбор"}},
        {"ai_not_passed", {"Model not accepted yet", "Модель пока не принята"}},
        {"ai_hist_none", {"neither", "мимо"}},
        {"ai_p_short", {"chance of a rise", "шанс роста"}},
        {"ai_p_short_dn", {"chance of a fall", "шанс падения"}},
        {"ai_lv_model", {"Levels from the model", "Уровни от модели"}},
        {"ai_lv_formula", {"Levels from volatility", "Уровни по волатильности"}},
        {"ai_model_ok", {"The model is working", "Модель работает"}},
        {"ai_hits_of", {"right {n} times out of 100", "угадывает {n} раз из 100"}},
        {"ai_like_coin", {"no better than a coin flip yet", "пока не лучше монетки"}},
        {"ai_collecting", {"collecting results", "собираем результаты"}},
        {"ai_auc_hint", {"0.50 is a coin flip, 1.00 is perfect", "0.50 — как монетка, 1.00 — идеально"}},
        {"ai_loss_hint", {"lower is better; next to it, the same without the model", "меньше — лучше; рядом то же без модели"}},
        {"ai_wf_hint", {"the same check on several stretches of time in a row", "та же проверка на нескольких отрезках времени подряд"}},
        {"ai_hist_head", {"{a} of {b} signals reached the target", "{a} из {b} сигналов дошли до цели"}},
        {"ai_hist_rest", {"the rest hit the stop or went nowhere", "остальные ушли в стоп или никуда"}},
        {"ai_hist_title", {"Signal history", "История сигналов"}},
        {"ai_hist_empty", {"No completed signals yet.", "Завершённых сигналов пока нет."}},
        {"ai_hist_rate", {"Hit rate:", "Угадано:"}},
        {"ai_hist_avg", {"Average move:", "Средний ход:"}},
        {"ai_hist_hours", {"h ago", "ч назад"}},
        {"ai_w7", {"7d", "7д"}},
        {"ai_w30", {"30d", "30д"}},
        {"ai_trade_hint", {"Cortex estimate. Not investment advice.", "Оценка Cortex. Не инвестиционный совет."}},
        {"ai_long", {"Long", "Лонг"}},
        {"ai_short", {"Short", "Шорт"}},
        {"ai_hours", {"{n}h", "{n} ч"}},
        {"ai_market", {"Market entry", "Вход по рынку"}},
        {"ai_entry", {"📍 Entry", "📍 Вход"}},
        {"ai_stop", {"🛑 Stop", "🛑 Стоп"}},
        {"ai_take", {"🎯 Targets", "🎯 Цели"}},
        {"ai_take_one", {"🎯 Target", "🎯 Цель"}},
        {"ai_expire", {"⏳ Valid", "⏳ Действует"}},
        {"ai_expired", {"⌛ Expired", "⌛ Истёк"}},
        {"ai_risk", {"risk", "риск"}},
        {"ai_hist_plans", {"Plans closed:", "Планов закрыто:"}},
        {"ai_hist_tp", {"by target", "по цели"}},
        {"ai_hist_sl", {"by stop", "по стопу"}},
        {"ai_acc", {"model accuracy", "точность модели"}},
        {"ai_conf", {"Confidence", "Уверенность"}},
        {"ai_why", {"Why", "Почему"}},
        {"ai_mode_formula", {"Formula · model still learning", "Формула · модель ещё учится"}},
        {"ai_mode_model", {"Model", "Модель"}},
        {"ai_why_flow", {"net flow", "чистый поток"}},
        {"ai_why_breadth", {"many wallets", "много кошельков"}},
        {"ai_why_share", {"not one whale", "не один кит"}},
        {"ai_why_top", {"top-100 in the flow", "топ-100 в потоке"}},
        {"ai_why_rsi", {"RSI", "RSI"}},
        {"ai_why_topdir", {"top wallets direction", "куда идут топ-100"}},
        {"ai_why_liqskew", {"liquidation skew", "перекос ликвидаций"}},
        {"ai_why_lev", {"leverage", "плечо"}},
        {"ai_why_oi", {"open interest", "открытый интерес"}},
        {"ai_why_vol", {"volume", "объём"}},
        {"ai_why_liq", {"pool liquidity", "ликвидность пула"}},
        {"ai_why_age", {"time since listing", "давно ли листинг"}},
        {"ai_why_volz", {"volume spike", "всплеск объёма"}},
        {"ai_why_shock", {"price shock", "резкий ход"}},
        {"big_btn_spot", {"🟡 Biggest buys / sells", "🟡 Крупнейшие покупки / продажи"}},
        {"big_btn_perp", {"🔵 Biggest positions", "🔵 Крупнейшие позиции"}},
        {"big_btn_liq", {"💀 Biggest liquidations", "💀 Крупнейшие ликвидации"}},
        {"fund_btn", {"💢 Funding skew", "💢 Перекос фандинга"}},
        {"fund_title", {"Funding skew", "Перекос фандинга"}},
        {"fund_hint", {"Where one side pays the other the most right now",
                       "Где одна сторона платит другой больше всего прямо сейчас"}},
        {"fund_longs_pay", {"longs pay", "лонги платят"}},
        {"fund_shorts_pay", {"shorts pay", "шорты платят"}},
        {"fund_apr", {"per year:", "в год:"}},
        {"fund_oi", {"OI", "ОИ"}},
        {"fund_vol", {"vol", "объём"}},
        {"fund_updated", {"updated", "обновлено"}},
        {"fund_min_ago", {"min ago", "мин назад"}},
        {"fund_empty", {"No strong anomalies right now.", "Сейчас нет сильных аномалий."}},
        {"fund_loading", {"Loading data, open in a few seconds.", "Данные загружаются, откройте через несколько секунд."}},
        {"big_liq_title", {"Biggest liquidations · Hyperliquid", "Крупнейшие ликвидации · Hyperliquid"}},
        {"big_liq_loss", {"Loss", "Убыток"}},
        {"big_liq_position", {"Position", "Позиция"}},
        {"big_liq_closed_at", {"Closed at", "Закрыто по"}},
        {"big_liq_account_was", {"Account was", "На счёте было"}},
        {"big_spot_title", {"Biggest buys / sells", "Крупнейшие покупки / продажи"}},
        {"big_perp_title", {"Biggest positions", "Крупнейшие позиции"}},
        {"big_win_1h", {"1h", "1ч"}},
        {"big_win_24h", {"24h", "24ч"}},
        {"big_win_7d", {"7d", "7д"}},
        {"big_win_30d", {"30d", "30д"}},
        {"big_empty", {"No trades in this window yet.", "За этот период сделок пока нет."}},
        {"big_track_btn", {"Track", "Отслеживать"}},
        {"menu_languages", {"🌐 Languages", "🌐 Язык"}},
        {"menu_help", {"❓ Help", "❓ Помощь"}},
        {"menu_no_wallets", {"You're not tracking any wallets yet.\nTap <b>Add Wallet</b> to start getting alerts.",
                             "Вы пока не отслеживаете ни одного кошелька.\nНажмите <b>Добавить кошелёк</b>, чтобы начать получать алерты."}},
        {"menu_tracking_prefix", {"Tracking", "Отслеживается"}},
        {"menu_alerts_above", {"alert threshold — from", "порог алертов — от"}},

        {"back_button", {"← Back", "← Назад"}},
        {"cancel_button", {"❌ Cancel", "❌ Отмена"}},

        {"add_wallet_title", {"➕ <b>Add Wallet</b>\n\nSend the address of the wallet you want to track — format 0x..., 42 characters.\n\n💡 <b>Not sure whose wallet to add?</b>\nThe 🏆 <b>Top Traders</b> tab below ranks the most profitable wallets of the last 30 days — pick anyone and start tracking in one tap.\nOr paste an address from a block explorer or an analytics service.",
                              "➕ <b>Добавить кошелёк</b>\n\nОтправьте адрес кошелька, который хотите отслеживать — формат 0x..., 42 символа.\n\n💡 <b>Не знаете, чей кошелёк добавить?</b>\nВкладка 🏆 <b>Топ трейдеров</b> ниже — рейтинг самых прибыльных кошельков за 30 дней. Выбирайте любого и берите на отслеживание одним тапом.\nИли вставьте адрес из блокчейн-эксплорера либо аналитического сервиса."}},
        {"add_wallet_addr_ok", {"✅ Address accepted.\n\nNow enter a name for this wallet — up to 32 characters (e.g., \"Binance\"):",
                                "✅ Адрес принят.\n\nТеперь введите имя для этого кошелька — до 32 символов (например, «Binance»):"}},
        {"add_wallet_invalid", {"❌ Invalid wallet address.\n\nThe address must start with 0x and be 42 characters long. Enter a valid address or press Cancel.",
                                "❌ Неверный адрес кошелька.\n\nАдрес должен начинаться с 0x и содержать 42 символа. Введите корректный адрес или нажмите Отмена."}},
        {"add_wallet_success", {"✅ <b>Wallet added</b>", "✅ <b>Кошелёк добавлен</b>"}},
        {"add_wallet_name_label", {"Name:", "Имя:"}},
        {"add_wallet_address_label", {"Address:", "Адрес:"}},
        {"add_wallet_tracking_enabled", {"Tracking enabled.", "Отслеживание включено."}},
        {"track_name_prompt", {"🏷 Enter a name for this trader:", "🏷 Введите имя для этого трейдера:"}},

        {"remove_confirm_title", {"🗑️ <b>Remove wallet?</b>", "🗑️ <b>Удалить кошелёк?</b>"}},
        {"remove_confirm_notice", {"You'll stop receiving alerts for this wallet.",
                                   "Вы перестанете получать алерты по этому кошельку."}},
        {"remove_yes", {"🗑️ Yes, remove", "🗑️ Да, удалить"}},
        {"wl_spot_rank",  {"Spot", "Спот"}},
        {"wl_perp_rank",  {"Futures", "Фьючерсы"}},
        {"wl_main_wallet", {"main", "основной"}},
        {"wl_paused", {"paused", "на паузе"}},
        {"wl_not_ranked", {"not in ranking", "нет в рейтинге"}},
        {"toast_main_wallet_set", {"Main wallet updated", "Основной кошелёк выбран"}},
        {"toast_wallet_removed", {"✅ Wallet removed", "✅ Кошелёк удалён"}},

        {"err_invalid_number", {"❌ Invalid number.", "❌ Неверное число."}},
        {"err_threshold_positive", {"❌ Threshold must be positive.", "❌ Порог должен быть положительным."}},
        {"err_threshold_too_small", {"❌ The minimum alert threshold is $50. Please enter $50 or more.",
                                     "❌ Минимальный порог алертов — $50. Введите $50 или больше."}},
        {"err_threshold_too_large", {"❌ Threshold is too large.", "❌ Слишком большой порог."}},
        {"err_threshold_decimals", {"❌ Use at most 2 decimal places (e.g., 7500.50).", "❌ Используйте не более 2 знаков после запятой (например, 7500.50)."}},
        {"threshold_retry_hint", {"\n\nPlease enter a valid amount (e.g., 7500 or 7500.50) or press Cancel.",
                                  "\n\nВведите корректную сумму (например, 7500 или 7500.50) или нажмите Отмена."}},
        {"threshold_unchanged", {"ℹ️ Current threshold is already", "ℹ️ Текущий порог уже равен"}},
        {"threshold_updated", {"✅ <b>Alert threshold updated</b>\n\nCurrent threshold:",
                               "✅ <b>Порог алертов обновлён</b>\n\nТекущий порог:"}},
        {"threshold_save_failed", {"❌ Failed to save the threshold. Please try again.",
                                   "❌ Не удалось сохранить порог. Попробуйте ещё раз."}},
        {"threshold_title", {"💰 <b>Alert Threshold</b>", "💰 <b>Порог алертов</b>"}},
        {"threshold_desc", {"You'll only be alerted for transactions at or above this amount.",
                            "Вы будете получать алерты только по сделкам от этой суммы и выше."}},
        {"threshold_current", {"Current threshold:", "Текущий порог:"}},
        {"threshold_choose", {"Choose a preset or enter a custom amount:", "Выберите готовый вариант или введите свою сумму:"}},
        {"threshold_custom_btn", {"✏️ Custom amount", "✏️ Своя сумма"}},
        {"threshold_custom_title", {"💰 <b>Custom Threshold</b>\n\nEnter the minimum alert amount in USD — from $50 and up (e.g., 7500 or 7500.50):",
                                    "💰 <b>Своя сумма порога</b>\n\nВведите минимальную сумму алерта в USD — от $50 и выше (например, 7500 или 7500.50):"}},

        {"unknown_command", {"🤔 Please use the menu below.",
                             "🤔 Пожалуйста, используйте меню ниже."}},

        {"lang_title", {"🌐 <b>Language</b>", "🌐 <b>Язык</b>"}},
        {"lang_current", {"Current language:", "Текущий язык:"}},
        {"lang_choose", {"Choose the interface language for alerts and menus:", "Выберите язык интерфейса для алертов и меню:"}},

        {"rk_generating", {"⏳ Rating is being generated.\n\nPlease try again in a minute.",
                           "⏳ Рейтинг формируется.\n\nПопробуйте снова через минуту."}},

        {"mw_no_wallets", {"No wallets tracked yet.", "Пока нет отслеживаемых кошельков."}},
        {"mw_tap_add", {"Tap ➕ <b>Add Wallet</b> to start tracking.", "Нажмите ➕ <b>Добавить кошелёк</b>, чтобы начать отслеживание."}},
        {"mw_free_notice1", {"ℹ️ Free plan: alerts are active only for your main wallet (🔔).",
                             "ℹ️ Бесплатный план: алерты работают только с основного кошелька (🔔)."}},
        {"mw_free_notice2", {"Your other wallets are saved (⏸) and will re-activate with Premium.",
                             "Остальные кошельки сохранены (⏸) и активируются снова с Премиум."}},
        {"mw_upgrade", {"⭐ Upgrade to Premium", "⭐ Улучшить до Премиум"}},
        {"ws_winrate", {"Win Rate", "Винрейт"}},

        {"rk_top_traders_30d", {"Top Traders (30D)", "Топ трейдеров (30д)"}},
        {"rk_roi_per_trade", {"ROI per trade", "ROI за сделку"}},
        {"rk_no_completed_trades", {"No completed trades in the last 30 days yet.", "Пока нет завершённых сделок за последние 30 дней."}},
        {"rk_track", {"➕ Track", "➕ Отследить"}},
        {"rk_avg_hold", {"Avg hold", "Средний холд"}},
        {"unit_day", {"d", "д"}},
        {"unit_hour", {"h", "ч"}},
        {"unit_min", {"m", "м"}},
        {"unit_sec", {"s", "с"}},
        {"rk_in_top", {"In top", "В топе"}},
        {"rk_days", {"days", "дн."}},
        {"rk_trades", {"Trades", "Сделки"}},
        {"rk_unlock_top100", {"🔒 Unlock Top 100 with Premium.", "🔒 Откройте Топ-100 с Премиум."}},
        {"rk_choose_ranking", {"Choose a ranking:", "Выберите рейтинг:"}},
        {"rk_btn_top_pnl", {"💵 Top PnL", "💵 Топ по PnL"}},
        {"rk_btn_top_roi", {"📈 Top ROI", "📈 Топ по ROI"}},
        {"rk_btn_top_winrate", {"🎯 Top Win Rate", "🎯 Топ по винрейту"}},
        {"rk_btn_most_active", {"🔄 Most Active", "🔄 Самые активные"}},

        {"help_title", {"❓ <b>Help</b>", "❓ <b>Помощь</b>"}},
        {"help_intro", {"🏆 See what large wallets buy and sell — BSC spot swaps and Hyperliquid perpetual futures.",
                        "🏆 Смотрите, что покупают и продают крупные кошельки — спот на BSC и бессрочные фьючерсы Hyperliquid."}},
        {"help_commands", {"<b>Use the menu buttons to:</b>", "<b>Используйте кнопки меню, чтобы:</b>"}},
        {"help_menu_add", {"➕ Add Wallet — track a new wallet", "➕ Добавить кошелёк — начать отслеживание"}},
        {"help_menu_mywallets", {"👤 My Account — view and manage your tracked wallets", "👤 Мой аккаунт — просмотр и управление отслеживаемыми кошельками"}},
        {"help_menu_threshold", {"💰 Alert Threshold — set the minimum alert amount", "💰 Порог алертов — задать минимальную сумму алерта"}},
        {"help_menu_top", {"🏆 Top Traders — wallet rankings by 30-day results, spot and futures", "🏆 Топ трейдеров — рейтинг кошельков по результатам за 30 дней, спот и фьючерсы"}},
        {"help_menu_positions", {"📈 Open positions — see what your wallets hold on Hyperliquid right now", "📈 Открытые позиции — что ваши кошельки держат на Hyperliquid прямо сейчас"}},
        {"help_menu_premium", {"⭐ Premium — view Premium plans", "⭐ Премиум — тарифы Премиум"}},
        {"help_menu_languages", {"🌐 Languages — change language", "🌐 Язык — сменить язык"}},
        {"help_premium_title", {"⭐ <b>Premium</b>", "⭐ <b>Премиум</b>"}},
        {"help_premium_1", {"• Hyperliquid futures: ranking, alerts and open positions with leverage, collateral and liquidation price", "• Фьючерсы Hyperliquid: рейтинг, алерты и открытые позиции с плечом, залогом и ценой ликвидации"}},
        {"help_premium_2", {"• Track up to 50 wallets instead of 1", "• Отслеживание до 50 кошельков вместо одного"}},
        {"help_premium_3", {"• Full Top 100 BSC spot traders (Top 30 is free)", "• Полный Топ-100 трейдеров BSC Спот (Топ-30 бесплатно)"}},
        {"help_premium_4", {"• Priority alert delivery", "• Приоритетная доставка алертов"}},
        {"help_disclaimer", {"⚠️ <b>Disclaimer</b>\n"
                             "The bot shows on-chain transactions that have already happened. "
                             "It is an information service, not investment advice and not a recommendation to trade. "
                             "Past results of any wallet do not predict future ones. "
                             "Leveraged trading can cost you your entire deposit. "
                             "You make your own decisions and bear your own risk.",
                             "⚠️ <b>Отказ от ответственности</b>\n"
                             "Бот показывает уже совершённые транзакции из блокчейна. "
                             "Это информационный сервис, а не инвестиционная рекомендация и не призыв к сделкам. "
                             "Прошлые результаты кошелька не предсказывают будущие. "
                             "Торговля с плечом может стоить вам всего депозита. "
                             "Решения вы принимаете сами и риск несёте сами."}},
        {"help_support", {"📞 Support: @WalletTrackerHelp", "📞 Поддержка: @WalletTrackerHelp"}},
        {"help_channel", {"📢 Channel: t.me/WalletTrackerOfficial", "📢 Канал: t.me/WalletTrackerOfficial"}},
        {"help_footer", {"Use the main menu for quick access to all features.",
                         "Используйте главное меню для быстрого доступа ко всем функциям."}},

        {"legal_privacy_title", {"🔒 <b>Privacy Policy</b>", "🔒 <b>Политика конфиденциальности</b>"}},
        {"legal_privacy_body", {"We store only your Telegram chat ID, interface language, alert threshold and the public blockchain addresses you asked us to follow. We never ask for or keep private keys, seed phrases, names, emails or phone numbers; the bot can neither move nor sell your funds.\n\nWallet activity comes from public blockchain and exchange data that exists independently of the bot, and we do not tie it to your identity. The Cortex model learns from those same public trades and market series; who asked to follow an address never enters training.\n\nData sits on our server, is never sold, and goes only where it must for a message to be delivered (Telegram) and a price to be known (public APIs). We keep it while your account exists; the model's outcome journal lives 90 days and then erases itself.\n\nPaying with Stars leaves a payment record: its Telegram id, the amount and the currency. Paying in USD₮ leaves the invoice comment, the amount and the transfer hash — we do not store the sender's address. When you connect a wallet, its public address reaches us and then a public network explorer: there is no other way to find your USD₮ wallet. It is not written to the database. The transfer itself is signed inside the wallet, keys and seed phrases never reach us, and you can revoke the connection in the wallet.\n\nThe /forgetme command and the \"Delete my data\" button erase everything we hold about you — at once and with no way back. One line survives deletion: the chat ID and the date the free week was granted, because without it the same account could take that week again and again. You can ask what we hold about you through the bot.\n\nThis policy may change; the current version is always here and in the bot.", "Мы храним только ваш Telegram chat ID, язык интерфейса, порог алертов и публичные адреса блокчейна, которые вы попросили отслеживать. Мы никогда не спрашиваем и не храним приватные ключи, seed-фразы, имена, почту и телефоны; бот не может ни перевести, ни продать ваши средства.\n\nАктивность кошельков берётся из публичных данных блокчейна и бирж, которые существуют независимо от бота, и с вашей личностью мы их не связываем. Модель Cortex учится на этих же публичных сделках и рыночных рядах; кто именно попросил следить за адресом, в обучение не попадает.\n\nДанные лежат на нашем сервере, никогда не продаются и попадают только туда, без чего не доставить сообщение (Telegram) и не узнать цену (публичные API). Храним, пока существует ваш аккаунт; журнал исходов модели живёт 90 дней и стирается сам.\n\nОплата звёздами оставляет запись о платеже: его номер в Telegram, сумму и валюту. Оплата USD₮ оставляет комментарий счёта, сумму и хеш перевода — адрес отправителя мы не храним. При подключении кошелька его публичный адрес уходит к нам и дальше в публичный обозреватель сети: иначе не найти ваш кошелёк USD₮. В базу он не попадает. Сам перевод подписывается внутри кошелька, ключи и seed-фразы до нас не доходят, а отключить подключение можно в самом кошельке.\n\nКоманда /forgetme и кнопка «Удалить мои данные» стирают всё, что мы о вас храним, — сразу и без возврата. Удаление переживает одна строка: chat ID и дата, когда была выдана бесплатная неделя, — без неё тот же аккаунт мог бы брать ту неделю снова и снова. Спросить, что о вас есть, можно через бота.\n\nПолитика может меняться; действующая версия всегда здесь и в боте."}},
        {"legal_terms_title", {"📄 <b>Terms of Use</b>", "📄 <b>Условия использования</b>"}},
        {"legal_terms_body", {"Wallet Tracker is an information service. It shows trades that have already happened, taken from public sources, and scores them: Cortex signals with entry, stop and target levels.\n\nThis is not investment advice, not a forecast and not a promise of profit. We do not manage your money, execute nothing on your behalf and have no access to your funds: we are not a custodian, an exchange or a broker. The decision is yours and so is the risk.\n\nA Cortex score is a probability from a model trained on the outcomes of the last few months over a 24-hour horizon — not a prediction. Accuracy, AUC and the hit history are measured on data the model never saw while training, describe the past, and can fall on a different market. Until the model is trained a formula is used, and the screen says so. Stop and target levels are derived from volatility: they are reference points, not an order and not a guarantee of execution.\n\nTrading with leverage can wipe out a deposit faster than you can close the position. Position size and leverage are your choice.\n\nData comes from public sources and can be delayed, incomplete or simply wrong. The service is provided as is, without a guarantee of uninterrupted operation. Our liability is limited to the amount paid for the current subscription period, as far as the law allows.\n\nYou may use the service if you are 18 or older and only where local law permits; compliance is on you.\n\nPremium is a paid subscription. Payment in Stars goes through Telegram under its rules, and Stars refunds follow those same rules. A USD₮ transfer on the blockchain is irreversible: paid days are not refunded, including if you delete your data. A payment is matched by the comment from the invoice; a transfer without the comment, for a different amount or in a different coin may not be matched automatically — write to the bot in that case.\n\nAttempts to break the service, get around limits or resell access end it without a refund.\n\nThese terms may change; the current version is always here and in the bot, and continued use means you accept it. Questions go through the bot.", "Wallet Tracker — информационный сервис. Он показывает уже совершённые сделки из публичных источников и считает по ним оценки: сигналы Cortex с уровнями входа, стопа и целей.\n\nЭто не инвестиционный совет, не прогноз и не обещание прибыли. Мы не управляем вашими деньгами, ничего за вас не исполняем и не имеем к средствам доступа: мы не кастодиан, не биржа и не брокер. Решение принимаете вы, риск несёте вы.\n\nОценка Cortex — вероятность по модели, обученной на исходах последних месяцев с горизонтом в сутки, а не предсказание. Точность, AUC и история попаданий измерены на данных, которых модель при обучении не видела, относятся к прошлому и на другом рынке могут упасть. Пока модель не обучена, работает формула — так и написано на экране. Уровни стопа и целей считаются от волатильности: это ориентиры, а не приказ и не гарантия исполнения.\n\nТорговля с плечом может обнулить депозит быстрее, чем вы успеете закрыть позицию. Размер позиции и плечо выбираете вы.\n\nДанные берутся из публичных источников и бывают с задержкой, неполными или неверными. Сервис работает как есть, без гарантии бесперебойной работы. Наша ответственность ограничена суммой, уплаченной за текущий период подписки, — насколько это допускает закон.\n\nПользоваться сервисом можно с 18 лет и только там, где это не запрещено местным законом; соблюдение закона на вас.\n\nПремиум — платная подписка. Оплата звёздами идёт через Telegram по его правилам, и возвраты звёзд — тоже по его правилам. Перевод USD₮ в блокчейне необратим: оплаченные дни не возвращаются, в том числе если вы удалите свои данные. Платёж засчитывается по комментарию из счёта; перевод без комментария, другой суммой или другой монетой может не засчитаться автоматически — тогда пишите в бота.\n\nПопытки сломать сервис, обойти лимиты или перепродать доступ прекращают его без возврата денег.\n\nУсловия могут меняться; действующая версия всегда здесь и в боте, а продолжение пользования означает согласие с ней. Вопросы — через бота."}},
        {"legal_btn_privacy", {"🔒 Privacy", "🔒 Конфиденциальность"}},
        {"legal_btn_terms", {"📄 Terms", "📄 Условия"}},
        {"legal_btn_forget", {"🗑 Delete my data", "🗑 Удалить мои данные"}},
        {"legal_forget_title", {"🗑 <b>Delete my data</b>", "🗑 <b>Удалить мои данные</b>"}},
        {"legal_forget_warn", {"This erases your tracked wallets, alert threshold, language, delivery history and payment records. "
                               "Premium, including days you have already paid for, is lost and is not refunded. "
                               "This cannot be undone. "
                               "The record that the free week was already issued stays — otherwise it could be taken again.",
                               "Будут стёрты отслеживаемые кошельки, порог алертов, язык, история доставок и записи о платежах. "
                               "Премиум, включая уже оплаченные дни, пропадёт и возврату не подлежит. "
                               "Отменить это нельзя. "
                               "Остаётся только отметка, что бесплатная неделя уже выдавалась, — иначе её можно было бы получать заново."}},
        {"legal_forget_yes", {"Yes, delete everything", "Да, удалить всё"}},
        {"legal_forget_done", {"✅ Done. Everything we held about you is deleted. If you ever want to come back, send /start.",
                               "✅ Готово. Всё, что мы о вас хранили, удалено. Если захотите вернуться — отправьте /start."}},
        {"legal_forget_failed", {"❌ Could not delete the data. Please try again later.",
                                 "❌ Не удалось удалить данные. Попробуйте позже."}},

        {"premium_expired_notice", {"⭐ Your Premium has ended. Here is what changed:\n\n🔔 Alerts — main wallet only\nThe others are saved and paused. Pick which one is main in 💼 My wallets.\n\n🔵 Hyperliquid futures — off\nRanking, alerts and open positions are unavailable.\n\n🏆 Top traders — 30 instead of 100\n\nRenewing brings everything back: all 50 wallets, futures and the full Top-100.",
                                    "⭐ Премиум закончился. Что изменилось:\n\n🔔 Алерты — только с основного кошелька\nОстальные сохранены и стоят на паузе. Выбрать основной — в разделе 💼 Мои кошельки.\n\n🔵 Фьючерсы Hyperliquid — отключены\nРейтинг, алерты и открытые позиции недоступны.\n\n🏆 Топ трейдеров — 30 вместо 100\n\nПродление вернёт всё: 50 кошельков, фьючерсы и полный Топ-100."}},
        {"wc_title", {"🚨 <b>Wallet Tracker</b>", "🚨 <b>Wallet Tracker</b>"}},
        {"wc_how", {"You get an alert the moment a tracked wallet buys, sells or opens a position — about 4 seconds after it happens.\n\nBSC spot and Hyperliquid futures. Every figure is verifiable by transaction hash.",
                    "Вы получаете алерт в момент, когда отслеживаемый кошелёк покупает, продаёт или открывает позицию — примерно через 4 секунды.\n\nСпот BSC и фьючерсы Hyperliquid. Каждая цифра проверяется по хешу транзакции."}},
        {"wc_top_intro", {"<b>Start with the best traders right now:</b>", "<b>Начните с лучших трейдеров прямо сейчас:</b>"}},
        {"wc_track_btn", {"Track", "Отслеживать"}},
        {"wc_free_note", {"You can add any wallet yourself — send its address in 💼 My wallets.",
                          "Можно добавить любой кошелёк самому — отправьте его адрес в разделе 💼 Мои кошельки."}},
        {"wc_next_btn", {"➡️ Go to menu", "➡️ Перейти в меню"}},
        {"wc_track_done", {"✅ Added — you will get alerts from this wallet", "✅ Добавлен — алерты с этого кошелька будут приходить"}},
        {"wc_tracked_btn", {"Tracking", "Отслеживается"}},
        {"trial_granted", {"🎁 <b>7 days of Premium — on us</b>\n\nYou now have: Hyperliquid futures with ranking, alerts and open positions, up to 50 wallets, the full Top 100 and priority alerts.\n\nGiven once per account.",
                           "🎁 <b>7 дней Премиума — в подарок</b>\n\nВам доступны: фьючерсы Hyperliquid с рейтингом, алертами и открытыми позициями, до 50 кошельков, полный Топ-100 и приоритетные алерты.\n\nВыдаётся один раз на аккаунт."}},
        {"pr_active_title", {"⭐ <b>Premium Active</b>", "⭐ <b>Премиум активен</b>"}},
        {"pr_service_account", {"Service account — Premium access is permanent.",
                                "Сервисный аккаунт — доступ Премиум постоянный."}},
        {"pr_valid_until_inline", {"Your subscription is valid until", "Подписка действует до"}},
        {"pr_days_left", {"days left:", "осталось дней:"}},
        {"pr_renew", {"⭐ Renew — 250 Stars", "⭐ Продлить — 250 звёзд"}},
        {"pr_title", {"⭐ <b>Wallet Tracker Premium</b>", "⭐ <b>Wallet Tracker Премиум</b>"}},
        {"pr_unlock", {"Unlock the full potential of Wallet Tracker.", "Раскройте весь потенциал Wallet Tracker."}},
        {"pr_includes", {"<b>Premium includes:</b>", "<b>Премиум включает:</b>"}},
        {"pr_subscription_label", {"<b>Subscription:</b> 30 days", "<b>Подписка:</b> 30 дней"}},
        {"pr_price_label", {"<b>Price:</b>", "<b>Цена:</b>"}},
        {"pr_buy", {"⭐ Buy — 250 Stars", "⭐ Купить — 250 звёзд"}},
        {"pr_buy_ton", {"💵 Buy — 3.99 USDT", "💵 Купить — 3.99 USDT"}},
        {"pr_renew_ton", {"💵 Renew — 3.99 USDT", "💵 Продлить — 3.99 USDT"}},
        {"ton_invoice_header", {"💵 <b>Payment in USDT — 3.99</b>\n\nSend from any TON wallet:",
                                "💵 <b>Оплата в USDT — 3.99</b>\n\nОтправьте с любого кошелька TON:"}},
        {"ton_step_network", {"<b>Network</b>", "<b>Сеть</b>"}},
        {"ton_step_amount", {"<b>Amount</b>", "<b>Сумма</b>"}},
        {"ton_step_address", {"<b>Address</b>", "<b>Адрес</b>"}},
        {"ton_network_note", {"Send only in the TON network — coins from other networks will be lost.",
                              "Отправляйте только в сети TON — монеты из других сетей потеряются."}},
        {"ton_step_memo", {"<b>Comment</b> — required", "<b>Комментарий</b> — обязательно"}},
        {"ton_memo_warning", {"⚠️ Without the comment the payment cannot be matched to your account. Some exchanges do not allow comments on withdrawal — then send from a wallet.",
                              "⚠️ Без комментария платёж не получится связать с вашим аккаунтом. Некоторые биржи не дают указать комментарий при выводе — тогда отправляйте из кошелька."}},
        {"ton_invoice_footer", {"Premium turns on automatically within a minute after the transfer. The invoice is valid for 1 hour.",
                                "Премиум включится автоматически в течение минуты после перевода. Счёт действует 1 час."}},
        {"ton_scam_note", {"🔒 These details work only inside this bot. We never ask for payment in DMs.",
                           "🔒 Эти реквизиты работают только внутри бота. Мы никогда не просим оплату в личных сообщениях."}},
        {"ton_support_note", {"Payment did not go through? Write to @WalletTrackerHelp — include the comment code.",
                              "Платёж не прошёл? Напишите @WalletTrackerHelp — укажите код из комментария."}},
        {"ton_paid_ok", {"✅ Payment received — Premium is active for 30 days.\n\nAll wallets, Hyperliquid futures and the full Top-100 are back on.",
                         "✅ Оплата получена — премиум активен 30 дней.\n\nВсе кошельки, фьючерсы Hyperliquid и полный Топ-100 снова включены."}},
        {"pr_limit_title", {"⚠️ <b>Wallet limit reached</b>", "⚠️ <b>Достигнут лимит кошельков</b>"}},
        {"pr_limit_free", {"Free plan allows tracking only 1 wallet.", "Бесплатный план позволяет отслеживать только 1 кошелёк."}},
        {"pr_limit_upgrade", {"Upgrade to Premium to track up to 50 wallets.", "Улучшите до Премиум, чтобы отслеживать до 50 кошельков."}},

        {"invoice_title", {"Wallet Tracker Premium", "Wallet Tracker Премиум"}},
        {"invoice_description", {"30-Day Premium Subscription", "Подписка Премиум на 30 дней"}},
        {"invoice_price_label", {"Premium (30 Days)", "Премиум (30 дней)"}},
        {"invoice_unknown_product", {"Unknown product. Please try again.", "Неизвестный товар. Попробуйте ещё раз."}},
        {"payments_unavailable", {"Payments are temporarily unavailable. Please try again later.",
                                  "Платежи временно недоступны. Пожалуйста, попробуйте позже."}},

        {"payment_success_title", {"✅ Payment successful!", "✅ Оплата прошла успешно!"}},
        {"payment_success_activated", {"Wallet Tracker Premium has been activated.", "Wallet Tracker Премиум активирован."}},
        {"payment_success_duration", {"Valid for 30 days.", "Действует 30 дней."}},

        {"limit_50_reached", {"⚠️ You've reached the limit of 50 tracked wallets.", "⚠️ Достигнут лимит 50 отслеживаемых кошельков."}},
        {"already_tracking", {"⚠️ You're already tracking this wallet.", "⚠️ Вы уже отслеживаете этот кошелёк."}},
        {"toast_already_tracking", {"✅ Already tracking", "✅ Уже отслеживается"}},
        {"wallet_limit_50_short", {"⚠️ Wallet limit reached (50)", "⚠️ Достигнут лимит кошельков (50)"}},
        {"free_plan_1_wallet", {"⚠️ Free plan allows tracking only 1 wallet. Upgrade to Premium — tap ⭐ Premium in the menu.",
                                "⚠️ Бесплатный план позволяет отслеживать только 1 кошелёк. Улучшите до Премиум — нажмите ⭐ Премиум в меню."}},
        {"already_tracking_retry", {"⚠️ You're already tracking this wallet.\n\nPlease enter a different address or press Cancel.",
                                    "⚠️ Вы уже отслеживаете этот кошелёк.\n\nВведите другой адрес или нажмите Отмена."}},
        {"wallet_bot_banned", {"🤖 This is a trading bot — hundreds of trades a day, not in the ranking. Tracking it tells you nothing.\n\nEnter a different address or press Cancel.",
                               "🤖 Это торговый бот — сотни сделок в день, в рейтинге не участвует. Отслеживать его бессмысленно.\n\nВведите другой адрес или нажмите Отмена."}},
        {"wallet_bot_removed", {"🤖 This wallet was removed from your list — it is a trading bot. It makes hundreds of trades a day and is not in the ranking, so tracking it tells you nothing.",
                                "🤖 Кошелёк удалён из вашего списка — это торговый бот. Он совершает сотни сделок в день и не участвует в рейтинге, поэтому отслеживать его бессмысленно."}},
        {"toast_invalid_address", {"❌ Invalid address.", "❌ Неверный адрес."}},
        {"track_now_tracked", {"✅ Wallet", "✅ Кошелёк"}},
        {"track_now_tracked_suffix", {"is now being tracked.", "теперь отслеживается."}},
        {"generic_error_retry", {"❌ Something went wrong, please try again.", "❌ Что-то пошло не так, попробуйте ещё раз."}},

        {"err_invalid_address", {"❌ Invalid wallet address.", "❌ Неверный адрес кошелька."}},
        {"err_loading_wallet", {"❌ Error loading wallet.", "❌ Ошибка загрузки кошелька."}},
        {"err_loading_wallets", {"❌ Error loading wallets.", "❌ Ошибка загрузки кошельков."}},
        {"op_cancelled", {"❌ Operation cancelled.", "❌ Операция отменена."}},
        {"err_invoice_failed", {"❌ Could not create the invoice. Please try again later.",
                                "❌ Не удалось создать счёт. Попробуйте позже."}},
        {"err_user_limit", {"⚠️ User limit reached. Please try again later.",
                            "⚠️ Достигнут лимит пользователей. Попробуйте позже."}},
        {"err_wallet_not_found", {"❌ Wallet not found in your list.", "❌ Кошелёк не найден в вашем списке."}},
        {"rename_title", {"✏️ <b>Rename Wallet</b>", "✏️ <b>Переименовать кошелёк</b>"}},
        {"rename_current_name", {"Current name:", "Текущее имя:"}},
        {"rename_enter_new", {"Please enter a new name:", "Введите новое имя:"}},
        {"rename_success", {"✅ <b>Wallet renamed</b>", "✅ <b>Кошелёк переименован</b>"}},
        {"rename_new_name", {"New name:", "Новое имя:"}},
        {"err_name_empty", {"❌ Name cannot be empty.\n\nPlease enter a name or press Cancel.",
                            "❌ Имя не может быть пустым.\n\nВведите имя или нажмите Отмена."}},
        {"err_name_too_long", {"❌ Name is too long (max 32 characters).\n\nPlease enter a shorter name or press Cancel.",
                               "❌ Имя слишком длинное (макс. 32 символа).\n\nВведите более короткое имя или нажмите Отмена."}},

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
        {"hl_trade_size",    {"Trade size",     "Размер сделки"}},
        {"hl_position_size", {"Position size",  "Размер позиции"}},
        {"hl_position_left", {"Left in position", "Осталось в позиции"}},
        {"hl_collateral",    {"Margin",         "Маржа"}},
        {"hl_of_account",    {"of account",     "счёта"}},
        {"hl_in_position",   {"in position",    "в позиции"}},
        {"hl_position_closed", {"Position fully closed", "Позиция закрыта полностью"}},
        {"hl_fills_in_series", {"Trades in this series", "Сделок в серии"}},
        {"hl_leverage",    {"Leverage",      "Плечо"}},
        {"hl_cross",       {"cross",         "кросс"}},
        {"hl_isolated",    {"isolated",      "изолированное"}},
        {"hl_price",       {"Price",         "Цена"}},
        {"hl_qty",         {"Quantity",      "Количество"}},
        {"hl_pnl",         {"Realized PnL",  "Прибыль по сделке"}},
        {"hl_liq",         {"Liquidation",   "Ликвидация"}},
        {"hl_account",     {"Account balance", "Баланс счёта"}},
        {"hl_venue",       {"Network",       "Сеть"}},
        {"menu_positions",    {"📈 Open positions", "📈 Открытые позиции"}},
        {"hl_open_positions", {"Open positions on Hyperliquid", "Открытые позиции на Hyperliquid"}},
        {"hl_no_open_positions", {"This wallet has no open positions right now.",
                                  "У этого кошелька сейчас нет открытых позиций."}},
        {"hl_positions_choose", {"Choose a wallet to see its open positions:",
                                 "Выберите кошелёк, чтобы посмотреть его открытые позиции:"}},
        {"hl_side_long",   {"Long",  "Лонг"}},
        {"hl_side_short",  {"Short", "Шорт"}},
        {"hl_entry_price", {"Entry", "Вход"}},
        {"hl_mark_price",  {"Current price", "Текущая цена"}},
        {"hl_unrealized",  {"Unrealized PnL", "Незакрытая прибыль"}},

        {"hl_venue_title",  {"Top traders", "Топ трейдеров"}},
        {"hl_venue_choose", {"Choose a network — BSC spot or Hyperliquid perpetual futures",
                             "Выберите сеть — спот на BSC или бессрочные фьючерсы Hyperliquid"}},
        {"hl_venue_spot",   {"🟡 BSC — Spot", "🟡 BSC — Спот"}},
        {"hl_venue_perp",   {"🔵 Hyperliquid — Futures", "🔵 Hyperliquid — Фьючерсы"}},
        {"hl_rk_choose",    {"Choose a ranking:", "Выберите рейтинг:"}},
        {"hl_locked_body",  {"Perpetual futures are available with Premium.\n\n"
                             "You get leverage, collateral and liquidation price on every trade, "
                             "realized PnL straight from the exchange, open positions of your wallets "
                             "and the 30-day trader ranking.",
                             "Фьючерсы доступны по подписке.\n\n"
                             "Вы получаете плечо, залог и цену ликвидации по каждой сделке, "
                             "готовую прибыль от биржи, открытые позиции ваших кошельков "
                             "и рейтинг трейдеров за 30 дней."}},
        {"hl_rk_leverage",  {"Avg leverage", "Среднее плечо"}},
        {"hl_rk_roi_account", {"ROI on deposit", "ROI к депозиту"}},
        {"hl_rk_empty",     {"No data yet — the ranking needs at least 5 closed trades per wallet over 30 days.",
                             "Пока нет данных — в рейтинг попадают кошельки минимум с 5 закрытыми сделками за 30 дней."}},
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

std::string pluralRu(long long n, const std::string& one, const std::string& few, const std::string& many) {
    unsigned long long mag = (n < 0)
        ? (~static_cast<unsigned long long>(n) + 1ULL)
        : static_cast<unsigned long long>(n);
    unsigned long long n100 = mag % 100;
    unsigned long long n10 = mag % 10;
    if (n100 >= 11 && n100 <= 14) return many;
    if (n10 == 1) return one;
    if (n10 >= 2 && n10 <= 4) return few;
    return many;
}
