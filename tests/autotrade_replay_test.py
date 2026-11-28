#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Бэктест-повтор (tools/autotrade_replay.py) — на синтетической базе китов
и поддельных свечах OKX: без сети. Проверяется, что повтор торгует по тем же
правилам, что живой бот: вход от 2 китов не хуже их цены, тейк +1,5 %,
выход вслед за китом в убытке, стоп 1,5 %, «входим с крупными», MA30, до 3
позиций, пауза на бумагу, нет на OKX — пропуск; уровней 4ч и вариантов нет;
ход — в progress.json; итог — summary.json для вкладки (с комиссиями и
половинами периода); второй прогон — из кеша, без сети.
"""
import argparse
import json
import os
import sqlite3
import sys
import tempfile
import unittest
from typing import ClassVar
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import autotrade_live as lv  # noqa: E402
import autotrade_replay as rp  # noqa: E402

DAY = 86_400_000
NOW = 1_800_000_000_000 // rp.BAR_MS * rp.BAR_MS
T = NOW - 10 * DAY            # сигнал — 10 дней назад
MIN = 60_000
INSTR = [{"instId": "BTC-USDT-SWAP", "ctVal": "0.01", "ctMult": "1", "ctValCcy": "BTC", "lotSz": "0.01", "minSz": "0.01",
          "tickSz": "0.1", "state": "live"},
         {"instId": "ETH-USDT-SWAP", "ctVal": "0.1", "ctMult": "1", "ctValCcy": "ETH", "lotSz": "0.01", "minSz": "0.01",
          "tickSz": "0.01", "state": "live"},
         {"instId": "SOL-USDT-SWAP", "ctVal": "1", "ctMult": "1", "ctValCcy": "SOL", "lotSz": "0.01", "minSz": "0.01",
          "tickSz": "0.001", "state": "live"},
         {"instId": "DOGE-USDT-SWAP", "ctVal": "1", "ctMult": "1", "ctValCcy": "DOGE", "lotSz": "1", "minSz": "1",
          "tickSz": "0.00001", "state": "live"}]


class FakeMarket:
    """OKX: каталог и 5-минутные свечи по функции цены."""

    def __init__(self, price) -> None:
        self.price = price
        self.calls: list[dict] = []

    def __call__(self, body: dict):
        self.calls.append(body)
        if body["okx"] == "/api/v5/public/instruments":
            return INSTR
        p = body["params"]
        coin = p["instId"].split("-")[0]
        end = int(p["after"])
        step = rp.BAR_MS
        rows = []
        for t in range(end - 100 * step, end, step):
            if t >= NOW:
                continue
            o, c = self.price(coin, t), self.price(coin, t + step - 1)
            rows.append([str(t), str(o), str(max(o, c)), str(min(o, c)), str(c), "1", "1", "1", "1"])
        return list(reversed(rows))


def make_db(path: str, rows: list[tuple]) -> None:
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE hl_fills (tid INTEGER PRIMARY KEY, wallet TEXT, coin TEXT, dir_code INTEGER, ts INTEGER, "
                "notional_nanos INTEGER, sz TEXT, closed_pnl_nanos INTEGER NOT NULL DEFAULT 0, dir TEXT NOT NULL DEFAULT '')")
    con.executemany("INSERT INTO hl_fills VALUES (?,?,?,?,?,?,?,?,?)", rows)
    # Как в базе бота (hyperliquid_core.cpp): без этих индексов запросы китов — полным перебором.
    con.execute("CREATE INDEX idx_hl_fills_code_ts ON hl_fills(dir_code, ts)")
    con.execute("CREATE INDEX idx_hl_fills_wallet_coin_ts ON hl_fills(wallet, coin, ts)")
    con.commit()
    con.close()


def opn(tid: int, wallet: str, coin: str, side: int, ts: int, usd: float, px: float) -> tuple:
    return (tid, wallet, coin, 1 if side > 0 else 2, ts, int(usd * 1e9), str(usd / px), 0, "")


def cls(tid: int, wallet: str, coin: str, side: int, ts: int, pnl: float) -> tuple:
    return (tid, wallet, coin, 3 if side > 0 else 4, ts, int(5000 * 1e9), "0.05", int(pnl * 1e9), "")


def step_price(base: float, moves: list[tuple[int, float]]):
    """Цена: base, с момента t — base × (1 + pct)."""
    def f(_coin: str, t: int) -> float:
        k = 1.0
        for at, pct in moves:
            if t >= at:
                k = 1 + pct
        return base * k
    return f


class ReplayTest(unittest.TestCase):
    AGAINST = False   # сценарии по направлению китов; против китов — test_against_whales

    def setUp(self) -> None:
        for name, value in (("AGAINST", self.AGAINST), ("CHASE", 0.0), ("MAX_OPEN", 3)):
            patcher = mock.patch.object(rp.lv, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.db = os.path.join(self.tmp.name, "hl.db")
        self.out = os.path.join(self.tmp.name, "out")
        self.cache = os.path.join(self.tmp.name, "cache")

    def args(self, **kw) -> argparse.Namespace:
        a = rp.parser().parse_args(["--db", self.db, "--out", self.out, "--cache", self.cache, "--pause", "0",
                                    "--deposit", "20000", "--max-mem-mb", "0"])
        for k, v in kw.items():
            setattr(a, k, v)
        return a

    def go(self, rows: list[tuple], price, **kw) -> dict:
        make_db(self.db, rows)
        self.market = FakeMarket(price)
        return rp.run(self.args(**kw), fetch=self.market, now_ms=NOW)

    def whales(self, side: int = 1, coin: str = "BTC", px: float = 100_000, t: int = T, base: int = 1) -> list[tuple]:
        return [opn(base, "0xa", coin, side, t - 2 * MIN, 50_000, px), opn(base + 1, "0xb", coin, side, t, 50_000, px)]

    def test_take_profit(self) -> None:
        r = self.go(self.whales(), step_price(100_000, [(T + 60 * MIN, 0.016)]))
        [t] = r["trades"]
        self.assertEqual((t.why, t.coin, t.side), ("тейк", "BTC", 1))
        self.assertGreater(t.pnl, 0)
        # 5 % × 20 000 × 10 = 10 000 $ = 0,1 BTC по рынку: с проскальзыванием было бы
        # 100 050, но граница — средняя цена китов 100 000 (IOC); тейк +1,5 % —
        # 101 500: +150 $ минус тейкер 0,05 % на входе и мейкер 0,02 % на выходе.
        self.assertAlmostEqual(t.pnl, 150 - 0.1 * 100_000 * 0.0005 - 0.1 * 101_500 * 0.0002, delta=0.02)
        self.assertAlmostEqual(t.fees, 0.1 * 100_000 * 0.0005 + 0.1 * 101_500 * 0.0002, delta=0.01)
        self.assertAlmostEqual(r["summary"]["full"]["fees"], 7.03, delta=0.01)
        self.assertEqual(r["summary"]["halves"], [0.0, r["summary"]["full"]["net"]], "вход — во второй половине 30 дней")
        self.assertNotIn("variants", r["summary"])
        self.assertNotIn("edge", r["summary"])
        self.assertRegex(r["text"], r"Весь период: сделок 1, .*; комиссии 7\.03 \$ \(до них \+150\.00 \$\); "
                                    r"половины периода: \+0\.00 / \+142\.97 \$")
        s = r["summary"]
        self.assertEqual((s["full"]["trades"], s["sizing"]["margin_pct"], s["trades"][0]["why"]), (1, 5.0, "тейк"))
        self.assertGreater(s["sizing"]["deposit_end"], s["sizing"]["deposit"])
        self.assertEqual(s["by_exit"][0]["key"], "тейк")
        with open(os.path.join(self.out, "progress.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["stage"], "готово")
        self.assertTrue(os.path.isfile(os.path.join(self.out, "summary.json")))

    def test_whale_loss_is_no_exit(self) -> None:
        """Выхода вслед за китом нет: кит закрыл в убыток — сидим до тейка."""
        rows = [*self.whales(), cls(10, "0xa", "BTC", 1, T + 30 * MIN, -100.0)]
        r = self.go(rows, step_price(100_000, [(T + 20 * MIN, -0.003), (T + 90 * MIN, 0.016)]))
        [t] = r["trades"]
        self.assertEqual(t.why, "тейк")

    def test_whale_profit_close_keeps_position(self) -> None:
        rows = [*self.whales(), cls(10, "0xa", "BTC", 1, T + 30 * MIN, +100.0)]
        r = self.go(rows, step_price(100_000, [(T + 90 * MIN, 0.016)]))
        self.assertEqual(r["trades"][0].why, "тейк")

    def test_stop_1_5_percent(self) -> None:
        """Стоп — 1,5 % от цены входа (как у живого бота): вход 100 000 → стоп
        98 500. Свеча открылась за стопом — по её открытию; задела стоп — по
        цене стопа; 98 600 — держим, потом тейк; ровная история — тот же стоп."""
        H = 60 * MIN

        def hist(after):
            def f(c: str, t: int) -> float:
                if t < T:
                    return 99_000.0 if T - 16 * H <= t < T - 12 * H else 103_000.0 if T - 24 * H <= t < T - 20 * H else 100_000.0
                return after(t)
            return f

        r = self.go(self.whales(), hist(lambda t: 96_000.0 if t >= T + 20 * MIN else 100_000.0))
        [t] = r["trades"]
        self.assertEqual(t.why, "стоп")
        self.assertAlmostEqual(t.exit_px, 96_000 * 0.9995, delta=1, msg="свеча открылась за стопом — по её открытию")
        os.remove(self.db)
        self.cache += "2"
        r = self.go(self.whales(), hist(lambda t: 101_600.0 if t >= T + 90 * MIN else 98_600.0 if t >= T + 20 * MIN else 100_000.0))
        self.assertEqual([t.why for t in r["trades"]], ["тейк"], "98 600 — выше стопа")
        os.remove(self.db)
        self.cache += "3"
        r = self.go(self.whales(), hist(lambda t: 96_000.0 if T + 21 * MIN <= t < T + 25 * MIN else 100_000.0))
        [t] = r["trades"]
        self.assertEqual(t.why, "стоп")
        self.assertAlmostEqual(t.exit_px, 98_500 * 0.9995, delta=1, msg="по цене стопа")
        os.remove(self.db)
        self.cache += "4"
        r = self.go(self.whales(), step_price(100_000, [(T + 20 * MIN, -0.014)]))   # история ровно 100 000
        self.assertEqual([t.why for t in r["trades"]], ["открыта"], "−1,4 % — держим")
        os.remove(self.db)
        self.cache += "5"
        r = self.go(self.whales(), step_price(100_000, [(T + 20 * MIN, -0.016)]))
        [t] = r["trades"]
        self.assertEqual(t.why, "стоп", "стоп 1,5 % от входа")
        self.assertAlmostEqual(t.exit_px, 98_400 * 0.9995, delta=1, msg="свеча открылась за стопом")

    def test_candles_retry(self) -> None:
        """Сбой запроса свечей — ещё попытка (до 3): блок со свечами, сбоев нет.
        Три сбоя подряд — блок без свечей и сбой в итоге."""
        self.market = FakeMarket(lambda c, ts: 100_000.0)
        calls = {"n": 0}

        def flaky(body):
            calls["n"] += 1
            if calls["n"] <= 2:
                raise OSError("обрыв")
            return self.market(body)

        with mock.patch.object(rp.time, "sleep"):
            px = rp.Prices(self.cache, flaky, 0, False, NOW)
            self.assertTrue(px.block("BTC-USDT-SWAP", (T // px.block_ms)))
            self.assertEqual((px.errors, calls["n"]), (0, 3))
            dead = rp.Prices(self.cache + "x", lambda b: None, 0, False, NOW)
            self.assertEqual(dead.block("BTC-USDT-SWAP", T // dead.block_ms), [])
            self.assertEqual(dead.errors, 1)
        make_db(self.db, self.whales())
        a = self.args()
        prices = rp.Prices(a.cache, self.market, 0, False, NOW)
        r = rp.Replay(a, prices, rp.instruments(self.market, False, a.cache), lambda _p: None)
        r.run(NOW - a.since_days * DAY, NOW)
        self.assertEqual([x.why for x in r.trades], ["открыта"])

    def test_halves_split_by_entry_time(self) -> None:
        """Итог по половинам периода — по времени входа; ровно середина — во
        второй; сделок нет — нули."""
        def tr(entry: int, pnl: float) -> rp.Trade:
            return rp.Trade("BTC", 1, entry, entry + 1, 1.0, 1.0, 1.0, 1.0, pnl, "тейк", 0.0, (), 0.0)

        self.assertEqual(rp.halves([tr(0, 1.0), tr(49, 2.0), tr(50, 4.0), tr(99, -8.0)], 0, 100), [3.0, -4.0])
        self.assertEqual(rp.halves([], 0, 100), [0.0, 0.0])

    def test_entry_not_worse_than_whales(self) -> None:
        """Вход не хуже цены китов: цена выше их средней на 0,2 % — пропуск
        «chase» (при допуске 0,3 % — вход); цена ниже их средней — вход."""
        r = self.go(self.whales(), step_price(100_200, [(T + 60 * MIN, 0.016)]))
        self.assertEqual((len(r["trades"]), r["skips"].get("chase")), (0, 1))
        os.remove(self.db)
        self.cache += "1"
        with mock.patch.object(rp.lv, "CHASE", 0.003):
            r = self.go(self.whales(), step_price(100_200, [(T + 60 * MIN, 0.016)]))
        self.assertEqual(len(r["trades"]), 1, "допуск — lv.CHASE, как у живого бота")
        os.remove(self.db)
        self.cache += "2"
        r = self.go(self.whales(), step_price(99_900, [(T + 60 * MIN, 0.016)]))
        self.assertEqual(len(r["trades"]), 1)
        os.remove(self.db)
        self.cache += "3"
        r = self.go(self.whales(), step_price(100_001, [(T + 60 * MIN, 0.016)]))
        self.assertEqual(r["skips"].get("chase"), 1, "и на 0,001 % хуже китов — пропуск")

    def test_stop_and_no_4h_candles(self) -> None:
        """Уровней 4ч нет (правило владельца 27.11.2026): поддержка далеко
        (90 000) и сопротивление рядом — не важно; стоп — 1,5 % от цены входа
        (100 000 → 98 500); свечи 4ч не запрашиваются."""
        H = 60 * MIN

        def f(c: str, t: int) -> float:
            if t < T:
                return 90_000.0 if T - 16 * H <= t < T - 12 * H else 130_000.0 if T - 24 * H <= t < T - 20 * H else 100_000.0
            return 96_950.0 if T + 20 * MIN <= t < T + 25 * MIN else 100_000.0

        [t] = self.go(self.whales(), f)["trades"]
        self.assertEqual(t.why, "стоп")
        self.assertAlmostEqual(t.exit_px, 96_950 * 0.9995, delta=1, msg="свеча открылась за стопом 98 500")
        bars = {c["params"].get("bar") for c in self.market.calls if c.get("okx") == "/api/v5/market/history-candles"}
        self.assertEqual(bars, {"5m"}, "только 5-минутные свечи — 4ч не запрашиваются")

    def test_ma30_15m(self) -> None:
        """MA30 15-минутного графика (29 закрытых свечей + идущая по цене).
        1) 2,5 ч до сигнала — 101 000, раньше — 99 000: MA ≈ 99 700 ниже цены —
           лонг входит (MA по 5-минуткам была бы 101 000 — не вошёл бы);
        2) 8 ч — 100 500: MA выше цены — пропуск ma30; провал до 60 000 внутри
           идущей 5-минутки (её закрытие — будущее) не в счёт;
        3) 99 000, а последняя закрытая 5-минутка — 120 000: MA ≈ 99 900 — лонг
           входит (идущая свеча — по цене 100 000, а не по прошлому закрытию)."""
        H = 60 * MIN

        def price(recent):
            def f(c: str, t: int) -> float:
                if t >= T:
                    if T + 1000 <= t < T + 5 * MIN:
                        return 60_000.0 if recent is None else 100_000.0
                    return 102_000.0 if t >= T + 60 * MIN else 100_000.0
                if T - 24 * H <= t < T - 20 * H:
                    return 97_000.0
                if T - 16 * H <= t < T - 12 * H:
                    return 108_000.0
                if t >= T - 8 * H:
                    return recent(t) if recent else 100_500.0
                return 100_000.0
            return f

        r = self.go(self.whales(), price(lambda t: 101_000.0 if t >= T - 150 * MIN else 99_000.0))
        self.assertEqual([t.why for t in r["trades"]], ["тейк"])
        os.remove(self.db)
        self.cache += "2"
        r = self.go(self.whales(), price(None))
        self.assertEqual(r["trades"], [])
        self.assertEqual(r["skips"].get("ma30"), 1)
        os.remove(self.db)
        self.cache += "3"
        r = self.go(self.whales(), price(lambda t: 120_000.0 if t >= T - 5 * MIN else 99_000.0))
        self.assertEqual([t.why for t in r["trades"]], ["тейк"])

    def test_rating_priority_whale_alone(self) -> None:
        """Рейтинг — по сделкам повтора: 5 сделок BTC с китами 0xa и 0xb (каждые
        4 ч) — все по тейку; потом 0xa один — приоритетный (5 из 5): входим с
        ним одним, хоть MA30 и против (MA ему не помеха). До пятой сделки
        одиночный 0xa — не сигнал."""
        H = 60 * MIN
        entries = [T + i * 4 * H for i in range(5)]

        def price(c: str, t: int) -> float:
            if T - 28 * H <= t < T - 24 * H:
                return 96_000.0                       # прошлое — не важно (уровней нет)
            if T - 24 * H <= t < T - 20 * H:
                return 110_000.0
            for e in entries:
                if e <= t < e + 10 * MIN:
                    return 100_000.0                  # вход
                if e + 10 * MIN <= t < e + 30 * MIN:
                    return 101_600.0                  # тейк +1,5 %
            if T + 16 * H + 30 * MIN <= t < T + 20 * H:
                return 120_000.0                      # перед одиночным: MA30 ≈ 109 000 — выше цены, против лонга
            if T + 20 * H <= t < T + 20 * H + 10 * MIN:
                return 100_000.0                      # приоритетный один
            if T + 20 * H + 10 * MIN <= t < T + 20 * H + 30 * MIN:
                return 101_600.0                      # тейк +1,5 %
            return 99_000.0                           # MA30 ниже цены входа

        rows: list[tuple] = []
        for i, e in enumerate(entries):
            rows += [opn(10 * i + 1, "0xa", "BTC", 1, e - MIN, 50_000, 100_000), opn(10 * i + 2, "0xb", "BTC", 1, e, 50_000, 100_000)]
        early = opn(90, "0xa", "BTC", 1, T + 15 * H + 30 * MIN, 50_000, 100_000)   # одиночный после паузы, до пятой — не сигнал
        rows += [early, opn(99, "0xa", "BTC", 1, T + 20 * H, 50_000, 100_000)]
        rows.sort(key=lambda r: r[4])
        r = self.go(rows, price)
        self.assertEqual([(t.why, (t.entry_t - T) // H) for t in r["trades"]],
                         [("тейк", 0), ("тейк", 4), ("тейк", 8), ("тейк", 12), ("тейк", 16), ("тейк", 20)])
        self.assertEqual(r["trades"][-1].wallets, ("0xa",))

    def test_one_big_against_blocks_small_side(self) -> None:
        big = [opn(100, "0xbig1", "BTC", -1, T - 40 * MIN, 7_000_000, 100_000)]   # один крупный — сигнала у него нет
        r = self.go(big + self.whales(), step_price(100_000, [(T + 60 * MIN, 0.016)]))
        self.assertEqual(r["trades"], [])
        self.assertGreater(r["skips"]["bigger_against"], 0)

    def test_bigger_against_goes_with_big(self) -> None:
        big = [opn(100, "0xbig1", "BTC", -1, T - 40 * MIN, 7_000_000, 100_000),
               opn(101, "0xbig2", "BTC", -1, T - 39 * MIN, 7_000_000, 100_000)]
        r = self.go(big + self.whales(), step_price(100_000, [(T + 60 * MIN, -0.016)]))
        [t] = r["trades"]
        self.assertEqual((t.side, t.why), (-1, "тейк"))

    PX: ClassVar[dict[str, float]] = {"BTC": 100_000.0, "ETH": 3000.0, "SOL": 150.0, "DOGE": 0.2, "NOPE": 1.0}

    def test_three_positions_and_no_inst(self) -> None:
        """Своих позиций — до 3 (как у живого бота): BTC, ETH, SOL открыты,
        DOGE — «own_open»; бумаги нет на OKX — «no_inst»."""
        rows = [*self.whales(), *self.whales(coin="ETH", px=3000, t=T + 30 * MIN, base=10),
                *self.whales(coin="SOL", px=150, t=T + 31 * MIN, base=20),  # NOPE — раньше: пока место есть
                *self.whales(coin="DOGE", px=0.2, t=T + 32 * MIN, base=30),
                *self.whales(coin="NOPE", px=1, t=T + 15 * MIN, base=40)]
        r = self.go(rows, lambda c, t: self.PX[c])
        self.assertEqual([t.coin for t in r["trades"]], ["BTC", "ETH", "SOL"], "позиций до трёх")
        self.assertGreater(r["skips"]["own_open"], 0)
        self.assertGreater(r["skips"]["no_inst"], 0)

    def test_same_coin_while_open_and_equity_from_closed_only(self) -> None:
        """BTC открыт дольше паузы — новый сигнал BTC: «own_same». Размер
        ETH (вход, пока BTC открыт) — от капитала без итога BTC; SOL (после
        тейка BTC) — с ним."""
        rows = [*self.whales(), *self.whales(coin="ETH", px=3000, t=T + 30 * MIN, base=10),
                *self.whales(coin="SOL", px=150, t=T + 90 * MIN, base=20)]

        def price(c: str, t: int) -> float:
            return 101_600.0 if c == "BTC" and t >= T + 60 * MIN else self.PX[c]

        r = self.go(rows, price)
        btc, eth, sol = r["trades"]
        self.assertEqual((btc.why, eth.why, sol.why), ("тейк", "открыта", "открыта"))
        self.assertAlmostEqual(eth.notional, 33.33 * 0.1 * 3000, delta=0.5, msg="10 000 $ без итога BTC")
        self.assertAlmostEqual(sol.notional, (20_000 + btc.pnl) * 0.5, delta=2, msg="с итогом BTC")
        self.assertGreater(sol.notional, eth.notional + 30)
        os.remove(self.db)
        self.cache += "2"
        rows = [*self.whales(), *self.whales(t=T + 4 * 60 * MIN, base=10)]
        r = self.go(rows, lambda c, t: self.PX[c])
        self.assertEqual(len(r["trades"]), 1)
        self.assertEqual(r["skips"].get("own_same"), 1)

    def test_against_whales(self) -> None:
        """Против китов (как живой бот: lv.AGAINST, lv.CHASE = −0,01): киты в
        лонг по 100 000 — шорт только от 101 000; цена −1,6 % — тейк шорта; кит
        закрыл лонг в убытке — выхода нет; 100 500 — рано («chase»); MA30 — по
        стороне сделки: цена выше MA30 — в шорт не входим («ma30»)."""
        with mock.patch.object(rp.lv, "AGAINST", True), mock.patch.object(rp.lv, "CHASE", -0.01):
            rows = [*self.whales(), cls(9, "0xa", "BTC", 1, T + 10 * MIN, -100.0)]
            [tr] = self.go(rows, step_price(101_000, [(T + 60 * MIN, -0.016)]))["trades"]
            self.assertEqual((tr.side, tr.why), (-1, "тейк"))
            self.assertAlmostEqual(tr.entry_px, 101_000.0, places=6)
            self.assertAlmostEqual(tr.tp, 99_485.0)
            os.remove(self.db)
            self.cache += "1"
            r = self.go(self.whales(), step_price(100_500, [(T + 60 * MIN, -0.016)]))
            self.assertEqual((r["trades"], r["skips"].get("chase")), ([], 1))
            os.remove(self.db)
            self.cache += "2"
            r = self.go(self.whales(), lambda c, ts: 100_000.0 if ts < T else 101_000.0)
            self.assertEqual((r["trades"], r["skips"].get("ma30")), ([], 1), "цена выше MA30 — в шорт не входим")
            self.assertIn("вход по рынку на 1 % лучше средней цены китов (шорт — выше, лонг — ниже), против китов", r["text"])

    def test_stall_exit_not_below_lock(self) -> None:
        """Замедление (как живой бот): +1 % и 15 минут без нового максимума —
        выход по рынку на открытии свечи (не хуже +0,8 %); максимум +1 %, потом
        +0,5 % и стоит — не выходим (не меньше +0,8 %)."""
        def stalled(c: str, ts: int) -> float:
            return 100_000.0 if ts < T + 60 * MIN else 101_000.0 if ts < T + 120 * MIN else 102_000.0

        [tr] = self.go(self.whales(), stalled)["trades"]
        self.assertEqual(tr.why, "замедление")
        self.assertAlmostEqual(tr.exit_px, 101_000 * 0.9995, places=3)
        self.assertEqual((tr.exit_t - T) // MIN, 80, "максимум виден в конце свечи (65 мин) + 15 минут")
        os.remove(self.db)
        self.cache += "1"

        def dipped(c: str, ts: int) -> float:
            return 100_000.0 if ts < T + 60 * MIN else 101_000.0 if ts < T + 65 * MIN else 100_500.0

        [tr] = self.go(self.whales(), dipped)["trades"]
        self.assertEqual(tr.why, "открыта")
        os.remove(self.db)
        self.cache += "2"

        def back(c: str, ts: int) -> float:   # +1 %, потом +0,5 %, потом свеча задела +0,8 % — выход по границе
            if ts < T + 60 * MIN:
                return 100_000.0
            if ts < T + 65 * MIN:
                return 101_000.0
            return 100_850.0 if T + 120 * MIN <= ts < T + 125 * MIN else 100_500.0

        [tr] = self.go(self.whales(), back)["trades"]
        self.assertEqual(tr.why, "замедление")
        self.assertGreaterEqual(tr.exit_px, 100_800.0 - 1e-6)

    def test_big_against_crowd_goes_with_big(self) -> None:
        """Против китов: крупные шорты (от 2) против толпы лонгов — шорт с
        крупными (без разворота); одна толпа лонгов — шорт против неё."""
        with mock.patch.object(rp.lv, "AGAINST", True), mock.patch.object(rp.lv, "CHASE", -0.01):
            # Крупные шорты по 100 000 пришли первыми: тогда они — толпа, но лонг
            # против них — только от 99 000 (цена 100 000 — пропуск «chase»).
            # Толпа лонгов (по 101 000) против крупных: шорт с крупными — от 101 000.
            big = [opn(100, "0xbig1", "BTC", -1, T - 40 * MIN, 700_000, 100_000),
                   opn(101, "0xbig2", "BTC", -1, T - 39 * MIN, 700_000, 100_000)]
            crowd = [opn(1, "0xa", "BTC", 1, T - 2 * MIN, 5000, 101_000), opn(2, "0xb", "BTC", 1, T, 5000, 101_000)]

            def price(c: str, ts: int) -> float:   # до T−45 мин — 103 000: MA30 выше цены, шорт разрешён
                if ts < T - 45 * MIN:
                    return 103_000.0
                return 100_000.0 if ts < T - 5 * MIN else 101_000.0 if ts < T + 60 * MIN else 99_000.0

            r = self.go(big + crowd, price)
            self.assertEqual([(x.side, x.why) for x in r["trades"]], [(-1, "тейк")])
            self.assertEqual(r["trades"][0].wallets, ("0xbig1", "0xbig2"), "с крупными, не против толпы лонгов")

    def test_stop_from_decision_price(self) -> None:
        """Стоп — 1,5 % от цены решения (99 000 → 97 515), как у живого бота,
        а не от цены входа с проскальзыванием (99 049,5 → 97 563,76): провал
        до 97 540 — не стоп, потом тейк."""
        def price(c: str, ts: int) -> float:
            if T + 17 * MIN <= ts < T + 20 * MIN:
                return 97_540.0
            return 100_600.0 if ts >= T + 60 * MIN else 99_000.0

        r = self.go(self.whales(), price)
        [t] = r["trades"]
        self.assertAlmostEqual(t.entry_px, 99_049.5)
        self.assertEqual(t.why, "тейк")

    def test_stall_inside_candle(self) -> None:
        """Замедление внутри свечи: максимум +0,9 %, потом 25 минут ниже +0,8 %,
        свеча открылась ниже, но внутри дошла до +0,85 % — выход по +0,8 %
        (100 800), а не стоп потом."""
        def price(c: str, ts: int) -> float:
            if ts < T + 10 * MIN:
                return 100_000.0
            if ts < T + 15 * MIN:
                return 100_900.0
            if T + 44 * MIN <= ts < T + 45 * MIN:
                return 100_850.0
            return 98_000.0 if ts >= T + 180 * MIN else 100_500.0

        r = self.go(self.whales(), price)
        [t] = r["trades"]
        self.assertEqual((t.why, t.exit_px, t.exit_t), ("замедление", 100_800.0, T + 40 * MIN))

    def test_big_rechecked_at_decision(self) -> None:
        """«Крупные против толпы» — на момент решения: сигнал крупных шортов
        против толпы лонгов, а к решению (через 6 минут) лонги вышли из окна —
        это обычная толпа: против неё, лонг (без пересчёта был бы шорт)."""
        with mock.patch.object(rp.lv, "AGAINST", True):
            crowd = [opn(1, "0xa", "BTC", 1, T - 116 * MIN, 5000, 100_000), opn(2, "0xb", "BTC", 1, T - 115 * MIN, 5000, 100_000)]
            big = [opn(100, "0xbig1", "BTC", -1, T - 2 * MIN, 700_000, 100_000),
                   opn(101, "0xbig2", "BTC", -1, T, 700_000, 100_000)]

            def price(c: str, ts: int) -> float:
                return 99_000.0 if ts < T - 60 * MIN else 101_600.0 if ts >= T + 60 * MIN else 100_000.0

            r = self.go(crowd + big, price, delay=360)
            self.assertEqual([(x.side, x.why) for x in r["trades"]], [(1, "тейк")])

    def test_two_stops_pause_two_hours(self) -> None:
        """Два стопа подряд — входов нет 2 часа (как живой бот): BTC и ETH по
        стопу, сигнал SOL через 30 минут — «loss_pause»; DOGE через 3 часа — вход."""
        rows = [*self.whales(), *self.whales(coin="ETH", px=3000, t=T + 10 * MIN, base=10),
                *self.whales(coin="SOL", px=150, t=T + 90 * MIN, base=20),
                *self.whales(coin="DOGE", px=0.2, t=T + 240 * MIN, base=30)]

        def price(c: str, ts: int) -> float:
            base = self.PX[c]
            if c in ("BTC", "ETH") and T + 30 * MIN <= ts < T + 60 * MIN:
                return base * 0.98                       # обе — по стопу
            return base

        r = self.go(rows, price)
        self.assertEqual([(x.coin, x.why) for x in r["trades"][:2]], [("BTC", "стоп"), ("ETH", "стоп")])
        self.assertEqual(r["skips"].get("loss_pause"), 1, "SOL — пауза")
        self.assertEqual([x.coin for x in r["trades"]], ["BTC", "ETH", "DOGE"])

    def test_whales_average_with_float_noise_enters(self) -> None:
        """Средняя цена китов 99 999,99999999999 (12 345 $ и 50 000 $ по
        100 000) при цене 100 000 — не «хуже китов»: вход (как у живого бота)."""
        rows = [opn(1, "0xa", "BTC", 1, T - 2 * MIN, 12_345, 100_000), opn(2, "0xb", "BTC", 1, T, 50_000, 100_000)]
        r = self.go(rows, step_price(100_000, [(T + 60 * MIN, 0.016)]))
        self.assertEqual([t.why for t in r["trades"]], ["тейк"])
        self.assertNotIn("chase", r["skips"])

    def test_market_entry_never_worse_than_chase_bound(self) -> None:
        """По рынку, но не хуже средней цены китов (IOC с границей у живого
        бота): цена 99 980 при китах по 100 000 — проскальзывание 0,05 % дало
        бы 100 029,99, вход — по границе 100 000; с допуском 0,3 %: 100 290 —
        по границе 100 300, а не 100 340."""
        r = self.go(self.whales(), step_price(99_980, [(T + 60 * MIN, 0.016)]))
        [t] = r["trades"]
        self.assertAlmostEqual(t.entry_px, 100_000.0, places=6)
        os.remove(self.db)
        self.cache += "2"
        r = self.go(self.whales(), step_price(99_900, [(T + 60 * MIN, 0.016)]))
        [t] = r["trades"]
        self.assertAlmostEqual(t.entry_px, 99_900 * 1.0005, places=6, msg="в пределах границы — с проскальзыванием")
        os.remove(self.db)
        self.cache += "3"
        with mock.patch.object(rp.lv, "CHASE", 0.003):
            [t] = self.go(self.whales(), step_price(100_290, [(T + 60 * MIN, 0.016)]))["trades"]
        self.assertAlmostEqual(t.entry_px, 100_300.0, places=6)

    def test_coin_pause_three_hours(self) -> None:
        rows = [*self.whales(), *self.whales(t=T + 120 * MIN, base=10), *self.whales(t=T + 200 * MIN, base=20)]
        r = self.go(rows, step_price(100_000, [(T + 60 * MIN, 0.016), (T + 61 * MIN, 0.0),
                                               (T + 130 * MIN, 0.016), (T + 131 * MIN, 0.0)]))
        self.assertEqual(len(r["trades"]), 2, "вход в +0 мин, в +120 — пауза 3 ч, в +200 — снова")
        self.assertGreater(r["skips"]["coin_pause"], 0)

    def test_no_okx_coin_skipped(self) -> None:
        r = self.go(self.whales(coin="NOPE", px=1), lambda c, t: 1.0)
        self.assertEqual(r["trades"], [])
        self.assertGreater(r["skips"]["no_inst"], 0)

    def test_second_run_from_cache_without_network(self) -> None:
        price = step_price(100_000, [(T + 60 * MIN, 0.016)])
        self.go(self.whales(), price)
        n = len(self.market.calls)
        self.assertGreater(n, 0)
        os.remove(self.db)
        r = self.go(self.whales(), price, offline=True)
        self.assertEqual(len(self.market.calls), 0, "офлайн — ни одного запроса")
        self.assertEqual(r["trades"][0].why, "тейк")

    def test_chase_whale_lost_and_stale_skipped(self) -> None:
        r = self.go(self.whales(), lambda c, t: 100_400.0)            # цена ушла на 0,4 % — не догоняем
        self.assertEqual(r["trades"], [])
        self.assertGreater(r["skips"]["chase"], 0)
        os.remove(self.db)
        self.cache += "2"  # свои свечи у каждого случая
        rows = [*self.whales(), cls(10, "0xa", "BTC", 1, T - MIN, -50.0)]   # кит сигнала уже вышел в убыток — входим
        rows[1] = opn(2, "0xb", "BTC", 1, T, 50_000, 100_000)
        r = self.go(rows, lambda c, t: 100_000.0)
        self.assertEqual(len(r["trades"]), 1)
        self.assertNotIn("whale_exit", r["skips"])
        os.remove(self.db)
        self.cache += "3"
        # Кит A — за 2 часа минус 2 секунды до кита B: на момент решения (+5 с) он старше 2 часов.
        rows = [opn(1, "0xa", "BTC", 1, T - 120 * MIN + 2000, 50_000, 100_000), opn(2, "0xb", "BTC", 1, T, 50_000, 100_000)]
        r = self.go(rows, lambda c, t: 100_000.0)
        self.assertEqual(r["trades"], [])
        self.assertGreater(r["skips"]["stale"], 0)

    def test_network_failure_is_not_cached_forever(self) -> None:
        """Ревизия: сбой сети на блоке свечей раньше ложился в кеш пустым
        навсегда — следующие прогоны «не видели» цен этого блока."""
        make_db(self.db, self.whales())
        good = FakeMarket(step_price(100_000, [(T + 60 * MIN, 0.016)]))

        def flaky(body: dict):
            if body["okx"] == "/api/v5/market/history-candles":
                raise OSError("сеть")
            return good(body)

        r = rp.run(self.args(), fetch=flaky, now_ms=NOW)
        self.assertEqual(r["trades"], [])
        self.assertGreater(r["summary"]["data"]["errors"], 0)
        r = rp.run(self.args(), fetch=good, now_ms=NOW)
        self.assertEqual([t.why for t in r["trades"]], ["тейк"], "сеть вернулась — цены есть")

    def test_corrupted_cache_is_cleaned(self) -> None:
        price = step_price(100_000, [(T + 60 * MIN, 0.016)])
        self.go(self.whales(), price)
        path = os.path.join(self.cache, "replay_BTC-USDT-SWAP_5m.json")
        with open(path, encoding="utf-8") as f:
            raw = json.load(f)
        for k in raw:
            raw[k] = [["мусор"], [1, 2, 3], [1, 2, 3, 4, float("nan")], *raw[k]]  # мусор — первым в блоке
        raw["не число"] = [[1, 2, 3, 4, 5]]
        with open(path, "w", encoding="utf-8") as f:
            json.dump(raw, f)
        os.remove(self.db)
        r = self.go(self.whales(), price, offline=True)
        self.assertEqual([t.why for t in r["trades"]], ["тейк"])

    def test_same_signal_as_live_bot(self) -> None:
        """Повтор и живой бот на одних и тех же сделках китов видят один и тот
        же сигнал (общий код Signals)."""
        rows = self.whales()
        make_db(self.db, rows)
        live = lv.Signals(self.db).poll(T + 1000)
        rep = rp.lv.Signals(self.db).feed([(r[0], r[1], r[2], r[3], r[4], r[5], float(r[6])) for r in rows])
        self.assertEqual([(s["coin"], s["side"], s["whales"]) for s in live][-1:],
                         [(s["coin"], s["side"], s["whales"]) for s in rep][-1:])


if __name__ == "__main__":
    r = unittest.main(exit=False, verbosity=1).result
    if r.wasSuccessful():
        print(f"autotrade_replay: {r.testsRun} проверок повтора по правилам живого бота — без сети")
    sys.exit(0 if r.wasSuccessful() else 1)
