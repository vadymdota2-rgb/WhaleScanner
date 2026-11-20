#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Бэктест автокопирования на базе размером с настоящую — по памяти (FULL=1 bash tests/run.sh).

На сервере бэктест съел больше 5 ГБ из 7,7 и был остановлен сторожем: он
держал все ордера объектами и заранее строил путь цены на каждый сигнал.
Здесь: синтетическая база (300 000 ордеров, 70 бумаг, 113 дней, кеш часовых
свечей), бэктест — отдельным процессом без сети; пик памяти — меньше 700 МБ,
а при пределе --max-mem-mb ниже нужного — сам останавливается с кодом 3.
"""
import json
import os
import random
import resource
import sqlite3
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "tools", "autotrade_backtest.py")


def make(out: str, n: int) -> None:
    """База как у бота: n ордеров по 2 куска, у 60 % — закрытие; 70 бумаг,
    5000 китов, 113 дней; и кеш часовых свечей (5000 штук до «сейчас»)."""
    os.makedirs(os.path.join(out, "bot"))
    os.makedirs(os.path.join(out, "cache"))
    con = sqlite3.connect(os.path.join(out, "bot", "hyperliquid.db"))
    con.executescript(
        "CREATE TABLE hl_fills (tid INTEGER PRIMARY KEY, wallet TEXT NOT NULL, coin TEXT NOT NULL DEFAULT '', "
        "dir TEXT NOT NULL DEFAULT '', side TEXT NOT NULL DEFAULT '', dir_code INTEGER NOT NULL DEFAULT 0, "
        "px TEXT NOT NULL DEFAULT '', sz TEXT NOT NULL DEFAULT '', closed_pnl_nanos INTEGER NOT NULL DEFAULT 0, "
        "fee_nanos INTEGER NOT NULL DEFAULT 0, notional_nanos INTEGER NOT NULL DEFAULT 0, "
        "margin_nanos INTEGER NOT NULL DEFAULT 0, leverage INTEGER NOT NULL DEFAULT 0, oid INTEGER NOT NULL DEFAULT 0, "
        "account_value_nanos INTEGER NOT NULL DEFAULT 0, ts INTEGER NOT NULL DEFAULT 0, hash TEXT NOT NULL DEFAULT '', "
        "flat INTEGER NOT NULL DEFAULT 0, start_pos_nanos INTEGER);"
        "CREATE INDEX idx_hl_fills_wallet_ts ON hl_fills(wallet, ts); CREATE INDEX idx_hl_fills_ts ON hl_fills(ts);"
        "CREATE TABLE hl_funding_rate (coin TEXT NOT NULL, hour_ts INTEGER NOT NULL, rate_nanos INTEGER NOT NULL "
        "DEFAULT 0, mark_nanos INTEGER NOT NULL DEFAULT 0, oi_nanos INTEGER NOT NULL DEFAULT 0, "
        "day_vlm_nanos INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (coin, hour_ts));")
    now = int(time.time() * 1000)
    t0 = now - 113 * 86_400_000
    coins = [f"xyz:S{i}" for i in range(70)]
    rnd = random.Random(1)
    rows: list[tuple] = []
    tid = 0
    insert = ("INSERT INTO hl_fills(tid, wallet, coin, dir_code, notional_nanos, sz, oid, account_value_nanos, "
              "leverage, ts, closed_pnl_nanos) VALUES(?,?,?,?,?,?,?,?,?,?,?)")
    for k in range(n):
        ts = t0 + rnd.randrange(113 * 86_400_000)
        coin, wallet = rnd.choice(coins), f"0x{rnd.randrange(5000):040x}"
        usd = rnd.choice([500, 2000, 8000, 30_000, 120_000])
        code = rnd.choice([1, 2])
        for part in range(2):
            tid += 1
            rows.append((tid, wallet, coin, code, int(usd / 2 * 1e9), str(usd / 200), k + 1, int(5e13), 10, ts + part, 0))
        if rnd.random() < 0.6:
            tid += 1
            rows.append((tid, wallet, coin, code + 2, int(usd * 1e9), str(usd / 100), 10**9 + k, 0, 0,
                         ts + 3_600_000, (tid % 7 - 3) * 10**9))
        if len(rows) > 200_000:
            con.executemany(insert, rows)
            rows = []
    con.executemany(insert, rows)
    con.commit()
    con.close()
    start = (now // 3_600_000 - 4999) * 3_600_000
    for coin in coins:
        g = random.Random(coin)
        px, have = 100.0, {}
        for i in range(5000):
            o = px
            px *= 1 + g.gauss(0, 0.004)
            have[str(start + i * 3_600_000)] = [o, max(o, px) * 1.002, min(o, px) * 0.998, px]
        with open(os.path.join(out, "cache", f"candles_{coin.replace(':', '_')}_1h.json"), "w", encoding="utf-8") as f:
            json.dump(have, f)


def run(*args: str) -> tuple[int, int, str]:
    """Код выхода, пик памяти (МБ) отдельного процесса и stderr."""
    before = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    r = subprocess.run([sys.executable, SCRIPT, *args], capture_output=True, text=True, check=False)
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return r.returncode, max(peak, before) // 1024, r.stderr


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        make(tmp, 300_000)
        base = ["--db", os.path.join(tmp, "bot", "hyperliquid.db"), "--out", os.path.join(tmp, "out"),
                "--cache", os.path.join(tmp, "cache"), "--offline", "--markets", "stocks", "--dexes", "xyz"]
        t = time.time()
        code, peak, err = run(*base, "--max-mem-mb", "1000")
        took = time.time() - t
        if code != 0 or peak >= 700:
            sys.stderr.write(f"ПРОВАЛ: код {code}, пик {peak} МБ (предел проверки 700)\n{err[-2000:]}")
            return 1
        code, _peak, err = run(*base, "--max-mem-mb", "150")
        if code != 3 or "предела памяти" not in err:
            sys.stderr.write(f"ПРОВАЛ: предел памяти не сработал: код {code}\n{err[-2000:]}")
            return 1
    print(f"autotrade: 300 000 ордеров — пик {peak} МБ за {took:.0f} с; предел памяти — останавливает сам")
    return 0


if __name__ == "__main__":
    sys.exit(main())
