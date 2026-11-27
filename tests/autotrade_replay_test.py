#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Бэктест-повтор (tools/autotrade_replay.py) — на синтетической базе китов
и поддельных свечах OKX: без сети. Проверяется, что повтор торгует по тем же
правилам, что живой бот: вход от 2 китов, тейк +1 %, выход вслед за китом в
убытке, стоп −4 %, «входим с крупными», одна позиция, пауза на
бумагу, нет на OKX — пропуск; ход — в progress.json; итог — summary.json
для вкладки; второй прогон — из кеша, без сети.
"""
import argparse
import json
import os
import sqlite3
import sys
import tempfile
import unittest
from typing import ClassVar

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
        step = {"4H": 4 * 3_600_000, "1H": 3_600_000}.get(p.get("bar", ""), rp.BAR_MS)   # 4ч — для уровней
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
    def setUp(self) -> None:
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
        return [opn(base, "0xa", coin, side, t - 2 * MIN, 5000, px), opn(base + 1, "0xb", coin, side, t, 5000, px)]

    def test_take_profit(self) -> None:
        r = self.go(self.whales(), step_price(100_000, [(T + 60 * MIN, 0.012)]))
        [t] = r["trades"]
        self.assertEqual((t.why, t.coin, t.side), ("тейк", "BTC", 1))
        self.assertGreater(t.pnl, 0)
        # 5 % × 20 000 × 10 = 10 000 $ = 0,1 BTC лимитом по 100 000; тейк 101 000: +100 $
        # минус мейкер 0,02 % на входе и на выходе (без проскальзывания).
        self.assertAlmostEqual(t.pnl, 100 - 0.1 * (100_000 + 101_000) * 0.0002, delta=0.02)
        s = r["summary"]
        self.assertEqual((s["full"]["trades"], s["sizing"]["margin_pct"], s["trades"][0]["why"]), (1, 5.0, "тейк"))
        self.assertGreater(s["sizing"]["deposit_end"], s["sizing"]["deposit"])
        self.assertEqual(s["by_exit"][0]["key"], "тейк")
        with open(os.path.join(self.out, "progress.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["stage"], "готово")
        self.assertTrue(os.path.isfile(os.path.join(self.out, "summary.json")))

    def test_whale_loss_exit(self) -> None:
        rows = [*self.whales(), cls(10, "0xa", "BTC", 1, T + 30 * MIN, -100.0)]
        r = self.go(rows, step_price(100_000, [(T + 20 * MIN, -0.003)]))
        [t] = r["trades"]
        self.assertEqual(t.why, "кит вышел в убыток")
        self.assertLess(t.pnl, 0)
        self.assertGreaterEqual(t.exit_t, T + 30 * MIN)

    def test_whale_profit_close_keeps_position(self) -> None:
        rows = [*self.whales(), cls(10, "0xa", "BTC", 1, T + 30 * MIN, +100.0)]
        r = self.go(rows, step_price(100_000, [(T + 90 * MIN, 0.012)]))
        self.assertEqual(r["trades"][0].why, "тейк")

    def test_stop_beyond_level(self) -> None:
        """Стоп за уровнем входа (как у живого бота): за сутки до сигнала — 99 000
        и 103 000 (раньше окна MA30 — она ровная), цена 100 000 (лонг у поддержки) → стоп 99 000 × 0,995 = 98 505.
        Свеча открылась за стопом — по её открытию; задела стоп — по цене стопа;
        98 600 — держим; уровней не было — стопа нет."""
        H = 60 * MIN

        def hist(after):
            def f(c: str, t: int) -> float:
                if t < T:
                    return 99_000.0 if T - 16 * H <= t < T - 12 * H else 103_000.0 if T - 24 * H <= t < T - 20 * H else 100_000.0
                return after(t)
            return f

        r = self.go(self.whales(), hist(lambda t: 98_000.0 if t >= T + 20 * MIN else 100_000.0))
        [t] = r["trades"]
        self.assertEqual(t.why, "стоп")
        self.assertAlmostEqual(t.exit_px, 98_000 * 0.9995, delta=1, msg="свеча открылась за стопом — по её открытию")
        os.remove(self.db)
        self.cache += "2"
        r = self.go(self.whales(), hist(lambda t: 101_200.0 if t >= T + 90 * MIN else 98_600.0 if t >= T + 20 * MIN else 100_000.0))
        self.assertEqual([t.why for t in r["trades"]], ["тейк"], "98 600 — выше стопа")
        os.remove(self.db)
        self.cache += "3"
        r = self.go(self.whales(), hist(lambda t: 98_400.0 if T + 20 * MIN < t < T + 25 * MIN else 100_000.0))
        [t] = r["trades"]
        self.assertEqual(t.why, "стоп")
        self.assertAlmostEqual(t.exit_px, 98_505 * 0.9995, delta=1, msg="по цене стопа")
        os.remove(self.db)
        self.cache += "4"
        r = self.go(self.whales(), step_price(100_000, [(T + 20 * MIN, -0.014)]))   # сутки ровно 100 000 — уровней нет
        self.assertEqual([t.why for t in r["trades"]], ["открыта"], "уровней нет — стоп 1,5 %: −1,4 % — держим")
        os.remove(self.db)
        self.cache += "5"
        r = self.go(self.whales(), step_price(100_000, [(T + 20 * MIN, -0.016)]))
        [t] = r["trades"]
        self.assertEqual(t.why, "стоп", "уровней нет — стоп 1,5 % от входа")
        self.assertAlmostEqual(t.exit_px, 98_400 * 0.9995, delta=1, msg="свеча открылась за стопом")

    def test_variants_one_change_each(self) -> None:
        """--variants: те же киты по 7 вариантам правил (по одному изменению).
        Цена +1,2 %: тейк +0,5 % и +1 % — тейк, +1,5 % — не дошла (открыта).
        Без вариантов — список пуст, повтор один."""
        r = self.go(self.whales(), step_price(100_000, [(T + 60 * MIN, 0.012)]), variants=True)
        v = {x["name"]: x for x in r["summary"]["variants"]}
        self.assertEqual(list(v), ["текущие правила", *(x.name for x in rp.VARIANTS)])
        self.assertEqual((v["текущие правила"]["trades"], v["текущие правила"]["net"]), (1, r["summary"]["full"]["net"]))
        self.assertEqual((v["текущие правила"]["tp_pct"], v["текущие правила"]["stop_max_pct"]), (1.0, 1.5))
        self.assertEqual(v["тейк +1,5 %"]["open"], 1)
        self.assertEqual(v["тейк +0,5 %"]["open"], 0)
        self.assertLess(v["тейк +0,5 %"]["net"], v["текущие правила"]["net"])
        self.assertIsNone(v["стоп только за уровнем"]["stop_max_pct"])
        self.assertEqual((v["без уровней 4ч"]["levels"], v["без MA30"]["ma"]), (False, False))
        self.assertIn("Варианты (те же киты, одно изменение правил):", r["text"])
        self.assertIn("  тейк +1,5 %: сделок 1", r["text"])
        with open(os.path.join(self.out, "progress.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["stage"], "готово")
        os.remove(self.db)
        self.cache += "2"
        r = self.go(self.whales(), step_price(100_000, [(T + 60 * MIN, 0.012)]))
        self.assertEqual(r["summary"]["variants"], [])
        self.assertNotIn("Варианты", r["text"])

    def test_variants_stop_and_filters(self) -> None:
        """Поддержка далеко (стоп за ней — 89 550), цена дошла до 98 450:
        стоп до 1 % и 1,5 % — стоп; до 2,5 % и «только за уровнем» — сидим.
        Лонг у сопротивления — «без уровней 4ч» входит; MA30 выше цены —
        «без MA30» входит."""
        H = 60 * MIN

        def far(c: str, t: int) -> float:
            if t < T:
                return 90_000.0 if T - 16 * H <= t < T - 12 * H else 130_000.0 if T - 24 * H <= t < T - 20 * H else 100_000.0
            return 98_450.0 if T + 20 * MIN <= t < T + 25 * MIN else 100_000.0

        v = {x["name"]: x for x in self.go(self.whales(), far, variants=True)["summary"]["variants"]}
        self.assertEqual([v[n]["open"] for n in ("текущие правила", "стоп не дальше 1 %", "стоп не дальше 2,5 %",
                                                 "стоп только за уровнем")], [0, 0, 1, 1])
        self.assertLess(v["стоп не дальше 1 %"]["net"], 0)
        os.remove(self.db)
        self.cache += "2"

        def top(c: str, t: int) -> float:   # 4 дня 97 000–101 000, цена 100 000 — у сопротивления
            if t < T:
                return 97_000.0 if T - 16 * H <= t < T - 12 * H else 101_000.0 if T - 24 * H <= t < T - 20 * H else 100_000.0
            return 102_000.0 if t >= T + 60 * MIN else 100_000.0

        v = {x["name"]: x for x in self.go(self.whales(), top, variants=True)["summary"]["variants"]}
        self.assertEqual((v["текущие правила"]["trades"], v["без уровней 4ч"]["trades"], v["без MA30"]["trades"]), (0, 1, 0))
        os.remove(self.db)
        self.cache += "3"

        def ma_up(c: str, t: int) -> float:   # 7 часов до сигнала — 101 000: MA30 выше цены
            if T - 7 * H <= t < T:
                return 101_000.0
            return 102_000.0 if t >= T + 60 * MIN else 100_000.0

        v = {x["name"]: x for x in self.go(self.whales(), ma_up, variants=True)["summary"]["variants"]}
        self.assertEqual((v["текущие правила"]["trades"], v["без MA30"]["trades"], v["без уровней 4ч"]["trades"]), (0, 1, 0))

    def test_far_level_stop_capped(self) -> None:
        """Поддержка далеко (90 000 — стоп за ней в 10,5 %): стоп — 1,5 % от
        цены входа с проскальзыванием не считая (100 000 → 98 500)."""
        H = 60 * MIN

        def f(c: str, t: int) -> float:
            if t < T:
                return 90_000.0 if T - 16 * H <= t < T - 12 * H else 130_000.0 if T - 24 * H <= t < T - 20 * H else 100_000.0
            return 98_450.0 if T + 20 * MIN <= t < T + 25 * MIN else 100_000.0

        [t] = self.go(self.whales(), f)["trades"]
        self.assertEqual(t.why, "стоп", "за уровнем стоп был бы 89 550 — сидели бы")
        self.assertAlmostEqual(t.exit_px, 98_450 * 0.9995, delta=1, msg="свеча открылась за стопом 98 500")

    def test_levels_24h(self) -> None:
        """Уровни: минимум и максимум цены за 24 часа до решения. Был минимум
        99 000 за 10 часов до сигнала, цена 100 000 — у сопротивления: лонг не
        входит, шорт входит. Идущая свеча (до 103 000 к своему закрытию) — не в
        счёт: это будущее."""
        def price(c: str, t: int) -> float:
            if T - 12 * 60 * MIN <= t < T - 8 * 60 * MIN:
                return 99_000.0
            if T + 1000 <= t < T + 60 * MIN:   # идущий час (и идущая 5-минутка) — к закрытию 103 000
                return 103_000.0
            return 100_000.0
        r = self.go(self.whales(), price)
        self.assertEqual(r["trades"], [])
        self.assertEqual(r["skips"].get("level"), 1)
        os.remove(self.db)
        self.cache += "2"
        r = self.go(self.whales(side=-1), lambda c, t: price(c, t) if t < T else 100_000.0 * (0.988 if t > T + 60 * MIN else 1))
        self.assertEqual([(t.side, t.why) for t in r["trades"]], [(-1, "тейк")])

    def test_levels_current_hour_and_hourly_candles(self) -> None:
        """Как у живого бота (24 часовые свечи с идущей): идущий час — по его
        закрытым 5-минуткам. Сигнал в T+30 мин; в T+10…15 мин цена была 99 000 —
        у сопротивления: лонг не входит. Часовые свечи — запросом bar=1H, кеш —
        свой файл."""
        def price(c: str, t: int) -> float:
            return 99_000.0 if T + 10 * MIN <= t < T + 15 * MIN else 100_000.0
        r = self.go(self.whales(t=T + 30 * MIN), price)
        self.assertEqual(r["trades"], [])
        self.assertEqual(r["skips"].get("level"), 1)
        self.assertTrue(any(c["params"].get("bar") == "4H" for c in self.market.calls if "params" in c))
        self.assertTrue(os.path.isfile(os.path.join(self.cache, "replay_BTC-USDT-SWAP_4H.json")))

    def test_ma30_15m(self) -> None:
        """MA30 15-минутного графика (29 закрытых свечей + идущая по цене); уровни
        за (24 ч: 97 000–108 000+, цена 100 000 у поддержки).
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
        ним одним, хоть цена и у сопротивления (уровни ему не помеха). До пятой
        сделки одиночный 0xa — не сигнал."""
        H = 60 * MIN
        entries = [T + i * 4 * H for i in range(5)]

        def price(c: str, t: int) -> float:
            if T - 28 * H <= t < T - 24 * H:
                return 96_000.0                       # поддержка 4 дня
            if T - 24 * H <= t < T - 20 * H:
                return 110_000.0                      # сопротивление
            for e in entries:
                if e <= t < e + 10 * MIN:              # лимит входа исполняется следующей 5-минуткой
                    return 100_000.0                  # вход — у поддержки
                if e + 10 * MIN <= t < e + 30 * MIN:
                    return 101_200.0                  # тейк +1 %
            if T + 20 * H <= t < T + 20 * H + 10 * MIN:
                return 108_000.0                      # приоритетный: у сопротивления — уровни ему не помеха
            if T + 20 * H + 10 * MIN <= t < T + 20 * H + 30 * MIN:
                return 109_300.0
            return 99_000.0                           # MA30 ниже цены входа

        rows: list[tuple] = []
        for i, e in enumerate(entries):
            rows += [opn(10 * i + 1, "0xa", "BTC", 1, e - MIN, 5000, 100_000), opn(10 * i + 2, "0xb", "BTC", 1, e, 5000, 100_000)]
        early = opn(90, "0xa", "BTC", 1, T + 15 * H + 30 * MIN, 5000, 100_000)   # одиночный после паузы, до пятой — не сигнал
        rows += [early, opn(99, "0xa", "BTC", 1, T + 20 * H, 5000, 108_000)]
        rows.sort(key=lambda r: r[4])
        r = self.go(rows, price)
        self.assertEqual([(t.why, (t.entry_t - T) // H) for t in r["trades"]],
                         [("тейк", 0), ("тейк", 4), ("тейк", 8), ("тейк", 12), ("тейк", 16), ("тейк", 20)])
        self.assertEqual(r["trades"][-1].wallets, ("0xa",))

    def test_one_big_against_blocks_small_side(self) -> None:
        big = [opn(100, "0xbig1", "BTC", -1, T - 40 * MIN, 700_000, 100_000)]   # один крупный — сигнала у него нет
        r = self.go(big + self.whales(), step_price(100_000, [(T + 60 * MIN, 0.012)]))
        self.assertEqual(r["trades"], [])
        self.assertGreater(r["skips"]["bigger_against"], 0)

    def test_bigger_against_goes_with_big(self) -> None:
        big = [opn(100, "0xbig1", "BTC", -1, T - 40 * MIN, 700_000, 100_000),
               opn(101, "0xbig2", "BTC", -1, T - 39 * MIN, 700_000, 100_000)]
        r = self.go(big + self.whales(), step_price(100_000, [(T + 60 * MIN, -0.012)]))
        [t] = r["trades"]
        self.assertEqual((t.side, t.why), (-1, "тейк"))

    PX: ClassVar[dict[str, float]] = {"BTC": 100_000.0, "ETH": 3000.0, "SOL": 150.0, "DOGE": 0.2, "NOPE": 1.0}

    def test_three_positions_and_no_inst(self) -> None:
        """Своих позиций — до 3 (как у живого бота): BTC, ETH, SOL открыты,
        DOGE — «own_open»; бумаги нет на OKX — «no_inst». Вариант «1 позиция» —
        только BTC."""
        rows = [*self.whales(), *self.whales(coin="ETH", px=3000, t=T + 30 * MIN, base=10),
                *self.whales(coin="SOL", px=150, t=T + 31 * MIN, base=20),  # NOPE — раньше: пока место есть
                *self.whales(coin="DOGE", px=0.2, t=T + 32 * MIN, base=30),
                *self.whales(coin="NOPE", px=1, t=T + 15 * MIN, base=40)]
        r = self.go(rows, lambda c, t: self.PX[c], variants=True)
        self.assertEqual([t.coin for t in r["trades"]], ["BTC", "ETH", "SOL"], "позиций до трёх")
        self.assertGreater(r["skips"]["own_open"], 0)
        self.assertGreater(r["skips"]["no_inst"], 0)
        v = {x["name"]: x for x in r["summary"]["variants"]}
        self.assertEqual((v["1 позиция"]["trades"], v["текущие правила"]["trades"]), (1, 3))

    def test_same_coin_while_open_and_equity_from_closed_only(self) -> None:
        """BTC открыт дольше паузы — новый сигнал BTC: «own_same». Размер
        ETH (вход, пока BTC открыт) — от капитала без итога BTC; SOL (после
        тейка BTC) — с ним."""
        rows = [*self.whales(), *self.whales(coin="ETH", px=3000, t=T + 30 * MIN, base=10),
                *self.whales(coin="SOL", px=150, t=T + 90 * MIN, base=20)]

        def price(c: str, t: int) -> float:
            return 101_200.0 if c == "BTC" and t >= T + 60 * MIN else self.PX[c]

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

    def test_limit_entry_and_market_variant(self) -> None:
        """Вход — лимитом (как у живого бота): по цене решения, мейкером, без
        проскальзывания — если за 5 минут после решения цена дошла до лимита;
        ушла вверх сразу — входа нет («limit_miss»). Вариант «вход рынком» —
        вход всегда, но тейкер и проскальзывание."""
        r = self.go(self.whales(), step_price(100_000, [(T + 60 * MIN, 0.012)]), variants=True)
        v = {x["name"]: x for x in r["summary"]["variants"]}
        self.assertEqual(v["вход рынком"]["trades"], 1)
        self.assertLess(v["вход рынком"]["net"], v["текущие правила"]["net"], "тейкер и проскальзывание — дороже")
        self.assertAlmostEqual(v["вход рынком"]["net"], 100 - 10_000 * (0.0005 + 0.0002), delta=0.1)
        os.remove(self.db)
        self.cache += "2"
        r = self.go(self.whales(), step_price(100_000, [(T + 5 * MIN, 0.012)]), variants=True)
        v = {x["name"]: x for x in r["summary"]["variants"]}
        self.assertEqual((v["текущие правила"]["trades"], v["вход рынком"]["trades"]), (0, 1))
        self.assertEqual(r["skips"].get("limit_miss"), 1)
        self.assertIn("в месяц ≈", r["text"])
        self.assertEqual(v["вход рынком"]["per_month"], 1.0, "1 сделка за 30 дней повтора")

    def test_coin_pause_three_hours(self) -> None:
        rows = [*self.whales(), *self.whales(t=T + 120 * MIN, base=10), *self.whales(t=T + 200 * MIN, base=20)]
        r = self.go(rows, step_price(100_000, [(T + 60 * MIN, 0.012), (T + 61 * MIN, 0.0),
                                               (T + 130 * MIN, 0.012), (T + 131 * MIN, 0.0)]))
        self.assertEqual(len(r["trades"]), 2, "вход в +0 мин, в +120 — пауза 3 ч, в +200 — снова")
        self.assertGreater(r["skips"]["coin_pause"], 0)
        os.remove(self.db)
        self.cache += "2"
        r = self.go(rows, step_price(100_000, [(T + 60 * MIN, 0.012), (T + 61 * MIN, 0.0),
                                               (T + 130 * MIN, 0.012), (T + 131 * MIN, 0.0)]), variants=True)
        v = {x["name"]: x for x in r["summary"]["variants"]}
        self.assertEqual((v["текущие правила"]["trades"], v["пауза бумаги 30 мин"]["trades"]), (2, 3))

    def test_no_okx_coin_skipped(self) -> None:
        r = self.go(self.whales(coin="NOPE", px=1), lambda c, t: 1.0)
        self.assertEqual(r["trades"], [])
        self.assertGreater(r["skips"]["no_inst"], 0)

    def test_second_run_from_cache_without_network(self) -> None:
        price = step_price(100_000, [(T + 60 * MIN, 0.012)])
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
        rows = [*self.whales(), cls(10, "0xa", "BTC", 1, T - MIN, -50.0)]   # кит сигнала уже вышел в убыток
        rows[1] = opn(2, "0xb", "BTC", 1, T, 5000, 100_000)
        r = self.go(rows, lambda c, t: 100_000.0)
        self.assertEqual(r["trades"], [])
        self.assertGreater(r["skips"]["whale_exit"], 0)
        os.remove(self.db)
        self.cache += "3"
        # Кит A — за 2 часа минус 2 секунды до кита B: на момент решения (+5 с) он старше 2 часов.
        rows = [opn(1, "0xa", "BTC", 1, T - 120 * MIN + 2000, 5000, 100_000), opn(2, "0xb", "BTC", 1, T, 5000, 100_000)]
        r = self.go(rows, lambda c, t: 100_000.0)
        self.assertEqual(r["trades"], [])
        self.assertGreater(r["skips"]["stale"], 0)

    def test_network_failure_is_not_cached_forever(self) -> None:
        """Ревизия: сбой сети на блоке свечей раньше ложился в кеш пустым
        навсегда — следующие прогоны «не видели» цен этого блока."""
        make_db(self.db, self.whales())
        good = FakeMarket(step_price(100_000, [(T + 60 * MIN, 0.012)]))

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
        price = step_price(100_000, [(T + 60 * MIN, 0.012)])
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
