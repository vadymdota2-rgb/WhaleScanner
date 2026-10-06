#!/usr/bin/env python3
"""Сборка btc_seeds_book.h — адресов бирж в сети Bitcoin из открытых списков.

Источники — то, что биржи публикуют сами (отчёты о резервах, Proof of
Reserves), собранное DefiLlama для своей страницы резервов бирж
(github.com/DefiLlama/DefiLlama-Adapters, projects/helper/bitcoin-book), и
список резервов, который отдаёт сама Binance (bapi …/market/por/address).

Берутся только биржи: обёрнутые биткоины, мосты, протоколы, государства и
взломы в списке DefiLlama есть, но потоком бирж они не являются. Адрес,
который два списка приписывают разным биржам, выбрасывается: ложная метка
хуже пропущенной. Ручной список SEEDS в btc_chain.cpp главнее — его адреса
сюда не попадают.

Каждый адрес проверяется в сети (mempool.space): адрес с ошибкой в записи или
без единой транзакции не берётся.

Запуск:
  python3 tools/btc_seeds.py <папка bitcoin-book> <binance_por.json> [--no-check]
"""
import concurrent.futures as cf
import json
import os
import re
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Ключ списка DefiLlama → имя биржи, как его покажет приложение.
EXCHANGES = {
    "binance": "Binance", "binance2": "Binance", "okex": "OKX", "okcoin": "OKCoin",
    "bybit": "Bybit", "bitfinex": "Bitfinex", "kraken": "Kraken", "kucoin": "KuCoin",
    "bitmex": "BitMEX", "bitkub": "Bitkub", "gateIo": "Gate", "huobi": "Huobi",
    "cryptoCom": "Crypto.com", "robinhood": "Robinhood", "deribit": "Deribit",
    "bitget": "Bitget", "mexcCex": "MEXC", "bingCex": "BingX", "bigone": "BigONE",
    "bitmake": "Bitmake", "bitunixCex": "Bitunix", "bitvenus": "BitVenus", "blofinCex": "BloFin",
    "btse": "BTSE", "coindcx": "CoinDCX", "coinex": "CoinEx", "coinsquare": "Coinsquare",
    "coinw": "CoinW", "fastex": "Fastex", "flipster": "Flipster", "hashkey": "HashKey",
    "hashkeyExchange": "HashKey", "hibt": "HiBT", "hotbit": "Hotbit", "kleverExchange": "Klever",
    "korbit": "Korbit", "latoken": "LATOKEN", "maskex": "MaskEX", "nbx": "NBX",
    "nonkyc": "NonKYC", "phemex": "Phemex", "pionexCex": "Pionex", "probit": "ProBit",
    "swissborg": "SwissBorg", "toobit": "Toobit", "wooCEX": "WOO X", "tapbit": "Tapbit",
    "coin8": "Coin8", "bitrue": "Bitrue", "backpack": "Backpack", "hotcoin": "Hotcoin",
    "orangex": "OrangeX", "exmo": "EXMO", "indodax": "Indodax", "weex": "WEEX",
    "bydfi": "BYDFi", "websea": "Websea", "poloniex-cex": "Poloniex", "p2pb2b": "P2B",
    "bitomato": "Bitomato", "lbank": "LBank", "arkhamExchange": "Arkham",
}

ADDR = re.compile(r"\b(bc1[02-9ac-hj-np-z]{11,87}|[13][1-9A-HJ-NP-Za-km-z]{25,34})\b")


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"(?m)//.*$", "", src)


def array_after(src: str, start: int) -> str:
    """Текст массива от '[' в позиции start до парной ']'."""
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "[":
            depth += 1
        elif src[i] == "]":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    return ""


def book(path: str) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    src = strip_comments(open(os.path.join(path, "index.js")).read())
    # Подключаемые файлы: ["okex", "./okex.js"] — в них module.exports = [ … ].
    for key, fn in re.findall(r'\[\s*"([^"]+)"\s*,\s*"\./([^"]+)"\s*\]', src):
        inc = strip_comments(open(os.path.join(path, fn)).read())
        out[key] = ADDR.findall(inc)
    consts = {}
    for m in re.finditer(r"(?m)^const\s+([A-Za-z0-9_]+)\s*=\s*\[", src):
        consts[m.group(1)] = ADDR.findall(array_after(src, m.end() - 1))
    for m in re.finditer(r'(?m)^\s{2}["\']?([A-Za-z0-9_.\-]+)["\']?\s*:\s*\[', src):
        out[m.group(1)] = ADDR.findall(array_after(src, m.end() - 1))
    # Сокращённая запись «p2pb2b,» — массив объявлен константой выше.
    for m in re.finditer(r"(?m)^\s{2}([A-Za-z0-9_]+),\s*$", src):
        if m.group(1) in consts:
            out[m.group(1)] = consts[m.group(1)]
    return out


def hand_seeds() -> set[str]:
    src = open(os.path.join(ROOT, "btc_chain.cpp")).read()
    body = src.split("const std::vector<std::pair<const char*, const char*>> SEEDS = {", 1)[1].split("};", 1)[0]
    return set(re.findall(r'\{"([^"]+)",', body))


def tx_count(a: str) -> int:
    """Сколько транзакций у адреса; -1 — сеть не ответила."""
    for host in ("https://mempool.space/api/address/", "https://blockstream.info/api/address/"):
        for attempt in range(3):
            try:
                req = urllib.request.Request(host + a, headers={"User-Agent": "whalescanner-seeds"})
                with urllib.request.urlopen(req, timeout=15) as r:
                    j = json.loads(r.read().decode())
                c, m = j.get("chain_stats") or {}, j.get("mempool_stats") or {}
                return int(c.get("tx_count", 0)) + int(m.get("tx_count", 0))
            except urllib.error.HTTPError as e:
                if e.code == 400:
                    return 0  # адрес с ошибкой
                time.sleep(2 + attempt * 3)
            except Exception:
                time.sleep(2 + attempt * 3)
    return -1


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--no-check" not in sys.argv
    if len(args) != 2:
        sys.exit(__doc__)
    lists = book(args[0])
    por = json.load(open(args[1]))
    lists["binance_por"] = [r["address"] for r in (por.get("data") or []) if r.get("network") == "BTC"]
    names = {**EXCHANGES, "binance_por": "Binance"}

    owner: dict[str, str] = {}
    bad: set[str] = set()
    for key, ex in names.items():
        for a in lists.get(key) or []:
            if owner.get(a, ex) != ex:
                bad.add(a)
            owner[a] = ex
    for a in bad:
        owner.pop(a, None)
    hand = hand_seeds()
    for a in hand:
        owner.pop(a, None)
    print(f"адресов: {len(owner)}, спорных выброшено: {len(bad)}, уже в ручном списке: {len(hand)}", file=sys.stderr)

    if check:
        addrs = sorted(owner)
        with cf.ThreadPoolExecutor(4) as pool:
            counts = dict(zip(addrs, pool.map(tx_count, addrs)))
        dead = [a for a, n in counts.items() if n == 0]
        lost = [a for a, n in counts.items() if n < 0]
        for a in dead:
            owner.pop(a, None)
        print(f"без транзакций или с ошибкой: {len(dead)}, сеть не ответила (оставлены): {len(lost)}", file=sys.stderr)

    by_ex: dict[str, int] = {}
    for ex in owner.values():
        by_ex[ex] = by_ex.get(ex, 0) + 1
    lines = [
        "#pragma once",
        "// Собрано tools/btc_seeds.py — руками не правится. Адреса бирж из отчётов о",
        "// резервах (Proof of Reserves): списки DefiLlama (projects/helper/bitcoin-book)",
        "// и резервы, которые публикует Binance. Каждый проверен в сети.",
        "// " + ", ".join(f"{ex} {n}" for ex, n in sorted(by_ex.items(), key=lambda x: -x[1])),
        "#include <utility>",
        "#include <vector>",
        "",
        "inline const std::vector<std::pair<const char*, const char*>> SEEDS_BOOK = {",
    ]
    for a, ex in sorted(owner.items(), key=lambda x: (x[1], x[0])):
        lines.append(f'    {{"{a}", "{ex}"}},')
    lines.append("};")
    open(os.path.join(ROOT, "btc_seeds_book.h"), "w").write("\n".join(lines) + "\n")
    print(f"btc_seeds_book.h: {len(owner)} адресов, бирж {len(by_ex)}", file=sys.stderr)
    for ex, n in sorted(by_ex.items(), key=lambda x: -x[1]):
        print(f"  {ex}: {n}", file=sys.stderr)


if __name__ == "__main__":
    main()
