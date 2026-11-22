#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Карта ликвидаций на момент в прошлом (tools/autotrade_liq.py) — на
поддельных Binance и Bybit, без сети: разбор ответов бирж, множитель
1000PEPE, интерес Bybit с одной стороны, только закрытые свечи (без взгляда
в будущее), чужой актив по цене, кеш на диске; и правило входа
(autotrade_core.liq_ok). С настоящей моделью (wt_liqmap.py рядом, в
../Wallet-Tracker) — сборка карты целиком.
"""
import json
import os
import sys
import tempfile
import unittest
import urllib.parse
from typing import Any

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import autotrade_core as core  # noqa: E402
import autotrade_liq as lq  # noqa: E402

Q = lq.ROW_MS
END = 1_800_000_000_000 // Q * Q
START = END - 10 * 96 * Q        # десять суток


class FakeEx:
    """Binance и Bybit: 15-минутные свечи (цена px, ±0,5 %) и интерес по
    времени; ответы — как у настоящих бирж, страницы — по startTime/endTime."""

    def __init__(self, px: float = 100.0, symbols: tuple[str, ...] = ("BTCUSDT",), bybit_single: bool = True,
                 start: int = START, end: int = END) -> None:
        self.px, self.symbols, self.single, self.start, self.end = px, symbols, bybit_single, start, end
        self.urls: list[str] = []

    def oi(self, t: int) -> float:
        return 1e6 + (t - self.start) // Q * 1000.0

    def __call__(self, url: str) -> Any:
        self.urls.append(url)
        u = urllib.parse.urlparse(url)
        q = dict(urllib.parse.parse_qsl(u.query))
        sym = q.get("symbol", "")
        if sym not in self.symbols:
            return {"code": -1121, "msg": "Invalid symbol."} if "binance" in u.netloc else {"retCode": 10001, "result": {}}
        a = int(q.get("startTime") or q.get("start") or 0)
        b = int(q.get("endTime") or q.get("end") or 0)
        ts = [t for t in range(a // Q * Q, b + 1, Q) if a <= t <= b and self.start <= t < self.end]
        px = self.px * (1000 if sym.startswith("1000") else 1)
        if u.path.endswith("openInterestHist"):
            return [{"symbol": sym, "sumOpenInterest": "1", "sumOpenInterestValue": str(self.oi(t)), "timestamp": t}
                    for t in ts]
        if u.path.endswith("takerlongshortRatio"):
            return [{"buySellRatio": "3", "buyVol": "1", "sellVol": "1", "timestamp": t} for t in ts]
        if u.path.endswith("klines"):
            return [[t, str(px), str(round(px * 1.005, 9)), str(round(px * 0.995, 9)), str(px), "1"] for t in ts]
        if u.path.endswith("open-interest"):
            rows = [{"openInterest": str(self.oi(t) / px * 2), "timestamp": str(t),
                     **({"singleOpenInterest": str(self.oi(t) / px)} if self.single else {})} for t in reversed(ts)]
            return {"retCode": 0, "result": {"list": rows, "nextPageCursor": ""}}
        if u.path.endswith("kline"):
            return {"retCode": 0, "result": {"list": [[str(t), str(px), str(round(px * 1.005, 9)), str(round(px * 0.995, 9)), str(px), "1", "1"]
                                                      for t in reversed(ts)]}}
        return None


class FakeModel:
    """Вместо wt_liqmap: запоминает, что пришло в сборку, и отдаёт cum."""

    def __init__(self, longs: float = 10.0, shorts: float = 5.0) -> None:
        self.longs, self.shorts = longs, shorts
        self.calls: list[tuple[str, str, dict[str, list[lq.Row]], list[lq.Row], int, float]] = []

    def build_map(self, sym: str, rng: str, got: dict[str, list[lq.Row]], ref: list[lq.Row], at: int,
                  px: float) -> dict[str, Any]:
        self.calls.append((sym, rng, got, ref, at, px))
        return {"ok": True, "cum": {2: {"L": self.longs, "S": self.shorts}}}


class ParseTest(unittest.TestCase):
    def test_binance_rows(self) -> None:
        ex = FakeEx()
        rows = lq.binance(ex, "BTC", START, START + 700 * Q)
        self.assertEqual(len(rows), 701)
        self.assertEqual(rows[0], (START // 1000, 100.5, 99.5, 100.0, 1e6, 0.75), "доля покупок — 3 / (1 + 3)")
        self.assertEqual(rows[-1][0], (START + 700 * Q) // 1000)
        oi = [u for u in ex.urls if "openInterestHist" in u]
        self.assertEqual(len(oi), 2, "по 500 снимков за запрос")
        self.assertIn("period=15m", oi[0])
        self.assertEqual(len([u for u in ex.urls if "klines" in u]), 1, "свечи — по 1500")

    def test_bybit_rows_one_side_and_half(self) -> None:
        for single in (True, False):
            ex = FakeEx(bybit_single=single)
            rows = lq.bybit(ex, "BTC", START, START + 300 * Q)
            self.assertEqual(len(rows), 301)
            self.assertAlmostEqual(rows[5][4], 1e6 + 5000, places=3)   # монеты × цена, одна сторона
            self.assertEqual(rows[5][5], 0.5)
            self.assertEqual(len([u for u in ex.urls if "open-interest" in u]), 2, "по 200 за запрос")

    def test_multiplier_name(self) -> None:
        ex = FakeEx(px=0.01, symbols=("1000PEPEUSDT",))
        for fn in lq.SOURCES.values():
            rows = fn(ex, "PEPE", START, START + 20 * Q)
            self.assertEqual(len(rows), 21)
            self.assertAlmostEqual(rows[0][3], 0.01, "цена — за одну монету")
        self.assertEqual(lq.SOURCES["Binance"](FakeEx(symbols=()), "XYZ", START, START + 20 * Q), [])

    def test_garbage_is_dropped(self) -> None:
        rows = lq.clean([(1, 2, 1, 1.5, 10, 0.4), (2, "x", 1, 1, 1, 0.5), (3, 1, 2, 1.5, 1, 0.5), (4, 2, 1, 0, 1, 0.5),
                         (5, 2, 1, 1.5, -1, 0.5), (6, 2, 1, 3, 5, 7), [7, 2, 1], "row", (0, 2, 1, 1.5, 1, 0.5),
                         (1, 2, 1, 1.4, 11, 0.4), (8, float("nan"), 1, 1, 1, 1)])
        self.assertEqual(rows, [(1, 2.0, 1.0, 1.4, 11.0, 0.4), (6, 3.0, 1.0, 3.0, 5.0, 0.5)])
        self.assertEqual(lq.clean(None), [])
        self.assertEqual(lq._t("garbage"), 0)


class HistTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def hist(self, model: Any, ex: FakeEx, offline: bool = False) -> lq.LiqHist:
        return lq.LiqHist(model, self.tmp.name, ex, START, END, offline, 0)

    def test_only_closed_candles_and_window(self) -> None:
        m, ex = FakeModel(), FakeEx()
        h = self.hist(m, ex)
        t = START + 5 * 96 * Q + 7 * 60_000          # 7 минут внутри свечи
        self.assertEqual(h.cum("BTC", t, 100.0), {2: {"L": 10.0, "S": 5.0}})
        sym, rng, got, ref, at, px = m.calls[0]
        self.assertEqual((sym, rng, at, px, sorted(got)), ("BTC", "1d", t // 1000, 100.0, ["Binance", "Bybit"]))
        for rows in got.values():
            self.assertLessEqual(rows[-1][0] * 1000 + Q, t, "незакрытая свеча — это будущее")
            self.assertEqual(rows[-1][0] * 1000, t // Q * Q - Q)
            self.assertEqual(len(rows), 4 * 96, "сутки окна и три дня учёта до него")
        self.assertEqual(len(ref), lq.WINDOW)
        self.assertIs(ref[-1], got["Binance"][-1], "опора — биржа с наибольшим интересом, окно — сутки")
        self.assertTrue(all(r[5] == 0.75 for r in got["Bybit"]), "доля покупок Bybit — у Binance")
        h.cum("BTC", t + 60_000, 100.0)
        self.assertEqual(len(m.calls), 1, "в той же 15-минутке — та же карта")

    def test_other_asset_and_short_history(self) -> None:
        m = FakeModel()
        h = self.hist(m, FakeEx(px=100.0))
        self.assertIsNone(h.cum("BTC", START + 3 * 96 * Q, 104.0), "цена бирж дальше 3 % — чужой актив")
        self.assertIsNone(h.cum("BTC", START + 5 * Q, 100.0), "снимков меньше 8 — без карты")
        self.assertIsNone(h.cum("BTC", START + 40 * Q, 100.0), "окно короче половины суток — без карты")
        self.assertEqual(m.calls, [])
        self.assertIsNotNone(h.cum("BTC", START + 3 * 96 * Q + Q, 102.9), "в пределах 3 % — та же монета")

    def test_gap_in_history_is_no_map(self) -> None:
        m = FakeModel()
        h = self.hist(m, FakeEx())
        self.assertIsNone(h.cum("BTC", END + 2 * 3_600_000, 100.0), "последний снимок старше часа — карты нет")

    def test_cache_then_offline(self) -> None:
        ex = FakeEx()
        h = self.hist(FakeModel(), ex)
        h.cum("BTC", START + 4 * 96 * Q, 100.0)
        n = len(ex.urls)
        self.assertGreater(n, 0)
        with open(os.path.join(self.tmp.name, "liq_BTC_15m.json"), encoding="utf-8") as f:
            self.assertEqual(sorted(json.load(f)), ["Binance", "Bybit"])
        ex2 = FakeEx()
        h2 = self.hist(FakeModel(), ex2)
        self.assertIsNotNone(h2.cum("BTC", START + 4 * 96 * Q, 100.0))
        self.assertEqual(ex2.urls, [], "кеш покрывает отрезок — без сети")
        h3 = self.hist(FakeModel(), FakeEx(), offline=True)
        h3.rows.clear()
        self.assertIsNotNone(h3.cum("BTC", START + 4 * 96 * Q, 100.0))

    def test_network_failure_is_no_map(self) -> None:
        def boom(_url: str) -> object:
            raise OSError("нет сети")
        h = self.hist(FakeModel(), boom)  # type: ignore[arg-type]
        self.assertIsNone(h.cum("BTC", START + 4 * 96 * Q, 100.0))
        self.assertGreater(h.requests, 0)

    def test_missing_model_dir(self) -> None:
        self.assertIsNone(lq.load_model(os.path.join(self.tmp.name, "nope")))
        bad = os.path.join(self.tmp.name, "bad")
        os.makedirs(bad)
        with open(os.path.join(bad, "wt_liqmap.py"), "w", encoding="utf-8") as f:
            f.write("raise RuntimeError('сломан')\n")
        sys.modules.pop("wt_liqmap", None)
        self.assertIsNone(lq.load_model(bad))
        sys.path.remove(os.path.abspath(bad))
        sys.modules.pop("wt_liqmap", None)


class RuleTest(unittest.TestCase):
    def test_rule(self) -> None:
        self.assertTrue(core.liq_ok({"2": {"L": 10, "S": 5}}, -1))
        self.assertFalse(core.liq_ok({"2": {"L": 10, "S": 5}}, 1))
        self.assertTrue(core.liq_ok({2: {"L": 5, "S": 10}}, 1))
        self.assertFalse(core.liq_ok({2: {"L": 5, "S": 5}}, 1), "поровну — не за нас")
        self.assertFalse(core.liq_ok({2: {"L": 5, "S": 5}}, -1))
        for bad in (None, [], {}, {"5": {"L": 1, "S": 2}}, {"2": {"L": 0, "S": 0}}, {"2": {"L": 1}}, {"2": "x"},
                    {"2": {"L": True, "S": 2}}, {"2": {"L": -1, "S": 2}}, {"2": {"L": float("inf"), "S": 2}}):
            self.assertIsNone(core.liq_ok(bad, 1), bad)
        self.assertEqual(core.liq_near({"2": {"L": 3, "S": 0}}), (3.0, 0.0))


WT = os.path.join(os.path.dirname(ROOT), "Wallet-Tracker")


@unittest.skipUnless(os.path.isfile(os.path.join(WT, "wt_liqmap.py")), "нет ../Wallet-Tracker — проверка с настоящей моделью")
class RealModelTest(unittest.TestCase):
    def test_real_map_from_history(self) -> None:
        model = lq.load_model(WT)
        self.assertIsNotNone(model)
        with tempfile.TemporaryDirectory() as tmp:
            h = lq.LiqHist(model, tmp, FakeEx(), START, END, False, 0)
            cum = h.cum("BTC", START + 5 * 96 * Q, 100.0)
        near = core.liq_near(cum)
        if near is None:
            self.fail(f"карта не собралась: {cum}")
        self.assertGreater(near[0] + near[1], 0)


if __name__ == "__main__":
    r = unittest.main(exit=False, verbosity=1).result
    if r.wasSuccessful():
        print(f"autotrade_liq: {r.testsRun} проверок карты ликвидаций на момент в прошлом — без сети")
    sys.exit(0 if r.wasSuccessful() else 1)
