#pragma once

/*
 * Сеть Bitcoin — по той же логике, что BSC: блок за блоком, крупные движения
 * в свою базу, оттуда их читает API приложения.
 *
 * На BSC сделка видна прямо в логах свопа. У биткоина бирж в цепочке нет,
 * есть только адреса, поэтому «покупка» здесь — вывод монет с биржи на
 * частный адрес, «продажа» — завод на биржу. Какие адреса биржевые, сканер
 * узнаёт сам: из проверенного списка, из адресов отчётов о резервах бирж
 * (btc_seeds_book.h, собирает tools/btc_seeds.py), по совместной трате входов,
 * по сбору депозитов в горячий кошелёк и по открытой разметке walletexplorer.com.
 *
 * База отдельная (btc.db, путь — WHALE_BTC_DB_FILE): блок биткоина — тысячи
 * транзакций, и писать их в общую базу бота значило бы держать её замок,
 * пока ждут сделки BSC.
 */

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

void startBtcLoop();

// Убрать из базы автокошельки (найденные поиском), которые не двигали деньги
// с момента `cut` (unix-время) и на которых меньше 0,2 BTC (держателей не
// трогаем). Не банит: вернутся к делу — поиск добавит снова. Возвращает,
// сколько убрано.
int btcPruneAuto(long long cut);
// Найденные поиском, ставшие сервисами (1000+ транзакций), — убрать навсегда.
int btcBanServices();
void stopBtc();

// База кошельков BTC сервисного аккаунта — та же роль, что у его десяти
// тысяч адресов BSC и Hyperliquid: по ним пишется каждое движение, и они
// идут в рейтинг. Лежит в btc.db (btc_watch), а не в user_whales: адреса
// base58 чувствительны к регистру, а там всё приводится к строчным, и
// сканеры BSC и Hyperliquid взялись бы искать биткоин-адрес у себя.
struct BtcImportResult {
    int added = 0;
    int dup = 0;
};

// Похоже ли на адрес биткоина: bech32/bech32m с проверкой контрольной
// суммы, либо base58 на 1 или 3.
bool isBtcAddress(const std::string& a);
// bc1 — строчными, base58 — как есть.
std::string normBtcAddress(const std::string& a);
// Адрес биткоина в строчной форме — так он лежит в user_whales. bech32
// проверяется по контрольной сумме, у base58 после строчных остаётся только
// форма: длина, первая цифра, буквы и цифры.
bool isBtcKey(const std::string& lowerAddr);
BtcImportResult btcImport(const std::vector<std::string>& addrs);
size_t btcWatchCount();
// Ответ на /statsbtc владельца.
std::string btcStatsLine();

// Движение кошелька, на который подписаны люди, — для алерта. Покупка или
// продажа определяется по второй стороне: пришло с адреса биржи — покупка
// (вывод купленного), ушло на адрес биржи — продажа (завод на продажу).
// Без биржи с другой стороны — перевод.
struct BtcAlert {
    enum Kind { BUY = 0, SELL = 1, IN = 2, OUT = 3 };
    std::string wallet;   // как в цепочке: у base58 регистр значим
    std::string key;      // строчными — так адрес лежит в user_whales
    std::string txid;
    int kind = IN;
    std::string ex;       // биржа с другой стороны, если есть
    long long sats = 0;
    long long usdNanos = 0;
    long long priceNanos = 0;
    long long ts = 0;
    long long height = 0;
    // Сколько транзакций одного вида склеено в этот алерт: у кошелька,
    // который платит пачками, за блок их бывают десятки.
    int txs = 1;
    // По прошлым движениям кошелька в btc.db: средняя цена покупки и, для
    // продажи, результат относительно неё.
    long long avgEntryNanos = 0;
    int priorBuys = 0;
    bool hasPnl = false;
    long long pnlNanos = 0;
    double pnlPct = 0;
};

// Адреса BTC из user_whales (строчными) — бот обновляет их вместе со
// списком наблюдателей. Сюда же входят адреса бесплатных, которые бот
// отсеет при рассылке по тем же правилам, что у BSC.
void btcSetFollowed(std::unordered_set<std::string> lowerAddrs);
// Куда отдавать алерты: рассылка живёт в main.cpp, рядом с BSC.
void btcSetAlertSink(std::function<void(const BtcAlert&)> sink);
