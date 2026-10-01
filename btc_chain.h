#pragma once

/*
 * Сеть Bitcoin — по той же логике, что BSC: блок за блоком, крупные движения
 * в свою базу, оттуда их читает API приложения.
 *
 * На BSC сделка видна прямо в логах свопа. У биткоина бирж в цепочке нет,
 * есть только адреса, поэтому «покупка» здесь — вывод монет с биржи на
 * частный адрес, «продажа» — завод на биржу. Какие адреса биржевые, сканер
 * узнаёт сам: из проверенного списка, по совместной трате входов, по сбору
 * депозитов в горячий кошелёк и по открытой разметке walletexplorer.com.
 *
 * База отдельная (btc.db, путь — WHALE_BTC_DB_FILE): блок биткоина — тысячи
 * транзакций, и писать их в общую базу бота значило бы держать её замок,
 * пока ждут сделки BSC.
 */

#include <cstddef>
#include <string>
#include <vector>

void startBtcLoop();
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
BtcImportResult btcImport(const std::vector<std::string>& addrs);
size_t btcWatchCount();
// Ответ на /statsbtc владельца.
std::string btcStatsLine();
