#include "ru.h"
#include <unordered_map>

Lang langFromCode(const std::string& code) {
    return code == "ru" ? Lang::RU : Lang::EN;
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
        {"alert_amount", {"Amount", "Сумма"}},
        {"alert_token", {"Token", "Токен"}},
        {"alert_qty", {"Qty", "Кол-во"}},
        {"alert_buy_price", {"Buy Price", "Цена покупки"}},
        {"alert_sell_price", {"Sell Price", "Цена продажи"}},
        {"alert_spent", {"Spent", "Потрачено"}},
        {"alert_received", {"Received", "Получено"}},
        {"alert_contract", {"Contract", "Контракт"}},
        {"alert_wallet", {"Wallet", "Кошелёк"}},
        {"alert_transaction", {"Transaction", "Транзакция"}},

        {"menu_title", {"🚨 <b>Wallet Tracker</b>", "🚨 <b>Wallet Tracker</b>"}},
        {"menu_add_wallet", {"➕ Add Wallet", "➕ Добавить кошелёк"}},
        {"menu_my_wallets", {"💼 My Wallets", "💼 Мои кошельки"}},
        {"menu_alert_threshold", {"💰 Alert Threshold", "💰 Порог алертов"}},
        {"menu_top_traders", {"🏆 Top Traders", "🏆 Топ трейдеров"}},
        {"menu_premium", {"⭐ Premium", "⭐ Премиум"}},
        {"menu_languages", {"🌐 Languages", "🌐 Язык"}},
        {"menu_help", {"❓ Help", "❓ Помощь"}},
        {"menu_no_wallets", {"You're not tracking any wallets yet.\nTap <b>Add Wallet</b> to start getting alerts.",
                             "Вы пока не отслеживаете ни одного кошелька.\nНажмите <b>Добавить кошелёк</b>, чтобы начать получать алерты."}},
        {"menu_tracking_prefix", {"Tracking", "Отслеживается"}},
        {"menu_alerts_above", {"alerts above", "алерты от"}},

        {"back_button", {"← Back", "← Назад"}},
        {"cancel_button", {"❌ Cancel", "❌ Отмена"}},

        {"add_wallet_title", {"➕ <b>Add Wallet</b>\n\nPlease enter the wallet address (0x...):",
                              "➕ <b>Добавить кошелёк</b>\n\nВведите адрес кошелька (0x...):"}},
        {"add_wallet_addr_ok", {"✅ Address accepted.\n\nNow enter a name for this wallet (e.g., \"Binance\"):",
                                "✅ Адрес принят.\n\nТеперь введите имя для этого кошелька (например, «Binance»):"}},
        {"add_wallet_invalid", {"❌ Invalid BSC address.\n\nPlease enter a valid address or press Cancel.",
                                "❌ Неверный адрес BSC.\n\nВведите корректный адрес или нажмите Отмена."}},
        {"add_wallet_success", {"✅ <b>Wallet added</b>", "✅ <b>Кошелёк добавлен</b>"}},
        {"add_wallet_name_label", {"Name:", "Имя:"}},
        {"add_wallet_address_label", {"Address:", "Адрес:"}},
        {"add_wallet_tracking_enabled", {"Tracking enabled.", "Отслеживание включено."}},
        {"track_name_prompt", {"🏷 Enter a name for this trader:", "🏷 Введите имя для этого трейдера:"}},
        {"token_search_prompt", {"🪙 <b>Top PnL by Token</b>\n\nEnter a token symbol (e.g. <code>CAKE</code>) or a contract address (<code>0x...</code>):",
                                 "🪙 <b>Топ по PnL для токена</b>\n\nВведите символ токена (например, <code>CAKE</code>) или адрес контракта (<code>0x...</code>):"}},
        {"token_search_empty", {"❌ Please enter a token symbol or contract address, or press Cancel.",
                                "❌ Введите символ токена или адрес контракта, либо нажмите Отмена."}},

        {"remove_confirm_title", {"🗑️ <b>Remove wallet?</b>", "🗑️ <b>Удалить кошелёк?</b>"}},
        {"remove_confirm_notice", {"You'll stop receiving alerts for this wallet.",
                                   "Вы перестанете получать алерты по этому кошельку."}},
        {"remove_yes", {"🗑️ Yes, remove", "🗑️ Да, удалить"}},
        {"toast_wallet_removed", {"✅ Wallet removed", "✅ Кошелёк удалён"}},

        {"err_invalid_number", {"❌ Invalid number.", "❌ Неверное число."}},
        {"err_threshold_positive", {"❌ Threshold must be positive.", "❌ Порог должен быть положительным."}},
        {"err_threshold_too_large", {"❌ Threshold is too large.", "❌ Слишком большой порог."}},
        {"err_threshold_decimals", {"❌ Use at most 2 decimal places (e.g., 7500.50).", "❌ Используйте не более 2 знаков после запятой (например, 7500.50)."}},
        {"threshold_retry_hint", {"\n\nPlease enter a valid amount (e.g., 7500 or 7500.50) or press Cancel.",
                                  "\n\nВведите корректную сумму (например, 7500 или 7500.50) или нажмите Отмена."}},
        {"threshold_unchanged", {"ℹ️ Current threshold is already", "ℹ️ Текущий порог уже равен"}},
        {"threshold_updated", {"✅ <b>Alert threshold updated</b>\n\nCurrent threshold:",
                               "✅ <b>Порог алертов обновлён</b>\n\nТекущий порог:"}},
        {"threshold_title", {"💰 <b>Alert Threshold</b>", "💰 <b>Порог алертов</b>"}},
        {"threshold_desc", {"You'll only be alerted for transactions at or above this amount.",
                            "Вы будете получать алерты только по сделкам от этой суммы и выше."}},
        {"threshold_current", {"Current threshold:", "Текущий порог:"}},
        {"threshold_choose", {"Choose a preset or enter a custom amount:", "Выберите готовый вариант или введите свою сумму:"}},
        {"threshold_custom_btn", {"✏️ Custom amount", "✏️ Своя сумма"}},
        {"threshold_custom_title", {"💰 <b>Custom Threshold</b>\n\nPlease enter the minimum alert amount in USD (e.g., 7500 or 7500.50):",
                                    "💰 <b>Своя сумма порога</b>\n\nВведите минимальную сумму алерта в USD (например, 7500 или 7500.50):"}},

        {"unknown_command", {"🤔 Please use the menu below.",
                             "🤔 Пожалуйста, используйте меню ниже."}},

        {"lang_title", {"🌐 <b>Languages</b>", "🌐 <b>Язык</b>"}},
        {"lang_choose", {"Choose your language:", "Выберите язык:"}},

        {"rk_generating", {"⏳ Rating is being generated.\n\nPlease try again in a minute.",
                           "⏳ Рейтинг формируется.\n\nПопробуйте снова через минуту."}},

        {"mw_no_wallets", {"No wallets tracked yet.", "Пока нет отслеживаемых кошельков."}},
        {"mw_tap_add", {"Tap ➕ <b>Add Wallet</b> to start tracking.", "Нажмите ➕ <b>Добавить кошелёк</b>, чтобы начать отслеживание."}},
        {"mw_free_notice1", {"ℹ️ Free plan: alerts are active only for your first wallet (🔔).",
                             "ℹ️ Бесплатный план: алерты активны только для первого кошелька (🔔)."}},
        {"mw_free_notice2", {"Your other wallets are saved (⏸) and will re-activate with Premium.",
                             "Остальные кошельки сохранены (⏸) и активируются снова с Премиум."}},
        {"mw_upgrade", {"⭐ Upgrade to Premium", "⭐ Улучшить до Премиум"}},

        {"ws_title", {"📊 <b>Wallet Statistics</b>", "📊 <b>Статистика кошелька</b>"}},
        {"ws_rank", {"Rank", "Ранг"}},
        {"ws_not_in_ranking", {"(not in 30D ranking)", "(нет в рейтинге за 30 дней)"}},
        {"ws_winrate", {"Win Rate", "Винрейт"}},
        {"ws_trades_30d", {"Trades (30D)", "Сделки (30д)"}},
        {"ws_last_trade", {"Last trade", "Последняя сделка"}},

        {"rk_top_pnl_30d", {"Top PnL (30D)", "Топ по PnL (30д)"}},
        {"rk_top_traders_30d", {"Top Traders (30D)", "Топ трейдеров (30д)"}},
        {"rk_top_roi_30d", {"Top ROI (30D)", "Топ по ROI (30д)"}},
        {"rk_top_winrate_30d", {"Top Win Rate (30D)", "Топ по винрейту (30д)"}},
        {"rk_most_active_30d", {"Most Active (30D)", "Самые активные (30д)"}},
        {"rk_smart_contract", {"Smart Contract", "Смарт-контракт"}},
        {"rk_no_wallets_min_trades1", {"No wallets with at least", "Нет кошельков минимум с"}},
        {"rk_no_wallets_min_trades2", {"completed trades in the last 30 days.", "завершёнными сделками за последние 30 дней."}},
        {"rk_no_completed_trades", {"No completed trades in the last 30 days yet.", "Пока нет завершённых сделок за последние 30 дней."}},
        {"rk_prev", {"⬅️ Prev", "⬅️ Назад"}},
        {"rk_next", {"Next ➡️", "Далее ➡️"}},
        {"rk_track", {"➕ Track", "➕ Отследить"}},
        {"rk_trades", {"Trades", "Сделки"}},
        {"rk_unlock_top100", {"🔒 Unlock Top 100 with Premium.", "🔒 Откройте Топ-100 с Премиум."}},
        {"rk_choose_ranking", {"Choose a ranking:", "Выберите рейтинг:"}},
        {"rk_btn_top_pnl", {"💵 Top PnL", "💵 Топ по PnL"}},
        {"rk_btn_top_roi", {"📈 Top ROI", "📈 Топ по ROI"}},
        {"rk_btn_top_winrate", {"🎯 Top Win Rate", "🎯 Топ по винрейту"}},
        {"rk_btn_most_active", {"🔄 Most Active", "🔄 Самые активные"}},
        {"rk_btn_top_pnl_by_token", {"🪙 Top PnL by Token", "🪙 Топ по PnL для токена"}},
        {"rk_unknown_token1", {"❌ Unknown token:", "❌ Неизвестный токен:"}},
        {"rk_unknown_token2", {"Either give the contract address directly, or a symbol the bot has already seen in a trade.",
                               "Укажите адрес контракта напрямую либо символ токена, который бот уже видел в сделке."}},
        {"rk_expired", {"⏳ This ranking has expired. Please search for the token again via the menu.",
                        "⏳ Этот рейтинг устарел. Пожалуйста, найдите токен заново через меню."}},

        {"help_title", {"❓ <b>Help</b>", "❓ <b>Помощь</b>"}},
        {"help_intro", {"🏆 Discover top on-chain traders, track wallets, and receive real-time trading alerts across supported networks.",
                        "🏆 Находите лучших ончейн-трейдеров, отслеживайте кошельки и получайте уведомления о сделках в реальном времени на поддерживаемых сетях."}},
        {"help_commands", {"<b>Use the menu buttons to:</b>", "<b>Используйте кнопки меню, чтобы:</b>"}},
        {"help_menu_add", {"➕ Add Wallet — track a new wallet", "➕ Добавить кошелёк — начать отслеживание"}},
        {"help_menu_mywallets", {"💼 My Wallets — view and manage your tracked wallets", "💼 Мои кошельки — просмотр и управление отслеживаемыми кошельками"}},
        {"help_menu_threshold", {"💰 Alert Threshold — set the minimum alert amount", "💰 Порог алертов — задать минимальную сумму алерта"}},
        {"help_menu_top", {"🏆 Top Traders — browse top-performing wallets", "🏆 Топ трейдеров — лучшие по доходности кошельки"}},
        {"help_menu_premium", {"⭐ Premium — view Premium plans", "⭐ Премиум — тарифы Премиум"}},
        {"help_menu_languages", {"🌐 Languages — change language", "🌐 Язык — сменить язык"}},
        {"help_premium_title", {"⭐ <b>Premium</b>", "⭐ <b>Премиум</b>"}},
        {"help_premium_1", {"• Full access to Top 100 Traders", "• Полный доступ к Топ-100 трейдеров"}},
        {"help_premium_2", {"• Track up to 50 wallets", "• Отслеживание до 50 кошельков"}},
        {"help_premium_3", {"• Advanced features", "• Расширенные функции"}},
        {"help_premium_4", {"• Priority access to new updates", "• Приоритетный доступ к новым функциям"}},
        {"help_support", {"📞 Support: @WalletTrackerHelp", "📞 Поддержка: @WalletTrackerHelp"}},
        {"help_channel", {"📢 Channel: t.me/WalletTrackerOfficial", "📢 Канал: t.me/WalletTrackerOfficial"}},
        {"help_footer", {"Use the main menu for quick access to all features.",
                         "Используйте главное меню для быстрого доступа ко всем функциям."}},

        {"pr_active_title", {"⭐ <b>Premium Active</b>", "⭐ <b>Премиум активен</b>"}},
        {"pr_service_account", {"Service account — Premium access is permanent.",
                                "Сервисный аккаунт — доступ Премиум постоянный."}},
        {"pr_subscription_active", {"Your subscription is active.", "Ваша подписка активна."}},
        {"pr_valid_until", {"<b>Valid until:</b>", "<b>Действует до:</b>"}},
        {"pr_renew", {"🔄 Renew Premium", "🔄 Продлить Премиум"}},
        {"pr_title", {"⭐ <b>Wallet Tracker Premium</b>", "⭐ <b>Wallet Tracker Премиум</b>"}},
        {"pr_unlock", {"Unlock the full potential of Wallet Tracker.", "Раскройте весь потенциал Wallet Tracker."}},
        {"pr_includes", {"<b>Premium includes:</b>", "<b>Премиум включает:</b>"}},
        {"pr_50_wallets", {"✅ Track up to 50 wallets", "✅ Отслеживание до 50 кошельков"}},
        {"pr_top100", {"✅ Access Top 100 Traders", "✅ Доступ к Топ-100 трейдеров"}},
        {"pr_top10_free", {"(Top 10 available for free)", "(Топ-10 доступен бесплатно)"}},
        {"pr_subscription_label", {"<b>Subscription:</b>\n30 Days", "<b>Подписка:</b>\n30 дней"}},
        {"pr_price_label", {"<b>Price:</b>", "<b>Цена:</b>"}},
        {"pr_buy", {"⭐ Buy Premium", "⭐ Купить Премиум"}},
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
        {"toast_invalid_address", {"❌ Invalid address.", "❌ Неверный адрес."}},
        {"track_now_tracked", {"✅ Wallet", "✅ Кошелёк"}},
        {"track_now_tracked_suffix", {"is now being tracked.", "теперь отслеживается."}},
        {"generic_error_retry", {"❌ Something went wrong, please try again.", "❌ Что-то пошло не так, попробуйте ещё раз."}},

        {"err_invalid_address", {"❌ Invalid wallet address.", "❌ Неверный адрес кошелька."}},
        {"err_loading_wallet", {"❌ Error loading wallet.", "❌ Ошибка загрузки кошелька."}},
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
    };
    return t;
}
}

std::string tr(Lang lang, const std::string& key) {
    auto it = table().find(key);
    if (it == table().end()) return key;
    return lang == Lang::RU ? it->second.ru : it->second.en;
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
