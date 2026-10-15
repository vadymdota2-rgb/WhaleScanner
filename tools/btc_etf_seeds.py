#!/usr/bin/env python3
"""Сборка btc_etf_book.h — кошельков биржевых фондов (ETF) в сети Bitcoin.

Источник — только то, что фонд публикует о себе сам. Сейчас это Bitwise
Bitcoin ETF (BITB): на странице фонда bitbetf.com выложен полный список его
кошельков с остатками («чтобы любой мог проверить запасы фонда в цепочке»).
Другие эмитенты своих адресов не публикуют — их кошельки сканер ищет сам,
по цепочке (btc_etf.cpp).

Страницу сайт эмитента не разрешает скачивать программой, поэтому скрипт
сам в сеть за ней не ходит: страница сохраняется в браузере (Ctrl+S,
«только HTML»), а скрипт читает файл. Из страницы берутся только адреса —
это факты цепочки, а не текст сайта; остатки и потоки сканер потом считает
сам, по блокам.

Каждый адрес проверяется по контрольной сумме (bech32/base58check): адрес с
ошибкой в записи не берётся. С --check ещё и в сети (mempool.space): адрес
без единой транзакции не берётся.

Запуск:
  python3 tools/btc_etf_seeds.py --fund BITB <сохранённая страница bitbetf.com> [--check]
"""
import hashlib
import json
import os
import re
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "btc_etf_book.h")

# Фонд → откуда список и чем он подписан в шапке книги.
FUNDS = {"BITB": "страница фонда bitbetf.com, раздел кошельков (walletBalances)"}

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
B32 = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def base58_ok(a: str) -> bool:
    n = 0
    for ch in a:
        if ch not in B58:
            return False
        n = n * 58 + B58.index(ch)
    raw = n.to_bytes(25, "big") if n < 1 << 200 else b""
    if len(raw) != 25 or raw[0] not in (0, 5):
        return False
    return hashlib.sha256(hashlib.sha256(raw[:21]).digest()).digest()[:4] == raw[21:]


def bech32_ok(a: str) -> bool:
    if a != a.lower() or not a.startswith("bc1") or not 14 <= len(a) <= 74:
        return False
    data = a[3:]
    if any(ch not in B32 for ch in data):
        return False
    vals = [B32.index(ch) for ch in data]
    gen = [0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3]
    chk = 1
    for v in [3, 3, 0, 2, 3] + vals:  # hrp «bc», развёрнутый по BIP-173
        top = chk >> 25
        chk = (chk & 0x1FFFFFF) << 5 ^ v
        for i in range(5):
            chk ^= gen[i] if (top >> i) & 1 else 0
    witver = vals[0]
    # bech32 (v0) — константа 1, bech32m (v1+) — 0x2BC830A3.
    return chk == (1 if witver == 0 else 0x2BC830A3)


def valid(a: str) -> bool:
    return bech32_ok(a) if a.startswith("bc1") else base58_ok(a)


def wallets(page: str) -> list[tuple[str, float]]:
    rows = re.findall(r'\{"active":(true|false),"address":"([^"]+)","balance":([0-9.]+)\}', page)
    return [(a, float(b)) for act, a, b in rows if act == "true"]


def on_chain(a: str) -> bool:
    req = urllib.request.Request(f"https://mempool.space/api/address/{a}", headers={"User-Agent": "whalescanner"})
    with urllib.request.urlopen(req, timeout=20) as r:
        j = json.load(r)
    return j["chain_stats"]["tx_count"] > 0


def main() -> int:
    args = sys.argv[1:]
    check = "--check" in args
    args = [a for a in args if a != "--check"]
    if len(args) != 3 or args[0] != "--fund" or args[1] not in FUNDS:
        print(__doc__)
        return 2
    fund, path = args[1], args[2]
    with open(path, encoding="utf-8", errors="replace") as f:
        page = f.read()
    asof = re.search(r'"walletBalances":\[.*?\],"updatedAt":"(\d{4}-\d\d-\d\d)', page, re.S)
    rows = wallets(page)
    good = []
    for a, b in rows:
        if not valid(a):
            print("ошибка в записи, пропускаю:", a)
            continue
        if check:
            if not on_chain(a):
                print("в сети пусто, пропускаю:", a)
                continue
            time.sleep(0.3)
        good.append((a, b))
    if not good:
        print("в странице нет списка кошельков — та ли страница сохранена?")
        return 1
    total = sum(b for _, b in good)
    lines = [
        "#pragma once",
        "// Собрано tools/btc_etf_seeds.py — руками не правится. Кошельки биржевых",
        "// фондов, которые фонды публикуют сами. Остатки и потоки сканер считает",
        "// по цепочке, отсюда берутся только адреса.",
        f"// {fund}: {FUNDS[fund]}, на {asof.group(1) if asof else '?'} — "
        f"{len(good)} адресов, {f'{total:,.2f}'.replace(',', ' ')} BTC на странице.",
        "#include <utility>",
        "#include <vector>",
        "",
        "inline const std::vector<std::pair<const char*, const char*>> BTC_ETF_BOOK = {",
    ]
    lines += [f'    {{"{a}", "{fund}"}},' for a, _ in sorted(good)]
    lines += ["};", ""]
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"{fund}: {len(good)} адресов, {total:.8f} BTC → {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
