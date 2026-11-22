#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Бэктест-повтор (tools/autotrade_replay.py) — на синтетической базе китов
и поддельных свечах OKX: без сети. Проверяется, что повтор торгует по тем же
правилам, что живой бот: вход от 2 китов, тейк +1 %, выход вслед за китом в
убытке, разовое усреднение, «входим с крупными», одна позиция, пауза на
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
          "tickSz": "0.01", "state": "live"}]


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
        rows = []
        for t in range(end - 100 * rp.BAR_MS, end, rp.BAR_MS):
            if t >= NOW:
                continue
            o, c = self.price(coin, t), self.price(coin, t + rp.BAR_MS - 1)
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
        # 5 % × 20 000 × 10 = 10 000 $ ≈ 0,1 BTC; +1 % ≈ +100 $ минус комиссии.
        self.assertAlmostEqual(t.pnl, 100 - 10_000 * (0.0005 + 0.0002) - 5, delta=6)
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

    def test_average_once_then_tp_from_new_average(self) -> None:
        # −1,5 % через 20 мин, кит добирает 2 000 $ в +40 мин, потом цена +0,3 % от входа.
        rows = [*self.whales(), opn(20, "0xb", "BTC", 1, T + 40 * MIN, 2000, 98_500)]
        r = self.go(rows, step_price(100_000, [(T + 20 * MIN, -0.015), (T + 120 * MIN, 0.003)]))
        [t] = r["trades"]
        self.assertEqual(r["summary"]["averaged"], 1)
        self.assertEqual(t.why, "тейк", "после усреднения тейк ближе — +1 % от новой средней")
        self.assertLess(t.tp, 100_000 * 1.01)

    def test_no_average_without_whale_add(self) -> None:
        r = self.go(self.whales(), step_price(100_000, [(T + 20 * MIN, -0.015), (T + 120 * MIN, 0.003)]))
        self.assertEqual(r["summary"]["averaged"], 0)
        self.assertEqual(r["trades"][0].why, "открыта", "без усреднения тейк +1 % от 100 000 не достигнут")

    def test_no_average_small_dip_or_small_add(self) -> None:
        for dip, add in ((-0.005, 2000), (-0.015, 500)):   # минус 0,5 % — мало; добор 500 $ — мало
            self.cache = os.path.join(self.tmp.name, f"cache{dip}")  # свои свечи у каждого случая
            rows = [*self.whales(), opn(20, "0xb", "BTC", 1, T + 40 * MIN, add, 98_500)]
            r = self.go(rows, step_price(100_000, [(T + 20 * MIN, dip)]))
            self.assertEqual(r["summary"]["averaged"], 0, (dip, add))
            os.remove(self.db)

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

    def test_one_position_and_no_inst(self) -> None:
        rows = [*self.whales(), *self.whales(coin="ETH", px=3000, t=T + 30 * MIN, base=10),
                *self.whales(coin="NOPE", px=1, t=T + 31 * MIN, base=20)]
        r = self.go(rows, lambda c, t: 3000.0 if c == "ETH" else 100_000.0)
        self.assertEqual([t.coin for t in r["trades"]], ["BTC"], "позиция одна")
        self.assertGreater(r["skips"]["own_open"], 0)

    def test_coin_pause_three_hours(self) -> None:
        rows = [*self.whales(), *self.whales(t=T + 120 * MIN, base=10), *self.whales(t=T + 200 * MIN, base=20)]
        r = self.go(rows, step_price(100_000, [(T + 60 * MIN, 0.012), (T + 61 * MIN, 0.0),
                                               (T + 130 * MIN, 0.012), (T + 131 * MIN, 0.0)]))
        self.assertEqual(len(r["trades"]), 2, "вход в +0 мин, в +120 — пауза 3 ч, в +200 — снова")
        self.assertGreater(r["skips"]["coin_pause"], 0)

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

    def test_variants_with_whales_exit_on_profit_close(self) -> None:
        """«Вместе с китами»: кит сигнала закрыл в плюс от 1000 $ — выходим с
        ним; правила бота держат до тейка +1 %. Все варианты — на тех же сигналах."""
        rows = [*self.whales(), cls(10, "0xa", "BTC", 1, T + 30 * MIN, +100.0)]
        r = self.go(rows, step_price(100_000, [(T + 20 * MIN, 0.005), (T + 90 * MIN, 0.012)]))
        b = r["books"]
        self.assertEqual(list(b), ["live", "with_whales", "tp_or_whales", "no_average"])
        self.assertEqual(b["live"].trades[0].why, "тейк")
        for key in ("with_whales", "tp_or_whales"):
            [t] = b[key].trades
            self.assertEqual(t.why, "вместе с китом", key)
            self.assertLess(t.exit_t, T + 60 * MIN, key)
            self.assertGreater(t.pnl, 0, key)
            self.assertLess(t.pnl, b["live"].trades[0].pnl, key)
        v = {x["key"]: x for x in r["summary"]["variants"]}
        self.assertEqual(v["live"]["full"], r["summary"]["full"], "первый вариант — сами правила бота")
        self.assertEqual(v["with_whales"]["full"]["trades"], 1)
        self.assertLess(v["with_whales"]["deposit_end"], v["live"]["deposit_end"])
        self.assertIn("Варианты выхода", r["text"])

    def test_with_whales_small_or_early_close_ignored(self) -> None:
        # Кит закрыл 500 $ после входа — мало; кит закрыл 5000 $ в плюс ДО нашего входа — не в счёт.
        small = (10, "0xa", "BTC", 3, T + 30 * MIN, int(500e9), "0.005", int(10e9), "")
        early = cls(11, "0xa", "BTC", 1, T - MIN, +100.0)   # 0xa открылся в T − 2 мин — закрытие в его окне
        r = self.go([*self.whales(), small, early], step_price(100_000, [(T + 90 * MIN, 0.012)]))
        self.assertEqual(r["books"]["with_whales"].trades[0].why, "открыта", "тейка у варианта нет — держим")
        self.assertEqual(r["books"]["tp_or_whales"].trades[0].why, "тейк")

    def test_variants_loss_exit_and_no_average(self) -> None:
        rows = [*self.whales(), cls(10, "0xa", "BTC", 1, T + 30 * MIN, -100.0)]
        r = self.go(rows, step_price(100_000, [(T + 20 * MIN, -0.003)]))
        self.assertEqual(r["books"]["with_whales"].trades[0].why, "вместе с китом", "в убытке — тоже выходим")
        self.assertEqual(r["books"]["no_average"].trades[0].why, "кит вышел в убыток")
        os.remove(self.db)
        self.cache += "2"
        rows = [*self.whales(), opn(20, "0xb", "BTC", 1, T + 40 * MIN, 2000, 98_500)]
        r = self.go(rows, step_price(100_000, [(T + 20 * MIN, -0.015), (T + 120 * MIN, 0.003)]))
        self.assertTrue(r["books"]["live"].trades[0].averaged)
        [t] = r["books"]["no_average"].trades
        self.assertEqual((t.averaged, t.why), (False, "открыта"))
        self.assertEqual({x["key"]: x["averaged"] for x in r["summary"]["variants"]}["no_average"], 0)

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
