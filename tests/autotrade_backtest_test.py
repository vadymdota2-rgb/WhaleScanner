#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Бэктест автокопирования (tools/autotrade_backtest.py) — без сети и без базы бота.

База — синтетическая во временном каталоге (схема hl_fills как у бота),
свечи отдаёт подменённый «API» по заданной формуле цены. Проверяется то, от
чего зависит вердикт: нет заглядывания вперёд, стоп раньше тейка в одной
свече, комиссии с обеих сторон, фандинг, пауза и одна позиция на бумагу,
отложенные данные не участвуют в подборе, база — только чтение, в каталог
бота ничего не пишется, без сети — ни одного запроса.
"""
import hashlib
import math
import os
import sqlite3
import sys
import tempfile
import unittest
from typing import ClassVar

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import autotrade_backtest as bt

NANOS = 1_000_000_000
T0 = 1_800_000_000_000  # мс, начало синтетики
MIN = 60_000
NOW = T0 + 40 * 86_400_000

SCHEMA = """CREATE TABLE hl_fills (tid INTEGER PRIMARY KEY, wallet TEXT NOT NULL, coin TEXT NOT NULL DEFAULT '',
  dir TEXT NOT NULL DEFAULT '', side TEXT NOT NULL DEFAULT '', dir_code INTEGER NOT NULL DEFAULT 0,
  px TEXT NOT NULL DEFAULT '', sz TEXT NOT NULL DEFAULT '', closed_pnl_nanos INTEGER NOT NULL DEFAULT 0,
  fee_nanos INTEGER NOT NULL DEFAULT 0, notional_nanos INTEGER NOT NULL DEFAULT 0, margin_nanos INTEGER NOT NULL DEFAULT 0,
  leverage INTEGER NOT NULL DEFAULT 0, oid INTEGER NOT NULL DEFAULT 0, account_value_nanos INTEGER NOT NULL DEFAULT 0,
  ts INTEGER NOT NULL DEFAULT 0, hash TEXT NOT NULL DEFAULT '', flat INTEGER NOT NULL DEFAULT 0, start_pos_nanos INTEGER);
CREATE TABLE hl_funding_rate (coin TEXT NOT NULL, hour_ts INTEGER NOT NULL, rate_nanos INTEGER NOT NULL DEFAULT 0,
  mark_nanos INTEGER NOT NULL DEFAULT 0, oi_nanos INTEGER NOT NULL DEFAULT 0, day_vlm_nanos INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (coin, hour_ts));"""


class Fills:
    def __init__(self) -> None:
        self.rows: list[tuple] = []

    def add(self, wallet, coin, code, ts, notional=50_000.0, oid=0, account=500_000.0, lev=3, pnl=0.0):
        self.rows.append((len(self.rows) + 1, wallet, coin, code, int(notional * NANOS), oid, int(account * NANOS), lev,
                          ts, int(pnl * NANOS)))

    def write(self, path: str, funding=()) -> None:
        con = sqlite3.connect(path)
        con.executescript(SCHEMA)
        con.executemany("INSERT INTO hl_fills(tid, wallet, coin, dir_code, notional_nanos, oid, account_value_nanos, "
                        "leverage, ts, closed_pnl_nanos) VALUES(?,?,?,?,?,?,?,?,?,?)", self.rows)
        con.executemany("INSERT INTO hl_funding_rate(coin, hour_ts, rate_nanos) VALUES(?,?,?)", funding)
        con.commit()
        con.close()


class FakeApi:
    """candleSnapshot по формуле цены; как настоящий — 500 свечей на ответ и
    только 5000 последних каждого интервала."""

    def __init__(self, price) -> None:
        self.price = price
        self.calls = 0
        self.candle_calls = 0

    def __call__(self, body: dict):
        self.calls += 1
        if body.get("type") != "candleSnapshot":
            return []
        self.candle_calls += 1
        req = body["req"]
        sec = dict(bt.INTERVALS)[req["interval"]]
        step = sec * 1000
        start = max(req["startTime"], NOW - bt.KEEP_CANDLES * step) // step * step
        out = []
        t = start
        while t < req["endTime"] and t < NOW and len(out) < 500:
            pts = [self.price(req["coin"], t + k * step // 4) for k in range(5)]
            out.append({"t": t, "T": t + step - 1, "o": str(pts[0]), "c": str(pts[-1]), "h": str(max(pts)),
                        "l": str(min(pts)), "i": req["interval"], "s": req["coin"], "n": 1, "v": "1"})
            t += step
        return out


def args(db: str, out: str, cache: str, *extra: str):
    a = bt.parser().parse_args(["--db", db, "--out", out, "--cache", cache, "--pause", "0", *extra])
    return a


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = self.tmp.name
        self.bot = os.path.join(root, "bot")
        os.makedirs(self.bot)
        self.db = os.path.join(self.bot, "hyperliquid.db")
        self.out = os.path.join(root, "out")
        self.cache = os.path.join(root, "cache")

    def tearDown(self) -> None:
        self.tmp.cleanup()


class DataTest(Base):
    def test_orders_history_and_no_lookahead(self):
        f = Fills()
        ts = T0 + 10 * 86_400_000
        # Один ордер кита — три куска (один oid): один сигнал, объём — сумма.
        for k in range(3):
            f.add("w1", "xyz:TSLA", bt.OPEN_LONG, ts + k, notional=10_000, oid=77)
        # Закрытый PnL: за 40 дней до (не считается), за 5 дней до (считается),
        # после сигнала (не считается — это было бы заглядывание вперёд).
        f.add("w1", "BTC", bt.CLOSE_LONG, ts - 40 * 86_400_000, pnl=1000)
        f.add("w1", "BTC", bt.CLOSE_LONG, ts - 5 * 86_400_000, pnl=-300)
        f.add("w1", "xyz:TSLA", bt.CLOSE_SHORT, ts + 5 * MIN, pnl=999)   # закрыт шорт — не наш лонг
        f.add("w1", "xyz:TSLA", bt.CLOSE_LONG, ts + 9 * MIN, pnl=5000)
        f.add("w2", "BTC", bt.OPEN_LONG, ts)          # не акция
        f.add("w3", "abc:TSLA", bt.OPEN_SHORT, ts)    # чужая площадка
        f.add("w4", "@107", bt.OPEN_LONG, ts)         # спот — не фьючерс
        f.add("w5", "PURR/USDC", bt.OPEN_LONG, ts)    # спот
        f.write(self.db)
        sigs = bt.load_signals(bt.open_ro(self.db), bt.STOCK_DEXES)
        self.assertEqual(len(sigs), 1)
        every = bt.load_signals(bt.open_ro(self.db), None)  # все фьючерсы: крипта, акции, металлы
        self.assertEqual(sorted(x.coin for x in every), ["BTC", "abc:TSLA", "xyz:TSLA"])
        s = sigs[0]
        self.assertEqual((s.coin, s.side, s.ts), ("xyz:TSLA", 1, ts))
        self.assertAlmostEqual(s.notional, 30_000)
        self.assertAlmostEqual(s.pnl30, -300)
        self.assertEqual(s.whale_close, ts + 9 * MIN)

    def test_db_is_read_only_and_bot_dir_untouched(self):
        f = Fills()
        f.add("w1", "xyz:TSLA", bt.OPEN_LONG, T0)
        f.write(self.db)
        con = bt.open_ro(self.db)
        with self.assertRaises(sqlite3.OperationalError):
            con.execute("DELETE FROM hl_fills")
        def digest() -> str:
            with open(self.db, "rb") as fh:
                return hashlib.sha256(fh.read()).hexdigest()

        before = digest()
        listing = sorted(os.listdir(self.bot))
        bt.run(args(self.db, self.out, self.cache, "--offline", "--min-train-trades", "0"), now_ms=NOW)
        self.assertEqual(digest(), before)
        self.assertEqual(sorted(os.listdir(self.bot)), listing)
        for where in ("--out", "--cache"):
            a = args(self.db, self.out, self.cache)
            setattr(a, where[2:], os.path.join(self.bot, "x"))
            with self.assertRaises(SystemExit):
                bt.run(a, now_ms=NOW)

    def test_offline_never_calls_network_and_cache_is_reused(self):
        f = Fills()
        f.add("w1", "xyz:TSLA", bt.OPEN_LONG, NOW - 2 * 86_400_000)
        f.write(self.db)
        api = FakeApi(lambda _c, _t: 100.0)

        def boom(_body):
            raise AssertionError("сеть в режиме --offline")

        bt.run(args(self.db, self.out, self.cache, "--offline", "--min-train-trades", "0"), fetch=boom, now_ms=NOW)
        r1 = bt.run(args(self.db, self.out, self.cache, "--min-train-trades", "0"), fetch=api, now_ms=NOW)
        self.assertGreater(r1["requests"], 0)
        candles = api.candle_calls
        self.assertGreater(candles, 0)
        bt.run(args(self.db, self.out, self.cache, "--min-train-trades", "0"), fetch=api, now_ms=NOW)
        self.assertEqual(api.candle_calls, candles)  # свечи второй раз — из кеша

    def test_network_garbage_does_not_crash(self):
        f = Fills()
        f.add("w1", "xyz:TSLA", bt.OPEN_LONG, NOW - 86_400_000)
        f.write(self.db)
        for bad in (lambda _b: {"error": "x"}, lambda _b: [1, "x", None, {"t": "1"}],
                    lambda _b: [{"t": 1, "o": "x", "h": None}], lambda _b: (_ for _ in ()).throw(TimeoutError())):
            r = bt.run(args(self.db, self.out, self.cache + str(id(bad)), "--min-train-trades", "0"), fetch=bad, now_ms=NOW)
            self.assertFalse(r["verdict"])


def market(prices: list[tuple[int, float, float, float, float]]) -> bt.Market:
    return bt.Market({"1m": prices})


class OkxTest(Base):
    """Бумаги китов на OKX: имя — только кандидат, пару подтверждает цена.
    Масштаб kPEPE, замены имён (GOLD → XAU), «другой актив с тем же именем»
    (xyz:STX — акции Seagate, STX на OKX — токен Stacks), отсутствие."""

    INSTS: ClassVar[list] = [{"instId": i, "state": "live"} for i in
             ("BTC-USDT-SWAP", "PEPE-USDT-SWAP", "XAU-USDT-SWAP", "TSLA-USDT-SWAP", "STX-USDT-SWAP")]
    LAST: ClassVar[dict] = {"BTC-USDT-SWAP": 90_000, "PEPE-USDT-SWAP": 0.00001, "XAU-USDT-SWAP": 4_000, "TSLA-USDT-SWAP": 400,
            "STX-USDT-SWAP": 0.3}
    MIDS: ClassVar[dict] = {"": {"BTC": "90100", "kPEPE": "0.01"},
                            "xyz": {"xyz:GOLD": "3990", "xyz:TSLA": "401", "xyz:STX": "750", "xyz:VIX": "18"}}

    def okx_api(self, candles=None):
        def fetch(body):
            path = body.get("okx")
            if path == "/api/v5/public/instruments":
                return self.INSTS
            if path == "/api/v5/market/tickers":
                return [{"instId": k, "last": str(v)} for k, v in self.LAST.items()]
            if body.get("type") == "allMids":
                return self.MIDS.get(body["dex"], {})
            if path == "/api/v5/market/history-candles" and candles:
                return candles(body["params"])
            return []
        return fetch

    def test_candidates(self):
        self.assertEqual(bt.okx_candidate("kPEPE"), ("PEPE", 1000.0))
        self.assertEqual(bt.okx_candidate("xyz:GOLD"), ("XAU", 1.0))
        self.assertEqual(bt.okx_candidate("xyz:TSLA"), ("TSLA", 1.0))
        self.assertEqual(bt.okx_candidate("BTC"), ("BTC", 1.0))
        self.assertEqual(bt.okx_candidate("xyz:kAPPA"), ("kAPPA", 1.0))  # у акций «k» — часть имени

    def test_map_checks_price_not_just_name(self):
        md = bt.MarketData(self.cache, False, self.okx_api(), 0, NOW)
        m = bt.okx_map(md, ["BTC", "kPEPE", "xyz:GOLD", "xyz:TSLA", "xyz:STX", "xyz:VIX", "xyz:NEW"])
        self.assertEqual({k: v["status"] for k, v in m.items()},
                         {"BTC": "ok", "kPEPE": "ok", "xyz:GOLD": "ok", "xyz:TSLA": "ok", "xyz:STX": "mismatch",
                          "xyz:VIX": "missing", "xyz:NEW": "missing"})
        self.assertEqual((m["xyz:GOLD"]["inst"], m["kPEPE"]["scale"]), ("XAU-USDT-SWAP", 1000.0))
        # Сутки — из кеша, без сети; offline — тоже из кеша.
        n = md.requests
        self.assertEqual(bt.okx_map(md, ["BTC"])["BTC"]["status"], "ok")
        self.assertEqual(md.requests, n)
        off = bt.MarketData(self.cache, True, None, 0, NOW)
        self.assertEqual(bt.okx_map(off, ["xyz:STX", "ZZZ"]), {"xyz:STX": m["xyz:STX"], "ZZZ": {"status": "unverified"}})

    def test_backtest_on_okx_uses_okx_prices_and_drops_the_rest(self):
        f = Fills()
        ts = NOW - 2 * 86_400_000
        f.add("a", "kPEPE", bt.OPEN_SHORT, ts, notional=30_000, oid=1)
        f.add("b", "xyz:STX", bt.OPEN_LONG, ts + MIN, notional=30_000, oid=2)  # другой актив на OKX
        f.add("c", "xyz:VIX", bt.OPEN_LONG, ts + 2 * MIN, notional=30_000, oid=3)  # нет на OKX
        f.write(self.db)
        con = sqlite3.connect(self.db)
        con.execute("UPDATE hl_fills SET sz = '3000000' WHERE oid=1")  # 30 000 $ / 3 000 000 kPEPE = 0,01
        con.commit()
        con.close()
        asked = []

        def candles(params):
            asked.append(params["instId"])
            after = int(params["after"])
            step = bt.OKX_BAR_SEC * 1000
            return [[str(t), "0.00001", "0.0000102", "0.0000099", "0.00001"]
                    for t in range((after - 1) // step * step, after - 100 * step, -step) if t >= NOW - 5 * 86_400_000]

        r = bt.run(args(self.db, self.out, self.cache, "--venue", "okx", "--min-train-trades", "0",
                        "--delays", "5", "--chase", "0.5", "--cluster", "1", "--max-hold", "60", "--tp", "1",
                        "--pnl30", "0"),
                   fetch=self.okx_api(candles), now_ms=NOW)
        self.assertEqual(set(asked), {"PEPE-USDT-SWAP"})
        self.assertEqual(r["signals"], 1)
        self.assertIn("Торговля — на OKX", r["text"])
        self.assertIn("другой актив с тем же именем (цена не сходится): xyz:STX", r["text"])
        self.assertIn("нет на OKX: xyz:VIX", r["text"])
        self.assertIn("Комиссия тейкера 0.05 %", r["text"])
        self.assertEqual(r["summary"]["okx"]["map"]["kPEPE"]["inst"], "PEPE-USDT-SWAP")
        # Шорт кита по 0,01 за kPEPE = 0,00001 за PEPE; на OKX — 0,00001: вход
        # не хуже китов. Без пересчёта масштаба цена казалась бы на 99,9 %
        # «ниже китов» — и шорт пропускался бы как поздний.
        self.assertEqual(r["summary"]["full"]["trades"], 1)


class ClusterTest(Base):
    def test_whales_together_and_their_average_price(self):
        """Сговор: разные кошельки в одну сторону на одной бумаге за окно —
        счёт, общий объём, средняя цена по объёму; повтор того же кошелька
        не считается вторым китом; старше окна и другая сторона — не в счёт."""
        f = Fills()
        h = 3_600_000
        f.add("a", "xyz:GOOGL", bt.OPEN_LONG, T0, notional=81_800, oid=1)
        f.add("b", "xyz:GOOGL", bt.OPEN_LONG, T0 + h, notional=10_000, oid=2)
        f.add("a", "xyz:GOOGL", bt.OPEN_LONG, T0 + 2 * h, notional=1_000, oid=3)
        f.add("c", "xyz:GOOGL", bt.OPEN_SHORT, T0 + 2 * h, notional=5_000, oid=4)
        f.add("d", "xyz:GOOGL", bt.OPEN_LONG, T0 + 30 * h, notional=4_000, oid=5)
        f.write(self.db)
        con = sqlite3.connect(self.db)
        con.execute("UPDATE hl_fills SET sz = CAST(notional_nanos AS REAL) / 1e9 / CASE oid WHEN 1 THEN 272.5 "
                    "WHEN 2 THEN 270.87 ELSE 271.0 END")
        con.commit()
        con.close()
        sigs = bt.load_signals(bt.open_ro(self.db), None, cluster_ms=24 * h)
        got = [(s.wallet, s.side, s.wallets, round(s.cluster_notional)) for s in sigs]
        self.assertEqual(got, [("a", 1, 1, 81_800), ("b", 1, 2, 91_800), ("a", 1, 2, 92_800),
                               ("c", -1, 1, 5_000), ("d", 1, 1, 4_000)])
        self.assertAlmostEqual(sigs[0].px, 272.5)
        self.assertAlmostEqual(sigs[1].ref_px, (81_800 + 10_000) / (81_800 / 272.5 + 10_000 / 270.87), places=6)
        self.assertTrue(bt.Filters(90_000, 0, 0, False, 0, 2).ok(sigs[1]))
        self.assertFalse(bt.Filters(90_000, 0, 0, False, 0, 3).ok(sigs[1]))
        self.assertFalse(bt.Filters(95_000, 0, 0, False, 0, 2).ok(sigs[1]))


class OwnerRulesTest(Base):
    def test_two_whales_each_from_1000(self):
        """Правило владельца: минимум 2 кита в одну сторону, у каждого позиция
        от 1000 $ (все его ордера за окно). Мелкий кит не считается; два
        ордера одного по 600 $ — это 1200 $, он считается."""
        f = Fills()
        h = 3_600_000
        f.add("big", "xyz:GOOGL", bt.OPEN_LONG, T0, notional=81_800, oid=1)
        f.add("tiny", "xyz:GOOGL", bt.OPEN_LONG, T0 + h, notional=200, oid=2)       # мелочь — не кит
        f.add("two", "xyz:GOOGL", bt.OPEN_LONG, T0 + 2 * h, notional=600, oid=3)
        f.add("two", "xyz:GOOGL", bt.OPEN_LONG, T0 + 3 * h, notional=600, oid=4)    # 1200 $ — уже кит
        f.write(self.db)
        sigs = bt.load_signals(bt.open_ro(self.db), None, cluster_ms=24 * h, min_whale=1000)
        self.assertEqual([s.wallets for s in sigs], [1, 1, 1, 2])
        rule = bt.Filters(0, 0, 0, False, 0, 2)
        self.assertEqual([rule.ok(s) for s in sigs], [False, False, False, True])

    def test_defaults_are_the_owner_rules(self):
        a = bt.parser().parse_args([])
        self.assertEqual((a.tp, a.sl, a.hold, a.whale_exit, a.min_whale, a.margin, a.markets),
                         ("1", "0", "0", "0", 1000, "cross", "all"))
        self.assertTrue(all(k >= 2 for k in bt.int_list(a.cluster)))
        self.assertEqual((a.cluster_hours, a.chase), (2, "0.2,0.3,0.5"))  # 2 часа; предел догона 0,2–0,5 %
        self.assertEqual((a.leverage, a.margin_pct, a.max_open), (10, 10, 1))


class ExitTest(unittest.TestCase):
    def sig(self, side=1, ts=T0):
        return bt.Signal("w", "xyz:A", side, ts, 50_000, 1e6, 2)

    def flat(self, n=600, px=100.0):
        return [(T0 + k * MIN, px, px, px, px) for k in range(n)]

    def test_stop_wins_when_both_in_one_candle(self):
        c = self.flat()
        c[10] = (T0 + 10 * MIN, 100, 105, 95, 100)  # и тейк 2 %, и стоп 2 % — в одной свече
        p = bt.build_path(self.sig(), 0, market(c), 300 * MIN, [0], 0.0, {})
        t, pct, why = bt.exit_trade(p, 1, bt.Exits(0.02, 0.02, 0, False), 0.0, 0.0)
        self.assertEqual(why, "стоп")
        self.assertAlmostEqual(pct, -0.02)
        self.assertEqual(t, T0 + 10 * MIN)

    def test_take_profit_short_and_fees_both_sides(self):
        c = self.flat()
        c[20] = (T0 + 20 * MIN, 100, 100, 96, 97)
        p = bt.build_path(self.sig(-1), 0, market(c), 300 * MIN, [0], 0.0, {})
        _t, pct, why = bt.exit_trade(p, -1, bt.Exits(0.03, 0, 0, False), 0.0, 0.001)
        self.assertEqual(why, "тейк")
        self.assertAlmostEqual(pct, 0.03 - 0.002)

    def test_entry_candle_counts_only_adverse_move(self):
        c = self.flat()
        c[0] = (T0, 100, 110, 99.5, 100)  # всплеск вверх в свече входа — не наш тейк
        p = bt.build_path(self.sig(), 0, market(c), 300 * MIN, [0], 0.0, {})
        _t, _pct, why = bt.exit_trade(p, 1, bt.Exits(0.05, 0, 0, False), 0.0, 0.0)
        self.assertEqual(why, "предел удержания")
        _t, _pct, why = bt.exit_trade(p, 1, bt.Exits(0, 0.004, 0, False), 0.0, 0.0)
        self.assertEqual(why, "стоп")

    def test_slippage_delay_time_exit_and_whale_close(self):
        c = [(T0 + k * MIN, 100 + k, 100 + k + 1, 100 + k, 100 + k + 1) for k in range(600)]
        s = self.sig()
        s.whale_close = T0 + 30 * MIN
        p = bt.build_path(s, 30, market(c), 300 * MIN, [60], 0.001, {})
        self.assertAlmostEqual(p.entry_px, 100.5 * 1.001)  # середина первой минуты + проскальзывание
        t, _pct, why = bt.exit_trade(p, 1, bt.Exits(0, 0, 60, True), 0.0, 0.0)
        self.assertEqual((why, t), ("кит закрыл", T0 + 30 * MIN + 30_000))
        t, _pct, why = bt.exit_trade(p, 1, bt.Exits(0, 0, 60, False), 0.0, 0.0)
        self.assertEqual((why, t), ("время", T0 + 30_000 + 60 * MIN))

    def test_funding_long_pays_short_receives(self):
        c = self.flat()
        fund = {T0 + h * 3_600_000: 0.0001 for h in range(1, 10)}
        for side in (1, -1):
            p = bt.build_path(self.sig(side), 0, market(c), 300 * MIN, [0], 0.0, fund)
            _t, pct, _ = bt.exit_trade(p, side, bt.Exits(0, 0, 0, False), 0.0, 0.0)
            self.assertAlmostEqual(pct, -side * 5 * 0.0001)  # 5 часов удержания

    def test_coarse_candles_when_minutes_are_gone(self):
        hourly = [(T0 + k * 3_600_000, 100, 101, 99, 100) for k in range(48)]
        p = bt.build_path(self.sig(), 5, bt.Market({"1m": [], "1h": hourly}), 300 * MIN, [0], 0.0, {})
        self.assertEqual(p.interval, "1h")
        self.assertIsNone(bt.build_path(self.sig(ts=T0 + 47 * 3_600_000), 5, bt.Market({"1h": hourly}), 300 * MIN,
                                        [0], 0.0, {}))  # срок удержания за краем данных — сделки нет

    def test_cooldown_and_one_position_per_coin(self):
        c = self.flat(2000)
        sigs = [self.sig(ts=T0 + k * MIN) for k in (0, 10, 100, 400)]
        paths = {i: bt.build_path(s, 0, market(c), 300 * MIN, [0, 60], 0.0, {}) for i, s in enumerate(sigs)}
        f = bt.Filters(0, 0, 0, False, 60)
        sz = bt.Sizing(10_000, 0.1, 1, 0, 0.0)
        tr = bt.simulate(sigs, paths, bt.Params(0, f, bt.Exits(0, 0, 0, False)), 0, 0, sz)
        # 10-я минута — пауза 60 мин; 100-я — позиция ещё открыта (300 мин); 400-я — можно.
        self.assertEqual([t.sig.ts for t in tr], [T0, T0 + 400 * MIN])
        tr = bt.simulate(sigs, paths, bt.Params(0, f, bt.Exits(0, 0, 60, False)), 0, 0, sz)
        self.assertEqual([t.sig.ts for t in tr], [T0, T0 + 100 * MIN, T0 + 400 * MIN])

    def test_one_position_at_a_time_across_coins(self):
        c = self.flat(2000)
        sigs = [bt.Signal("w", coin, 1, T0 + k * MIN, 50_000, 1e6, 2)
                for coin, k in (("xyz:A", 0), ("xyz:B", 30), ("xyz:C", 61))]
        paths = {i: bt.build_path(s, 0, market(c), 300 * MIN, [0, 60], 0.0, {}) for i, s in enumerate(sigs)}
        prm = bt.Params(0, bt.Filters(0, 0, 0, False, 0), bt.Exits(0, 0, 60, False))
        one = bt.simulate(sigs, paths, prm, 0, 0, bt.Sizing(10_000, 0.1, 10, 1, 0.05))
        self.assertEqual([t.sig.coin for t in one], ["xyz:A", "xyz:C"])  # B — пока открыта A
        many = bt.simulate(sigs, paths, prm, 0, 0, bt.Sizing(10_000, 0.1, 10, 0, 0.05))
        self.assertEqual(len(many), 3)

    def test_liquidation_at_ten_x(self):
        """Маржа 10 % депозита, плечо 10×: позиция = депозит; поддерживающая
        маржа — половина начальной, ликвидация на ходе против нас 5 %:
        потеряна вся маржа (−10 % позиции) — 1000 $ из 10 000."""
        sz = bt.Sizing(10_000, 0.1, 10, 1, 0.05)
        self.assertAlmostEqual(sz.notional, 10_000)
        self.assertAlmostEqual(sz.liq_move, 0.05)
        c = self.flat()
        c[30] = (T0 + 30 * MIN, 100, 100, 94, 95)  # −6 %
        p = bt.build_path(self.sig(), 0, market(c), 300 * MIN, [0], 0.0, {})
        prm = bt.Params(0, bt.Filters(0, 0, 0, False, 0), bt.Exits(0, 0, 0, False))
        tr = bt.simulate([self.sig()], {0: p}, prm, 0.0, 0.001, sz)
        self.assertEqual(tr[0].why, "ликвидация")
        self.assertAlmostEqual(tr[0].pnl_usd, (-0.1 - 0.001) * 10_000)
        # Стоп 2 % — ближе ликвидации: сработает он, убыток 2 % позиции.
        prm = bt.Params(0, bt.Filters(0, 0, 0, False, 0), bt.Exits(0, 0.02, 0, False))
        tr = bt.simulate([self.sig()], {0: p}, prm, 0.0, 0.0, sz)
        self.assertEqual(tr[0].why, "стоп")
        self.assertAlmostEqual(tr[0].pnl_usd, -200)
        # Шорт: ликвидация на росте.
        up = self.flat()
        up[5] = (T0 + 5 * MIN, 100, 106, 100, 105)
        p = bt.build_path(self.sig(-1), 0, market(up), 300 * MIN, [0], 0.0, {})
        _t, pct, why = bt.exit_trade(p, -1, bt.Exits(0, 0, 0, False), 0.0, 0.0, 0.05, 0.1)
        self.assertEqual((why, pct), ("ликвидация", -0.1))

    def test_cross_margin_backs_position_with_whole_account(self):
        """Кросс-маржа, одна позиция 10 % × 10× = весь депозит: позицию держит
        весь счёт — ликвидация лишь при ходе против нас на 95 %; при двух
        позициях сразу на каждую — половина счёта."""
        one = bt.Sizing(10_000, 0.1, 10, 1, 0.05, cross=True)
        self.assertAlmostEqual(one.backing, 1.0)
        self.assertAlmostEqual(one.liq_move, 0.95)
        two = bt.Sizing(10_000, 0.1, 10, 2, 0.05, cross=True)
        self.assertAlmostEqual(two.liq_move, 0.45)
        c = self.flat()
        c[30] = (T0 + 30 * MIN, 100, 100, 50, 52)  # −50 %
        p = bt.build_path(self.sig(), 0, market(c), 300 * MIN, [0], 0.0, {})
        prm = bt.Params(0, bt.Filters(0, 0, 0, False, 0), bt.Exits(0, 0, 0, False))
        self.assertEqual(bt.simulate([self.sig()], {0: p}, prm, 0, 0, one)[0].why, "предел удержания")
        tr = bt.simulate([self.sig()], {0: p}, prm, 0, 0, two)[0]
        self.assertEqual((tr.why, tr.pnl_usd), ("ликвидация", -5_000))

    def test_chase_filter(self):
        """Лонг: цена ниже средней китов — входим всегда; выше больше чем на
        предел — поздно. Шорт — наоборот."""
        c = self.flat(px=101.0)
        f = bt.Filters(0, 0, 0, False, 0, 1, 0.005)
        prm = bt.Params(0, f, bt.Exits(0, 0, 0, False))
        sz = bt.Sizing(10_000, 0.1, 10, 0, 0.05, cross=True)
        for side, ref, enter in ((1, 100.0, False), (1, 100.6, True), (1, 110.0, True),
                                 (-1, 102.0, False), (-1, 101.4, True), (-1, 90.0, True)):
            s = bt.Signal("w", "xyz:A", side, T0, 50_000, 1e6, 2, px=ref)
            p = bt.build_path(s, 0, market(c), 300 * MIN, [0], 0.0, {})
            self.assertEqual(len(bt.simulate([s], {0: p}, prm, 0, 0, sz)), int(enter), (side, ref))
        # 0 — без предела.
        s = bt.Signal("w", "xyz:A", 1, T0, 50_000, 1e6, 2, px=50.0)
        p = bt.build_path(s, 0, market(c), 300 * MIN, [0], 0.0, {})
        self.assertEqual(len(bt.simulate([s], {0: p}, bt.Params(0, bt.Filters(0, 0, 0, False, 0), prm.exits),
                                         0, 0, sz)), 1)

    def test_filters(self):
        s = self.sig()
        s.pnl30 = -1
        self.assertTrue(bt.Filters(20_000, 0, 0, False, 0).ok(s))
        self.assertFalse(bt.Filters(60_000, 0, 0, False, 0).ok(s))
        self.assertFalse(bt.Filters(0, 0, 0, True, 0).ok(s))
        self.assertFalse(bt.Filters(0, 2e6, 0, False, 0).ok(s))
        self.assertFalse(bt.Filters(0, 0, 1, False, 0).ok(s))
        s.leverage = 0  # плечо неизвестно — при ограничении не проходит
        self.assertFalse(bt.Filters(0, 0, 5, False, 0).ok(s))

    def test_stats(self):
        def tr(usd, t):
            return bt.Trade(self.sig(), t, t, usd / 1000, usd, "", "1m")
        st = bt.stats([tr(100, 1), tr(-50, 2), tr(-70, 3), tr(200, 4), tr(-10, 5)])
        self.assertEqual((st.trades, st.wins), (5, 2))
        self.assertAlmostEqual(st.pf, 300 / 130)
        self.assertAlmostEqual(st.max_dd, 120)
        self.assertEqual((st.worst_streak, st.worst_streak_usd), (2, -120))
        self.assertTrue(math.isinf(bt.stats([tr(1, 1)]).pf))

    def test_us_session(self):
        # 2026-07-15 (среда, летнее время): 13:30 UTC = 9:30 Нью-Йорк — открыт; 13:29 — нет.
        july = 1_784_122_200_000
        self.assertTrue(bt.us_open(july))
        self.assertFalse(bt.us_open(july - 60_000))
        # 2026-01-14 (среда, зимнее): 14:30 UTC = 9:30 — открыт; 21:00 UTC = 16:00 — закрыт.
        jan = 1_768_401_000_000
        self.assertTrue(bt.us_open(jan))
        self.assertFalse(bt.us_open(jan + 390 * 60_000))
        self.assertFalse(bt.us_open(july + 3 * 86_400_000))  # суббота


class VerdictTest(Base):
    def scenario(self, edge: float, n: int = 400):
        """n сигналов за 20 дней; после каждого цена за 30 минут уходит на edge
        в сторону кита. Отложенные — последние 25 % по времени."""
        f = Fills()
        start = NOW - 20 * 86_400_000
        times = [start + k * (20 * 86_400_000 - 86_400_000) // n for k in range(n)]
        coins = ["xyz:A", "xyz:B", "io:C", "para:D"]
        for k, ts in enumerate(times):
            f.add(f"w{k % 7}", coins[k % 4], bt.OPEN_LONG, ts, notional=30_000, oid=k + 1)
        f.write(self.db)
        by_coin = {c: [ts for k, ts in enumerate(times) if coins[k % 4] == c] for c in coins}

        def price(coin, t):
            done = sum(min(max(t - ts, 0), 30 * MIN) / (30 * MIN) for ts in by_coin[coin])
            return 100 * (1 + edge) ** done

        return FakeApi(price)

    def test_real_edge_passes_and_none_fails(self):
        extra = ["--delays", "5,30", "--tp", "0,2", "--sl", "0,1", "--hold", "0,60", "--max-hold", "120",
                 "--min-notional", "20000", "--pnl30", "0", "--cooldown", "0", "--fee", "0.045", "--cluster", "1"]
        r = bt.run(args(self.db, self.out, self.cache, *extra), fetch=self.scenario(0.02), now_ms=NOW)
        self.assertTrue(r["verdict"], r["text"])
        self.assertIn("МОЖНО ДАЛЬШЕ", r["text"])
        for name in ("report.txt", "trades.csv", "grid.csv", "top10.csv", "summary.json"):
            self.assertTrue(os.path.getsize(os.path.join(self.out, name)) > 0)
        import json
        with open(os.path.join(self.out, "summary.json"), encoding="utf-8") as fh:
            sm = json.load(fh)
        self.assertTrue(sm["verdict"])
        self.assertEqual(sm["sizing"]["notional"], 10_000)
        self.assertEqual(len(sm["top"]), len(r["top"]))
        self.assertTrue(sm["equity"] and sm["trades"] and sm["by_coin"])
        # За китами лучше, чем против них: на этой цене против — в минус.
        self.assertIsNotNone(sm["fade"])
        self.assertLess(sm["fade"]["net"], sm["top"][0]["holdout"]["net"])
        self.assertIn("за китами лучше", r["text"])
        self.assertIn("По пределу догона", r["text"])
        self.assertTrue(sm["by_chase"] and {"chase", "train", "holdout"} <= set(sm["by_chase"][0]))
        self.assertLessEqual(len(sm["equity"]), 501)
        # Без преимущества — комиссии съедают всё.
        os.remove(self.db)
        r = bt.run(args(self.db, self.out, self.cache + "2", *extra), fetch=self.scenario(0.0), now_ms=NOW)
        self.assertFalse(r["verdict"])
        self.assertIn("НЕТ — дальше не идём", r["text"])

    def test_random_walk_never_passes(self):
        """Нулевая гипотеза: цена — случайное блуждание, киты ничего не
        знают, стороны случайны. Модель исполнения не должна рисовать
        прибыль из ничего: вердикт «нет», profit factor лучшего набора на
        отложенных < 1,3."""
        import array as arr
        import random
        rnd = random.Random(7)
        f = Fills()
        start = NOW - 20 * 86_400_000
        coins = [f"xyz:R{i}" for i in range(6)]
        for k in range(900):
            f.add(f"w{k % 9}", rnd.choice(coins), rnd.choice([bt.OPEN_LONG, bt.OPEN_SHORT]),
                  start + rnd.randrange(19 * 86_400_000), notional=30_000, oid=k + 1)
        f.write(self.db)
        base = NOW - 25 * 86_400_000
        walks: dict = {}

        def price(coin, t):
            if coin not in walks:
                g = random.Random(coin)
                w, v = arr.array("d"), 100.0
                for _ in range(26 * 1440 * 4):
                    v *= 1 + g.gauss(0, 0.0007)
                    w.append(v)
                walks[coin] = w
            return walks[coin][max(0, min(len(walks[coin]) - 1, (t - base) // 15_000))]

        extra = ["--delays", "5,120", "--tp", "0,1,4", "--sl", "0,1", "--hold", "0,60", "--max-hold", "240",
                 "--pnl30", "0", "--min-notional", "20000"]
        r = bt.run(args(self.db, self.out, self.cache, *extra), fetch=FakeApi(price), now_ms=NOW)
        self.assertFalse(r["verdict"], r["text"])
        self.assertLess(r["top"][0][2].pf, 1.3)

    def test_holdout_is_not_used_for_selection(self):
        """Преимущество есть только в первых 75 % — подбор его найдёт, а
        отложенные покажут, что дальше его нет: вердикт «нет»."""
        f = Fills()
        start = NOW - 20 * 86_400_000
        n = 400
        times = [start + k * (19 * 86_400_000) // n for k in range(n)]
        cut = times[0] + (times[-1] - times[0]) * 3 // 4
        for k, ts in enumerate(times):
            f.add("w", "xyz:A", bt.OPEN_LONG, ts, notional=30_000, oid=k + 1)
        f.write(self.db)

        def price(_coin, t):
            done = sum(min(max(t - ts, 0), 30 * MIN) / (30 * MIN) for ts in times if ts < cut)
            return 100 * 1.02 ** done

        extra = ["--delays", "5", "--tp", "0", "--sl", "0", "--hold", "60", "--max-hold", "60",
                 "--min-notional", "20000", "--pnl30", "0", "--cooldown", "0", "--cluster", "1"]
        r = bt.run(args(self.db, self.out, self.cache, *extra), fetch=FakeApi(price), now_ms=NOW)
        _prm, train, hold = r["top"][0]
        self.assertGreater(train.net, 0)
        self.assertLessEqual(hold.net, 0)
        self.assertFalse(r["verdict"])


if __name__ == "__main__":
    import io
    buf = io.StringIO()
    r = unittest.TextTestRunner(stream=buf, verbosity=0).run(unittest.defaultTestLoader.loadTestsFromModule(
        sys.modules[__name__]))
    if not r.wasSuccessful():
        sys.stderr.write(buf.getvalue())
        sys.exit(1)
    print(f"autotrade: бэктест — {r.testsRun} проверок без сети и без записи в базу")
