#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Реальная торговля — инварианты на тысячах случайных случаев (зерно
фиксировано: нарушение воспроизводится по номеру случая).

Свойства (рынок честный, параметры случайные; PROPS случаев):
  1. Вход — тогда и только тогда, когда: модуль включён, у бота нет своей
     позиции, ручной позиции владельца на этой же бумаге нет, сигнал свежий, в бумагу не входили 3 часа, цена OKX
     сходится с ценой китов (±2 %) и не хуже её больше чем на 0,3 %,
     размер не меньше минимума бумаги.
  2. Размер — ровно floor(маржа × капитал × 10 / (контракт × цена) / шаг) × шаг,
     маржа всегда 5 %, и при ручной позиции владельца на счёте (контракт в
     долларах — без цены); номинал не больше маржа × 10 капитала.
  3. Вход — рыночный; выход — один лимитный ордер на весь объём; цена тейка
     — на шаге цены, по ту сторону от входа, не ближе +1 % и не дальше
     +1 % + шаг.

Фаззинг (биржа отвечает мусором; FUZZ случаев по нескольку шагов):
  4. Шаг трейдера не бросает ничего, кроме OkxError и OSError.
  5. Ни один ордер не уходит с размером или ценой не конечным положительным
     числом; рыночный вход — только когда трейдер не знает ни одной позиции;
     тейк — по ту сторону от входа, не ближе +1 %.
  6. Секреты не попадают ни в один файл.
"""
import json
import os
import random
import re
import sys
import unittest
from decimal import ROUND_CEILING, ROUND_DOWN, ROUND_FLOOR, Decimal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import autotrade_live as lv  # noqa: E402
from autotrade_live_test import MIN, Base, FakeOkx, fill  # noqa: E402

PROPS = int(os.environ.get("AUTOTRADE_PROPS", "1500"))
FUZZ = int(os.environ.get("AUTOTRADE_FUZZ", "1500"))
NUM_RE = re.compile(r"^[0-9]+(\.[0-9]+)?$")

GARBAGE = ["NaN", "inf", "-1", "0", "", "abc", None, True, [], {}, "1e999", -5, 0, "1e-30", 10**20, "١٢"]


def rnd_garbage(r: random.Random):
    return r.choice(GARBAGE)


class PropsTest(Base):
    def test_entry_size_and_tp_properties(self) -> None:
        r = random.Random(20261121)
        entered = shared = 0
        for case in range(PROPS):
            self.ex = FakeOkx(r.choice(["net_mode", "long_short_mode"]))
            price = 10 ** r.uniform(-5, 5)
            ct_usd = r.random() < 0.3
            ct_val = Decimal(r.choice(["0.0001", "0.001", "0.01", "0.1", "1", "10", "100", "1000000"]))
            lot = Decimal(r.choice(["1", "0.1", "0.01"]))
            min_sz = lot * r.choice([1, 1, 5, 100])
            tick = Decimal(r.choice(["0.1", "0.01", "0.0001", "0.00000001", "0.000000001", "1"]))
            inst = "BTC-USDT-SWAP"
            self.ex.instr = {inst: {"instId": inst, "state": "live", "ctVal": str(ct_val), "ctMult": "1",
                                    "ctValCcy": "USD" if ct_usd else "BTC", "lotSz": str(lot), "minSz": str(min_sz),
                                    "tickSz": str(tick)}}
            side = r.choice([1, -1])
            # Цена китов: чаще рядом (вход возможен), иногда далеко (догон, другой актив).
            ref = price * (1 + (r.uniform(-0.004, 0.004) if r.random() < 0.7 else r.uniform(-0.03, 0.03)))
            self.ex.last = {inst: price}
            self.ex.eq = r.choice([0.0, 50.0, 1000.0, 10_000.0, 1e6 * r.random()])
            # Ручная позиция владельца: нет / на другой бумаге (вход — 5 %) / на той же (не входим).
            manual = r.choice([None] * 7 + ["other", "other", "same"])
            if manual:
                self.ex.pos = [{"instId": "ETH-USD_UM_XPERP-310815" if manual == "other" else inst,
                                "instType": "FUTURES", "pos": "1", "posSide": "net", "avgPx": str(price)}]
            on = r.random() < 0.85
            age = r.choice([1, 5, 14, 16, 60])
            paused = r.random() < 0.15
            if os.path.exists(self.db):
                os.remove(self.db)
            st_file = os.path.join(self.dir, "live", "state.json")
            if os.path.exists(st_file):
                os.remove(st_file)
            fills = [fill(1, "0xa", "BTC", side, self.now - age * MIN - 2 * MIN, 50_000, ref),
                     fill(2, "0xb", "BTC", side, self.now - age * MIN - MIN, 50_000, ref),
                     fill(3, "0xc", "BTC", side, self.now - age * MIN, 50_000, ref)]
            tr = self.trader(fills)
            if paused:
                tr.st["entered"]["BTC"] = self.now - 2 * 3_600_000
            self.on(on)
            tr.step()
            markets = [o for o in self.ex.orders if o["ordType"] == "market"]
            frac = lv.MARGIN_FRAC
            notional = Decimal(repr(round(self.ex.eq * frac * lv.LEVERAGE, 6)))
            qty = notional / (ct_val if ct_usd else ct_val * Decimal(repr(price)))
            want_sz = (qty / lot).to_integral_value(rounding=ROUND_DOWN) * lot
            hl = price
            should = (on and manual != "same" and age <= 15 and not paused
                      and self.ex.eq > 0 and abs(hl / ref - 1) <= lv.MATCH_TOL and side * (hl / ref - 1) <= lv.CHASE
                      and want_sz >= min_sz)
            tp_try = lv.round_step(price * (1 + side * lv.TP), tick, ROUND_CEILING if side > 0 else ROUND_FLOOR)
            should = should and tp_try > 0 and lv.TP * 0.999 <= side * (float(tp_try) / price - 1) <= lv.TP + lv.TP_SLACK
            ctx = f"случай {case}: side {side} price {price} ref {ref} eq {self.ex.eq} ct {ct_val} usd {ct_usd} " \
                  f"lot {lot} min {min_sz} on {on} age {age} paused {paused} ручная {manual}"
            self.assertEqual(len(markets), int(should), ctx)
            if not markets:
                continue
            entered += 1
            shared += manual == "other"
            m = markets[0]
            self.assertEqual(Decimal(m["sz"]), want_sz, ctx)
            self.assertLessEqual(Decimal(m["sz"]) * (ct_val if ct_usd else ct_val * Decimal(repr(price))),
                                 notional * Decimal("1.000001"), ctx)
            limits = [o for o in self.ex.orders if o["ordType"] == "limit"]
            self.assertEqual(len(limits), 1, ctx)
            tp = Decimal(limits[0]["px"])
            self.assertEqual(limits[0]["sz"], m["sz"], ctx)
            self.assertEqual(tp % tick, 0, ctx)
            want_tp = lv.round_step(price * (1 + side * lv.TP), tick, ROUND_CEILING if side > 0 else ROUND_FLOOR)
            self.assertEqual(tp, want_tp, ctx)
            move = side * (float(tp) / price - 1)
            self.assertGreaterEqual(move, lv.TP * 0.999999, ctx)
            self.assertLessEqual(move, min(lv.TP + float(tick) / price + 1e-9, lv.TP + lv.TP_SLACK), ctx)
        # Проверка не пустая: заметная доля случаев дошла до входа.
        self.assertGreater(entered, PROPS // 20)
        self.assertGreater(shared, 0, "ни одного входа при ручной позиции — генератор не проверяет правило")


class FuzzTest(Base):
    def chaos(self, r: random.Random, rate: float):
        real = self.ex.__call__

        def t(method, url, headers, body):
            if r.random() >= rate:
                return real(method, url, headers, body)
            kind = r.randrange(7)
            if kind == 0:
                raise OSError("сеть упала")
            if kind == 1:
                return r.choice([None, [], "x", 5, {"code": 0}])
            if kind == 2:
                return {"code": "0", "data": rnd_garbage(r)}
            if kind == 3:
                return {"code": str(r.choice([1, 50000, 50124, 51008, 50119])), "msg": "x", "data": [{"sCode": "51008"}]}
            # Копия: портим ответ, а не записи самой биржи (её ордера и позиции).
            got = json.loads(json.dumps(real(method, url, headers, body)))
            data = got.get("data") if isinstance(got, dict) else None
            if isinstance(data, list):
                for item in data:
                    if isinstance(item, dict) and item:
                        for k in r.sample(list(item), k=min(len(item), r.randint(1, 3))):
                            item[k] = rnd_garbage(r)
                if kind == 6 and data:
                    data.insert(0, rnd_garbage(r))
            return got

        return t

    def test_garbage_from_exchange_never_breaks_rules(self) -> None:
        r = random.Random(1121)
        for case in range(FUZZ):
            self.ex = FakeOkx(r.choice(["net_mode", "long_short_mode"]))
            for p in (os.path.join(self.dir, "live", "state.json"), self.db):
                if os.path.exists(p):
                    os.remove(p)
            side = r.choice([1, -1])
            tr = self.trader([fill(1, "0xa", "BTC", side, self.now - 3 * MIN, 50_000, 100_000),
                              fill(2, "0xb", "BTC", side, self.now - 2 * MIN, 50_000, 100_000),
                              fill(3, "0xc", "BTC", side, self.now - MIN, 50_000, 100_000)])
            tr.okx._t = self.chaos(r, r.choice([0.1, 0.3, 0.7]))
            self.on()
            entries: list[tuple[dict, bool]] = []
            for _step in range(r.randint(1, 4)):
                known = bool(tr.st.get("open"))  # своя позиция бота (ручные — разрешены, 5 %)
                before = len(self.ex.orders)
                try:
                    tr.step()
                except (lv.OkxError, OSError):
                    pass
                except Exception as e:  # noqa: BLE001 — это и есть нарушение
                    self.fail(f"случай {case}: шаг бросил {type(e).__name__}: {e}")
                for o in self.ex.orders[before:]:
                    if o["ordType"] == "market":
                        entries.append((o, known))
                self.now += r.choice([1, 2, 20]) * MIN
                tr.last_sync = 0
            for o in self.ex.orders:
                ctx = f"случай {case}: {o}"
                self.assertRegex(str(o.get("sz")), NUM_RE, ctx)
                self.assertGreater(Decimal(o["sz"]), 0, ctx)
                if o["ordType"] == "limit":
                    self.assertRegex(str(o.get("px")), NUM_RE, ctx)
                    entry = next((e for e, _k in entries if e["instId"] == o["instId"]), None)
                    if entry is not None:
                        fill_px = 100_000.0
                        s = 1 if entry["side"] == "buy" else -1
                        self.assertEqual(o["side"], "sell" if s > 0 else "buy", ctx)
                        self.assertGreaterEqual(s * (float(o["px"]) / fill_px - 1), lv.TP * 0.999, ctx)
            self.assertLessEqual(len(entries), 1, f"случай {case}: больше одного входа")
            for _o, known in entries:
                self.assertFalse(known, f"случай {case}: второй вход при своей открытой позиции")
            for name in os.listdir(os.path.join(self.dir, "live")):
                with open(os.path.join(self.dir, "live", name), encoding="utf-8") as f:
                    text = f.read()
                self.assertNotIn("SECRET-very-private", text)
                self.assertNotIn("PASS-x", text)
                if name.endswith(".json"):
                    json.loads(text)


if __name__ == "__main__":
    r = unittest.main(exit=False, verbosity=1).result
    if r.wasSuccessful():
        print(f"autotrade_live_props: {PROPS} случаев свойств и {FUZZ} — фаззинга: инварианты соблюдены")
    sys.exit(0 if r.wasSuccessful() else 1)
