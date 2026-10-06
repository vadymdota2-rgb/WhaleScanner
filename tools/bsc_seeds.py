#!/usr/bin/env python3
"""Сборка bsc_seeds_book.h — адресов бирж в сети BSC из открытых списков.

Источники — то, что биржи публикуют сами: отчёты о резервах (Proof of
Reserves), собранные DefiLlama для страницы резервов бирж
(github.com/DefiLlama/DefiLlama-Adapters, projects/<биржа>/index.js), и список
резервов, который отдаёт Binance (bapi …/market/por/address).

Адрес кошелька в EVM-сетях один: ключ, которым биржа подписывает в Ethereum,
подписывает и в BSC. Поэтому берутся EVM-адреса биржи из всех её сетей, но
только те, что в BSC — обычный кошелёк (не контракт) и сами отправили хотя бы
пять транзакций. Адрес, который списки приписывают разным биржам,
выбрасывается. Ручной список SEEDS в bsc_exchanges.cpp главнее — его адреса
сюда не попадают.

Третий источник — tools/data/bsc_labels_extra.tsv: метки кошельков бирж
(stablescan.achivx.com) и адреса из отчёта Bybit о резервах; проверяются так же.

Запуск:
  python3 tools/bsc_seeds.py <DefiLlama-Adapters/projects> <binance_por.json>
"""
import concurrent.futures as cf
import json
import os
import re
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAMES = {"okex": "OKX", "bitget": "Bitget", "kucoin": "KuCoin", "gate-io": "Gate", "huobi": "HTX",
         "bitfinex": "Bitfinex", "coinex": "CoinEx", "bitrue-cex": "Bitrue", "gemini": "Gemini",
         "bitstamp": "Bitstamp", "backpack": "Backpack"}
RPC = "https://bsc-dataseed1.bnbchain.org"
MIN_NONCE = 5


def rpc(m, p):
    for i in range(5):
        try:
            req = urllib.request.Request(RPC, json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}).encode(),
                                         {"Content-Type": "application/json"})
            r = json.loads(urllib.request.urlopen(req, timeout=20).read())
            if "result" in r:
                return r["result"]
        except Exception:
            pass
        time.sleep(1 + 2 * i)
    return None


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    projects, por_file = sys.argv[1], sys.argv[2]
    cand: dict[str, str] = {}
    bad: set[str] = set()

    def add(a: str, ex: str) -> None:
        a = a.lower()
        if cand.get(a, ex) != ex:
            bad.add(a)
        cand[a] = ex

    for d, ex in NAMES.items():
        path = os.path.join(projects, d, "index.js")
        if not os.path.exists(path):
            continue
        src = re.sub(r"/\*.*?\*/", "", open(path).read(), flags=re.S)
        # Блоклисты токенов — адреса контрактов, а не кошельки биржи.
        src = re.sub(r"blacklistedTokens\s*:\s*\[[^\]]*\]", "", src)
        for a in re.findall(r"0x[0-9a-fA-F]{40}", src):
            add(a, ex)
    for r in json.load(open(por_file)).get("data") or []:
        if re.fullmatch(r"0x[0-9a-fA-F]{40}", r.get("address", "")):
            add(r["address"], "Binance")
    extra = os.path.join(ROOT, "tools", "data", "bsc_labels_extra.tsv")
    if os.path.exists(extra):
        for line in open(extra):
            if line.startswith("#") or not line.strip():
                continue
            a, ex = line.rstrip("\n").split("\t")[:2]
            add(a, ex)
    for a in bad:
        cand.pop(a, None)
    hand = {a.lower() for a in re.findall(r'\{"(0x[0-9a-fA-F]{40})",',
                                          open(os.path.join(ROOT, "bsc_exchanges.cpp")).read())}

    def check(a: str):
        return a, rpc("eth_getCode", [a, "latest"]), rpc("eth_getTransactionCount", [a, "latest"])

    with cf.ThreadPoolExecutor(6) as pool:
        res = list(pool.map(check, sorted(cand)))
    keep, lost = [], 0
    for a, code, n in res:
        if code is None or n is None:
            lost += 1
            continue
        if code in ("0x", "0x0") and int(n, 16) >= MIN_NONCE and a not in hand:
            keep.append((a, cand[a], int(n, 16)))
    by: dict[str, int] = {}
    for _, ex, _ in keep:
        by[ex] = by.get(ex, 0) + 1
    print(f"кандидатов {len(cand)}, спорных {len(bad)}, сеть не ответила {lost}, взято {len(keep)}: {by}",
          file=sys.stderr)
    lines = [
        "#pragma once",
        "// Собрано tools/bsc_seeds.py — руками не правится. Адреса бирж из отчётов о",
        "// резервах (DefiLlama, projects/<биржа>; резервы Binance, Bybit) и меток кошельков",
        "// (tools/data/bsc_labels_extra.tsv), проверенные в BSC:",
        "// обычный кошелёк, сам отправил от пяти транзакций. Число — сколько отправил.",
        "// " + ", ".join(f"{ex} {n}" for ex, n in sorted(by.items(), key=lambda x: -x[1])),
        "",
        "struct BscSeedBook { const char* addr; const char* ex; };",
        "inline const BscSeedBook BSC_SEEDS_BOOK[] = {",
    ]
    for a, ex, n in sorted(keep, key=lambda x: (x[1], -x[2])):
        lines.append(f'    {{"{a}", "{ex}"}},  // {n}')
    lines.append("};")
    open(os.path.join(ROOT, "bsc_seeds_book.h"), "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
