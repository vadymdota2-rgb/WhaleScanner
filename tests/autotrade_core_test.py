#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Общее торговли и бэктеста-повтора (tools/autotrade_core.py): база только
на чтение, окно китов (по ходу = пересчёт с нуля), имя бумаги на OKX,
предел памяти — код 3 у бэктеста; и бэктест на базе размером с настоящую —
быстро и в пределах памяти.
"""
import io
import json
import os
import random
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import autotrade_core as core  # noqa: E402
import autotrade_replay as rp  # noqa: E402


class CoreTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_db_read_only_and_missing_is_oserror(self) -> None:
        db = os.path.join(self.tmp.name, "hl.db")
        con = sqlite3.connect(db)
        con.execute("CREATE TABLE hl_fills (tid INTEGER)")
        con.commit()
        con.close()
        ro = core.open_ro(db)
        with self.assertRaises(sqlite3.OperationalError):
            ro.execute("DELETE FROM hl_fills")
        with self.assertRaises(sqlite3.OperationalError):  # и временные таблицы — запрет «только запросы»
            ro.execute("CREATE TEMP TABLE x (a)")
        ro.close()
        with self.assertRaises(OSError):  # не SystemExit: у службы это ошибка шага, а не выход
            core.open_ro(os.path.join(self.tmp.name, "нет.db"))

    def test_okx_names(self) -> None:
        self.assertEqual(core.okx_candidate("kPEPE"), ("PEPE", 1000.0))
        self.assertEqual(core.okx_candidate("xyz:GOLD"), ("XAU", 1.0))
        self.assertEqual(core.okx_candidate("xyz:TSLA"), ("TSLA", 1.0))
        self.assertEqual(core.okx_candidate("BTC"), ("BTC", 1.0))
        self.assertEqual(core.okx_candidate("xyz:kAPPA"), ("kAPPA", 1.0))  # у акций «k» — часть имени
        self.assertTrue(core.is_perp("xyz:TSLA") and core.is_perp("BTC"))
        self.assertFalse(core.is_perp("@107") or core.is_perp("PURR/USDC") or core.is_perp(""))

    def test_window_matches_brute_force(self) -> None:
        """Окно по ходу (прибавили — вычли) даёт ровно то же, что пересчёт с
        нуля: китов от порога, средняя цена китов и объём без плеча по
        кошелькам; опоздавшие сделки — тоже."""
        rnd = random.Random(7)
        for limit in (0, 1000 * core.NANOS):
            w = core.Window(limit)
            orders: list[tuple[int, str, int, float, int]] = []
            t = 10**9
            for _ in range(3000):
                t += rnd.randrange(0, 600_000)
                late = rnd.random() < 0.1
                o = (t - rnd.randrange(0, 3_000_000) if late else t, f"w{rnd.randrange(30)}",
                     rnd.choice([0, 300, 900, 1500, 25_000]) * core.NANOS, rnd.choice([0.0, 1.5, 10.0]),
                     rnd.choice([0, 30, 500, 9000]) * core.NANOS)
                w.add(*o)
                w.expire(t - 7_200_000)
                orders = [x for x in [*orders, o] if x[0] >= t - 7_200_000]
                per: dict[str, list] = {}
                for _t, wl, n, u, m in orders:
                    v = per.setdefault(wl, [0, 0.0, 0])
                    v[0] += n
                    v[1] += u
                    v[2] += m
                self.assertEqual({k: v[4] for k, v in w.per.items()}, {k: v[2] for k, v in per.items()})
                self.assertEqual(w.count, sum(1 for v in per.values() if v[0] >= limit))
                big = [v for v in per.values() if v[0] >= limit and v[1] > 0]
                self.assertEqual(w.big_n, sum(v[0] for v in big))
                self.assertAlmostEqual(w.big_u, sum(v[1] for v in big), places=6)

    def test_memory_cap_any_flavour_is_code_3(self) -> None:
        """Предел памяти: MemoryError, «out of memory» от SQLite и SystemError
        (так падает sqlite3 в некоторых сборках) — код 3 и понятная строка;
        без предела SystemError — настоящая ошибка, не 3."""
        out = self.tmp.name
        for exc in (MemoryError(), sqlite3.OperationalError("out of memory"), SystemError("x")):
            err = io.StringIO()
            with mock.patch.object(rp, "run", side_effect=exc), mock.patch.object(core, "limit_memory"), \
                    mock.patch("sys.stderr", err):
                self.assertEqual(rp.main(["--out", out, "--max-mem-mb", "150"]), 3, repr(exc))
            self.assertIn("предела памяти", err.getvalue())
        with mock.patch.object(rp, "run", side_effect=SystemError("x")), self.assertRaises(SystemError):
            rp.main(["--out", out, "--max-mem-mb", "0"])
        with mock.patch.object(rp, "run", side_effect=sqlite3.OperationalError("no such table")), \
                mock.patch.object(core, "limit_memory"), self.assertRaises(sqlite3.OperationalError):
            rp.main(["--out", out, "--max-mem-mb", "150"])


class ScaleTest(unittest.TestCase):
    """База размером с настоящую: 300 000 сделок китов, 60 бумаг, 30 дней —
    повтор отдельным процессом без сети (кеш свечей готов) укладывается в
    две минуты и в 400 МБ (предел памяти — код 3 — CoreTest)."""

    def test_big_db_fast_and_within_memory(self) -> None:
        import autotrade_replay_test as t
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        db, cache, out = (os.path.join(tmp.name, x) for x in ("hl.db", "cache", "out"))
        r = random.Random(11)
        coins = ["BTC", "ETH", *(f"C{i}" for i in range(58))]
        n = int(os.environ.get("AUTOTRADE_SCALE_ROWS", "300000"))
        now = int(time.time() * 1000)  # процесс повтора берёт «сейчас» сам
        start = now - 30 * t.DAY + 60_000
        rows = [t.opn(i + 1, f"0x{r.randrange(5000):04x}", c := r.choice(coins), r.choice((1, -1)),
                      start + (now - 60_000 - start) * i // n, r.choice([300, 900, 1500, 5000, 20_000]),
                      100_000 if c == "BTC" else 3000) for i in range(n)]
        t.make_db(db, rows)
        # Кеш свечей и каталога — заранее (как после прошлого прогона): процесс без сети.
        mk = t.FakeMarket(lambda c, ts: 100_000.0 if c == "BTC" else 3000.0)
        rp.run(rp.parser().parse_args(["--db", db, "--out", out, "--cache", cache, "--pause", "0", "--max-mem-mb", "0",
                                       "--since-days", "1"]), fetch=mk, now_ms=now)
        cmd = [sys.executable, "-I", os.path.join(ROOT, "tools", "autotrade_replay.py"), "--db", db, "--out", out,
               "--cache", cache, "--offline", "--max-mem-mb", "600"]
        t0 = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=600, check=False)
        took = time.time() - t0
        self.assertEqual(p.returncode, 0, p.stderr[-2000:])
        with open(os.path.join(out, "progress.json"), encoding="utf-8") as f:
            prog = json.load(f)
        self.assertEqual((prog["stage"], prog["total"]), ("готово", n), "прошли все сделки китов")
        self.assertLess(took, 120, f"{n} сделок — {took:.0f} с")
        import resource
        peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss / 1024
        self.assertLess(peak, 400, f"пик памяти повтора {peak:.0f} МБ")
        print(f"autotrade_replay: {n} сделок китов — {took:.1f} с, пик памяти {peak:.0f} МБ", flush=True)


if __name__ == "__main__":
    r = unittest.main(exit=False, verbosity=1).result
    if r.wasSuccessful():
        print(f"autotrade_core: {r.testsRun} проверок общего кода и повтора на большой базе")
    sys.exit(0 if r.wasSuccessful() else 1)
