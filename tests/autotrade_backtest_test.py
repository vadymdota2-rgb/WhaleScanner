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

    def __call__(self, body: dict):
        self.calls += 1
        if body["type"] != "candleSnapshot":
            return []
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
        f.write(self.db)
        sigs = bt.load_signals(bt.open_ro(self.db), bt.STOCK_DEXES)
        self.assertEqual(len(sigs), 1)
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
        calls = api.calls
        r2 = bt.run(args(self.db, self.out, self.cache, "--min-train-trades", "0"), fetch=api, now_ms=NOW)
        self.assertEqual(r2["requests"], 1)  # свечи — из кеша; фандинга нет нигде — спросили один раз
        self.assertLessEqual(api.calls - calls, 1)

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
        tr = bt.simulate(sigs, paths, bt.Params(0, f, bt.Exits(0, 0, 0, False)), 0, 0, 1000)
        # 10-я минута — пауза 60 мин; 100-я — позиция ещё открыта (300 мин); 400-я — можно.
        self.assertEqual([t.sig.ts for t in tr], [T0, T0 + 400 * MIN])
        tr = bt.simulate(sigs, paths, bt.Params(0, f, bt.Exits(0, 0, 60, False)), 0, 0, 1000)
        self.assertEqual([t.sig.ts for t in tr], [T0, T0 + 100 * MIN, T0 + 400 * MIN])

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
                 "--min-notional", "20000", "--pnl30", "0", "--cooldown", "0", "--fee", "0.045"]
        r = bt.run(args(self.db, self.out, self.cache, *extra), fetch=self.scenario(0.02), now_ms=NOW)
        self.assertTrue(r["verdict"], r["text"])
        self.assertIn("МОЖНО ДАЛЬШЕ", r["text"])
        for name in ("report.txt", "trades.csv", "grid.csv", "top10.csv"):
            self.assertTrue(os.path.getsize(os.path.join(self.out, name)) > 0)
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
                 "--min-notional", "20000", "--pnl30", "0", "--cooldown", "0"]
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
