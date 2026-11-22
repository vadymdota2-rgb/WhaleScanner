#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Реальная торговля автокопирования (tools/autotrade_live.py) — на поддельной
OKX и синтетической базе: без сети, без ключей, без денег.

Проверяется: подпись запросов OKX; сигнал — 2 кита от 1000 $ за 2 часа;
размер — 5 % капитала × 10×; тейк +1 % лимитным ордером reduce-only; одна
позиция на счёте (и ручная владельца тоже блокирует); пауза 3 часа на
бумагу; не догоняем дальше 0,3 %; другой актив с тем же именем — пропуск;
«Стоп» — новых входов нет, позиция не закрывается; закрытие — запись сделки
и снятие оставшегося тейка; пропавший тейк ставится заново; режим
long/short; kPEPE → PEPE ×1000; без ключей — понятная ошибка; секреты не
попадают в файлы состояния.
"""
import base64
import hashlib
import hmac
import json
import os
import sqlite3
import sys
import tempfile
import unittest
import urllib.parse
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import autotrade_live as lv  # noqa: E402

T0 = 1_800_000_000_000
MIN = 60_000


class FakeOkx:
    """Биржа в памяти: каталог, цены, счёт, позиции, ордера."""

    def __init__(self, pos_mode: str = "net_mode") -> None:
        self.instr = {"BTC-USDT-SWAP": {"instId": "BTC-USDT-SWAP", "state": "live", "ctVal": "0.01", "ctMult": "1",
                                        "lotSz": "0.01", "minSz": "0.01", "tickSz": "0.1"},
                      "PEPE-USDT-SWAP": {"instId": "PEPE-USDT-SWAP", "state": "live", "ctVal": "10000000",
                                         "ctMult": "1", "lotSz": "1", "minSz": "1", "tickSz": "0.00000001"},
                      "STX-USDT-SWAP": {"instId": "STX-USDT-SWAP", "state": "live", "ctVal": "10", "ctMult": "1",
                                        "lotSz": "1", "minSz": "1", "tickSz": "0.0001"}}
        self.last = {"BTC-USDT-SWAP": 100_000.0, "PEPE-USDT-SWAP": 0.00001, "STX-USDT-SWAP": 0.5}
        self.eq = 20_000.0   # маржа 5 % × 10× = позиция 10 000 $
        self.pos: list[dict] = []
        self.orders: list[dict] = []
        self.history: list[dict] = []
        self.calls: list[tuple[str, str, dict]] = []
        self.pos_mode = pos_mode
        self.n = 0
        self.reject: dict[str, str] = {}   # бумага → код отказа OKX на ордер

    def __call__(self, method: str, url: str, headers: dict, body: bytes | None):
        u = urllib.parse.urlparse(url)
        q = dict(urllib.parse.parse_qsl(u.query))
        b = json.loads(body) if body else {}
        self.calls.append((method, u.path, {**q, **b}))
        ok = lambda data: {"code": "0", "msg": "", "data": data}  # noqa: E731
        p = u.path
        if p == "/api/v5/public/instruments":
            kind = q.get("instType", "SWAP")
            return ok([r for r in self.instr.values() if r.get("instType", "SWAP") == kind])
        if p == "/api/v5/market/ticker":
            return ok([{"instId": q["instId"], "last": str(self.last[q["instId"]])}])
        if p == "/api/v5/account/config":
            return ok([{"posMode": self.pos_mode, "acctLv": "2"}])
        if p == "/api/v5/account/balance":
            return ok([{"totalEq": str(self.eq)}])
        if p == "/api/v5/account/positions":
            return ok(self.pos)
        if p == "/api/v5/account/set-leverage":
            return ok([{"lever": b["lever"]}])
        if p == "/api/v5/trade/order" and method == "POST" and b["instId"] in self.reject:
            code = self.reject[b["instId"]]
            return {"code": "1", "msg": "", "data": [{"sCode": code, "sMsg": "This API Key does not have trading "
                                                      "permission for the market"}]}
        if p == "/api/v5/trade/order" and method == "POST":
            self.n += 1
            o = {**b, "ordId": str(self.n), "state": "live", "cTime": str(T0)}
            closing = b.get("reduceOnly") or (b.get("posSide") == "long" and b["side"] == "sell") \
                or (b.get("posSide") == "short" and b["side"] == "buy")
            if b["ordType"] == "market" and closing:
                # Закрытие: уменьшить свою позицию (не открыть встречную).
                px = self.last[b["instId"]]
                o.update(state="filled", avgPx=str(px), accFillSz=b["sz"], fillTime=str(T0))
                for p in self.pos:
                    if p["instId"] == b["instId"] and p.get("posSide", "net") == b.get("posSide", "net"):
                        left = abs(float(p["pos"])) - float(b["sz"])
                        sign = 1 if float(p["pos"]) > 0 else -1
                        o["pnl"] = str(round(float(b["sz"]) * (px - float(p["avgPx"])) * (1 if b["side"] == "sell" else -1)
                                             * float(self.instr[b["instId"]]["ctVal"]), 6))
                        p["pos"] = str(sign * left)
                self.pos = [p for p in self.pos if abs(float(p["pos"])) > 1e-12]
            elif b["ordType"] == "market" and any(
                    p["instId"] == b["instId"] and p.get("posSide", "net") == b.get("posSide", "net")
                    and (b.get("posSide") or (float(p["pos"]) > 0) == (b["side"] == "buy")) for p in self.pos):
                # Доливка в ту же позицию (усреднение): объём растёт, средняя — взвешенная.
                px = self.last[b["instId"]]
                o.update(state="filled", avgPx=str(px), accFillSz=b["sz"])
                p = next(p for p in self.pos if p["instId"] == b["instId"] and p.get("posSide", "net") == b.get("posSide", "net"))
                old, add = abs(float(p["pos"])), float(b["sz"])
                p["avgPx"] = str((float(p["avgPx"]) * old + px * add) / (old + add))
                p["pos"] = str((1 if float(p["pos"]) > 0 else -1) * (old + add))
            elif b["ordType"] == "market":
                o.update(state="filled", avgPx=str(self.last[b["instId"]]), accFillSz=b["sz"])
                sign = 1 if b["side"] == "buy" else -1
                self.pos.append({"instId": b["instId"], "pos": str(sign * float(b["sz"]) if self.pos_mode == "net_mode"
                                                                      else float(b["sz"])),
                                 "posSide": b.get("posSide", "net"), "avgPx": o["avgPx"], "markPx": o["avgPx"],
                                 "upl": "0", "lever": "10", "mgnMode": "cross", "cTime": "1"})
            self.orders.append(o)
            return ok([{"ordId": o["ordId"], "clOrdId": b.get("clOrdId", ""), "sCode": "0"}])
        if p == "/api/v5/trade/order":
            o = next((o for o in self.orders if o["ordId"] == q.get("ordId") or o.get("clOrdId") == q.get("clOrdId")), None)
            return ok([o] if o else [])
        if p in ("/api/v5/trade/orders-pending", "/api/v5/account/positions-history"):
            # Как OKX: вид контракта должен совпадать с бумагой (XPERP — FUTURES).
            kind = self.instr.get(q["instId"], {}).get("instType", "SWAP")
            if q.get("instType", kind) != kind:
                return ok([])
        if p == "/api/v5/trade/orders-history":
            return ok([o for o in reversed(self.orders) if o["instId"] == q["instId"] and o["state"] != "live"])
        if p == "/api/v5/trade/orders-pending":
            return ok([o for o in self.orders if o["state"] in ("live", "partially_filled") and o["instId"] == q["instId"]])
        if p == "/api/v5/trade/cancel-order":
            for o in self.orders:
                if o["ordId"] == b["ordId"]:
                    o["state"] = "canceled"
            return ok([{"ordId": b["ordId"], "sCode": "0"}])
        if p == "/api/v5/account/positions-history":
            return ok([h for h in self.history if h["instId"] == q["instId"]])
        return {"code": "50000", "msg": f"нет такого адреса: {p}", "data": []}


def make_db(path: str, fills: list[tuple]) -> None:
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE hl_fills (tid INTEGER PRIMARY KEY, wallet TEXT, coin TEXT, dir_code INTEGER, "
                "ts INTEGER, notional_nanos INTEGER, sz TEXT, closed_pnl_nanos INTEGER NOT NULL DEFAULT 0, "
                "dir TEXT NOT NULL DEFAULT '')")
    con.executemany(INS, fills)
    con.commit()
    con.close()


INS = "INSERT INTO hl_fills(tid, wallet, coin, dir_code, ts, notional_nanos, sz) VALUES (?,?,?,?,?,?,?)"


def close_fill(path: str, tid: int, wallet: str, coin: str, code: int, ts: int, pnl_usd: float, d: str = "") -> None:
    """Кит закрыл (3/4 — закрытие лонга/шорта, 5 — разворот, 6/7 — ликвидация) с итогом pnl_usd."""
    con = sqlite3.connect(path)
    con.execute("INSERT INTO hl_fills(tid, wallet, coin, dir_code, ts, notional_nanos, sz, closed_pnl_nanos, dir) "
                "VALUES (?,?,?,?,?,?,?,?,?)", (tid, wallet, coin, code, ts, 5000 * 10**9, "0.05", int(pnl_usd * 1e9), d))
    con.commit()
    con.close()


def fill(tid: int, wallet: str, coin: str, side: int, ts: int, usd: float, px: float) -> tuple:
    return (tid, wallet, coin, 1 if side > 0 else 2, ts, int(usd * 1e9), str(usd / px))


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.db = os.path.join(self.tmp.name, "hl.db")
        self.dir = os.path.join(self.tmp.name, "data")
        os.makedirs(self.dir)
        self.now = T0
        self.ex = FakeOkx()
        self.logs: list[str] = []
        sleep = mock.patch.object(lv.time, "sleep", lambda _s: None)
        sleep.start()
        self.addCleanup(sleep.stop)

    def on(self, value: bool = True) -> None:
        with open(os.path.join(self.dir, "switch.json"), "w") as f:
            json.dump({"on": value}, f)

    def trader(self, fills: list[tuple], ex: FakeOkx | None = None) -> lv.Trader:
        make_db(self.db, fills)
        okx = lv.Okx("KEY-x", "SECRET-very-private", "PASS-x", False, ex or self.ex)
        return lv.Trader(okx, lv.Signals(self.db), self.dir, clock=lambda: self.now, log=self.logs.append)

    def state(self) -> dict:
        with open(os.path.join(self.dir, "live", "state.json"), encoding="utf-8") as f:
            return json.load(f)

    def market_orders(self) -> list[dict]:
        return [o for o in self.ex.orders if o["ordType"] == "market"]

    def whales(self, coin: str = "BTC", side: int = 1, px: float = 100_000, usd: float = 5000, ago: int = 5) -> list:
        return [fill(1, "0xa", coin, side, self.now - ago * MIN - MIN, usd, px),
                fill(2, "0xb", coin, side, self.now - ago * MIN, usd, px)]


class SignTest(Base):
    def test_signature_and_headers(self) -> None:
        seen = {}

        def t(method, url, headers, body):
            seen.update(headers=headers, url=url, body=body)
            return {"code": "0", "data": [{"totalEq": "1"}]}

        okx = lv.Okx("K", "S", "P", True, t)
        okx.equity()
        h = seen["headers"]
        msg = h["OK-ACCESS-TIMESTAMP"] + "GET" + "/api/v5/account/balance"
        want = base64.b64encode(hmac.new(b"S", msg.encode(), hashlib.sha256).digest()).decode()
        self.assertEqual(h["OK-ACCESS-SIGN"], want)
        self.assertEqual((h["OK-ACCESS-KEY"], h["OK-ACCESS-PASSPHRASE"], h["x-simulated-trading"]), ("K", "P", "1"))
        self.assertRegex(h["OK-ACCESS-TIMESTAMP"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$")
        # Тело POST подписывается тем же текстом, что уходит.
        okx.set_leverage("BTC-USDT-SWAP", 10)
        msg = seen["headers"]["OK-ACCESS-TIMESTAMP"] + "POST/api/v5/account/set-leverage" + seen["body"].decode()
        self.assertEqual(seen["headers"]["OK-ACCESS-SIGN"],
                         base64.b64encode(hmac.new(b"S", msg.encode(), hashlib.sha256).digest()).decode())

    def test_detect_finds_eea_address_for_european_key(self) -> None:
        """Ключ европейского аккаунта (OKX EEA): www отвечает 50119 «ключа нет»
        — бот пробует eea.okx.com и остаётся там; подпись — без адреса."""
        hosts = []

        def t(method, url, headers, body):
            hosts.append(urllib.parse.urlparse(url).netloc)
            if "eea." in url:
                return {"code": "0", "data": [{"posMode": "net_mode"}]}
            return {"code": "50119", "msg": "API key doesn't exist", "data": []}

        okx = lv.Okx("K", "S", "P", False, t)
        self.assertEqual(okx.detect(), "https://eea.okx.com")
        self.assertEqual(hosts, ["www.okx.com", "eea.okx.com"])
        hosts.clear()
        okx.detect("https://eea.okx.com")  # запомненный — первым, без лишних запросов
        self.assertEqual(hosts, ["eea.okx.com"])
        # Нигде нет — понятная причина; другая ошибка (IP) — сразу, без перебора.
        none = lv.Okx("K", "S", "P", False, lambda *_: {"code": "50119", "msg": "x", "data": []})
        with self.assertRaisesRegex(lv.OkxError, "не найден ни на одном адресе"):
            none.detect()
        calls = []
        ip = lv.Okx("K", "S", "P", False, lambda *a: calls.append(a) or {"code": "50110", "msg": "ip", "data": []})
        with self.assertRaisesRegex(lv.OkxError, "IP сервера"):
            ip.detect()
        self.assertEqual(len(calls), 1)

    def test_error_is_raised_with_reason(self) -> None:
        okx = lv.Okx("K", "S", "P", False, lambda *_: {"code": "1", "msg": "", "data": [{"sCode": "51008", "sMsg": "мало маржи"}]})
        with self.assertRaisesRegex(lv.OkxError, "51008: мало маржи"):
            okx.order({})


class TradeTest(Base):
    def test_two_whales_enter_with_owner_size_and_tp(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        [m] = self.market_orders()
        # 5 % от 20 000 $ × 10 = 10 000 $; контракт 0,01 BTC × 100 000 = 1 000 $ → 10 контрактов.
        self.assertEqual((m["instId"], m["side"], m["tdMode"], m["sz"]), ("BTC-USDT-SWAP", "buy", "cross", "10"))
        self.assertIn(("POST", "/api/v5/account/set-leverage", {"instId": "BTC-USDT-SWAP", "lever": "10", "mgnMode": "cross"}),
                      self.ex.calls)
        [tp] = [o for o in self.ex.orders if o["ordType"] == "limit"]
        self.assertEqual((tp["side"], tp["px"], tp["sz"], tp["reduceOnly"]), ("sell", "101000", "10", True))
        st = self.state()
        self.assertEqual((st["open"]["inst"], st["open"]["tp"]), ("BTC-USDT-SWAP", 101000.0))
        tr.sync()
        tr.save()
        self.assertEqual(set(self.state()["positions"][0]), {"instId", "pos", "avgPx", "upl"},
                         "во вкладку — только то, что она читает")

    def test_one_whale_or_small_whale_no_trade(self) -> None:
        for fills in ([fill(1, "0xa", "BTC", 1, self.now - MIN, 5000, 100_000)],
                      [fill(1, "0xa", "BTC", 1, self.now - 2 * MIN, 5000, 100_000),
                       fill(2, "0xb", "BTC", 1, self.now - MIN, 900, 100_000)],
                      [fill(1, "0xa", "BTC", 1, self.now - 3 * 60 * MIN, 5000, 100_000),  # старше 2 часов
                       fill(2, "0xb", "BTC", 1, self.now - MIN, 5000, 100_000)],
                      [fill(1, "0xa", "BTC", 1, self.now - 2 * MIN, 5000, 100_000),     # разные стороны
                       fill(2, "0xb", "BTC", -1, self.now - MIN, 5000, 100_000)]):
            self.ex = FakeOkx()
            tr = self.trader(fills)
            self.on()
            tr.step()
            self.assertEqual(self.market_orders(), [], fills)
            os.remove(self.db)

    def test_switch_off_no_entry(self) -> None:
        tr = self.trader(self.whales())
        tr.step()
        self.assertEqual(self.market_orders(), [])

    def test_manual_position_same_5_percent(self) -> None:
        """Правило владельца: маржа всегда 5 %; ручная сделка на счёте (любого
        вида — здесь срочный фьючерс) входу не мешает и размер не меняет:
        20 000 $ × 5 % × 10 = 10 000 $ → 10 контрактов по 1 000 $."""
        self.ex.pos = [{"instId": "ETH-USDT-250328", "instType": "FUTURES", "pos": "-3", "posSide": "net", "avgPx": "3000"}]
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        [m] = self.market_orders()
        self.assertEqual((m["instId"], m["sz"]), ("BTC-USDT-SWAP", "10"))
        [tp] = [o for o in self.ex.orders if o["ordType"] == "limit"]
        self.assertEqual(tp["sz"], "10")
        self.assertEqual(self.state()["open"]["margin_pct"], 5.0)
        self.assertTrue(any("маржа 5 %" in e for e in self.state()["events"]))
        # Запрос позиций — без фильтра по виду (все позиции счёта).
        q = [c[2] for c in self.ex.calls if c[1] == "/api/v5/account/positions"]
        self.assertTrue(q and all("instType" not in x for x in q))
        # Своя позиция у бота — одна: пока она открыта, второй нет (и при ручной).
        self.now += 2 * MIN
        tr.last_sync = 0
        self.assertIn("у бота уже есть своя позиция", tr.try_enter({"coin": "SOL", "side": 1, "ts": self.now,
                                                                    "whales": 2, "ref_px": 100.0}))

    def test_no_manual_position_also_5_percent(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        self.assertEqual(self.market_orders()[0]["sz"], "10")
        self.assertEqual(self.state()["open"]["margin_pct"], 5.0)

    def test_same_instrument_as_manual_is_skipped(self) -> None:
        """Ручная позиция на той же бумаге: сделка бота смешалась бы с ней
        (в режиме net — вычлась бы), а тейк бота закрыл бы часть ручной."""
        self.ex.pos = [{"instId": "BTC-USDT-SWAP", "pos": "2", "posSide": "net", "avgPx": "90000", "markPx": "100000"}]
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        self.assertEqual(self.market_orders(), [])
        self.assertIn("уже ваша позиция", self.state()["last_skip"]["why"])

    def test_retaken_tp_is_only_bots_share(self) -> None:
        """Тейк сняли, а позиция на бумаге выросла (докупили руками): новый тейк
        — только на объём бота, не на всю позицию."""
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        for o in self.ex.orders:
            if o["ordType"] == "limit":
                o["state"] = "canceled"
        self.ex.pos[0]["pos"] = "25"
        self.now += 2 * MIN
        tr.last_sync = 0
        tr.step()
        live = [o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"]
        self.assertEqual([o["sz"] for o in live], ["10"])
    def test_chase_and_other_asset_and_stale(self) -> None:
        cases = [(100_400, "цена ушла от китов"), (110_000, "другой актив"), (99_000, "")]
        for okx_px, why in cases:
            self.ex = FakeOkx()
            self.ex.last["BTC-USDT-SWAP"] = okx_px
            tr = self.trader(self.whales())
            self.on()
            tr.step()
            got = self.state().get("last_skip", {}).get("why", "")
            if why:
                self.assertEqual(self.market_orders(), [], okx_px)
                self.assertIn(why, got)
            else:
                self.assertEqual(len(self.market_orders()), 1, "ниже китов на лонг — входим всегда")
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))
        self.ex = FakeOkx()
        tr = self.trader(self.whales(ago=20))  # сигнал 20 минут назад — момент ушёл
        self.on()
        tr.step()
        self.assertEqual(self.market_orders(), [])

    def test_short_in_long_short_mode_and_pepe_scale(self) -> None:
        self.ex = FakeOkx("long_short_mode")
        tr = self.trader(self.whales("kPEPE", -1, px=0.01))  # kPEPE на Hyperliquid = 1000 PEPE
        self.on()
        tr.step()
        [m] = self.market_orders()
        # 10 000 $ / (10 000 000 PEPE × 0,00001 $) = 100 контрактов.
        self.assertEqual((m["instId"], m["side"], m["posSide"], m["sz"]), ("PEPE-USDT-SWAP", "sell", "short", "100"))
        [tp] = [o for o in self.ex.orders if o["ordType"] == "limit"]
        self.assertEqual((tp["side"], tp["posSide"], tp["px"]), ("buy", "short", "0.0000099"))
        self.assertNotIn("reduceOnly", tp)

    def test_close_records_trade_cancels_tp_then_pause_3h(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        tp_id = tr.st["open"]["tp_id"]
        # Тейк исполнился: позиции нет, в истории — закрытие по 101 000.
        self.now += 30 * MIN
        self.ex.pos = []
        self.ex.history = [{"instId": "BTC-USDT-SWAP", "uTime": str(self.now), "realizedPnl": "88.5",
                            "closeAvgPx": "101000", "fee": "-9", "fundingFee": "-2.5"}]
        tr.last_sync = 0
        tr.step()
        with open(os.path.join(self.dir, "live", "trades.jsonl"), encoding="utf-8") as f:
            [t] = [json.loads(x) for x in f]
        self.assertEqual((t["why"], t["pnl"], t["side"], t["px_in"]), ("тейк", 88.5, 1, 100_000.0))
        self.assertIsNone(self.state()["open"])
        self.assertEqual(next(o for o in self.ex.orders if o["ordId"] == tp_id)["state"], "canceled")
        # Новые киты по той же бумаге через час — пауза 3 часа; по другой — можно было бы.
        self.now += 30 * MIN
        make_db(self.db + "2", [*self.whales()])
        tr.sig = lv.Signals(self.db + "2")
        tr.step()
        self.assertEqual(len(self.market_orders()), 1)
        self.assertIn("3 часов", self.state()["last_skip"]["why"])

    def test_missing_tp_is_placed_again_and_stop_keeps_position(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        for o in self.ex.orders:
            if o["ordType"] == "limit":
                o["state"] = "canceled"   # кто-то снял тейк
        self.now += 2 * MIN
        self.on(False)                    # «Стоп»
        tr.last_sync = 0
        tr.step()
        live_tp = [o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"]
        self.assertEqual(len(live_tp), 1, "тейк поставлен заново")
        self.assertEqual(len(self.ex.pos), 1, "«Стоп» позицию не закрывает")
        self.assertFalse(any(c[1] == "/api/v5/trade/close-position" for c in self.ex.calls))

    def test_secrets_never_in_files(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        for name in os.listdir(os.path.join(self.dir, "live")):
            with open(os.path.join(self.dir, "live", name), encoding="utf-8") as f:
                text = f.read()
            self.assertNotIn("SECRET-very-private", text, name)
            self.assertNotIn("PASS-x", text, name)

    def test_no_keys_is_a_clear_error(self) -> None:
        env = {k: v for k, v in os.environ.items() if not k.startswith("OKX_")}
        with mock.patch.dict(os.environ, env, clear=True), mock.patch("sys.stdout"):
            self.assertEqual(lv.main(["--db", self.db, "--dir", self.dir]), 1)
        self.assertIn("нет ключа OKX", self.state()["error"])


class SignalsTest(Base):
    def test_future_fills_ignored_until_their_time(self) -> None:
        make_db(self.db, [fill(1, "0xa", "BTC", 1, self.now + 10 * MIN, 5000, 100_000),
                          fill(2, "0xb", "BTC", 1, self.now + 11 * MIN, 5000, 100_000)])
        sig = lv.Signals(self.db)
        self.assertEqual(sig.poll(self.now), [])
        self.assertEqual(len(sig.poll(self.now + 12 * MIN)), 1)

    def test_window_drops_whales_older_than_two_hours(self) -> None:
        """Кит A — давно (3 часа назад от второго опроса), кит B — сейчас: двух
        китов за 2 часа нет — сигнала нет."""
        make_db(self.db, [fill(1, "0xa", "BTC", 1, self.now - MIN, 5000, 100_000),
                          fill(2, "0xb", "BTC", 1, self.now + 3 * 60 * MIN, 5000, 100_000)])
        sig = lv.Signals(self.db)
        self.assertEqual(sig.poll(self.now), [])
        self.assertEqual(sig.poll(self.now + 3 * 60 * MIN + MIN), [])


class OrderTypeTest(Base):
    def test_entry_always_market_exit_always_limit_at_tp(self) -> None:
        """Правило владельца: вход — всегда рыночным ордером, выход — лимитным
        по цене тейка; по рынку бот закрывает только вслед за китом, вышедшим
        в убыток (WhaleExitTest) — без этого рыночного закрытия нет."""
        for mode, side in (("net_mode", 1), ("long_short_mode", -1)):
            self.ex = FakeOkx(mode)
            tr = self.trader(self.whales(side=side))
            self.on()
            tr.step()
            opening = "buy" if side > 0 else "sell"
            entries = [o for o in self.ex.orders if o["side"] == opening]
            exits = [o for o in self.ex.orders if o["side"] != opening]
            self.assertEqual([o["ordType"] for o in entries], ["market"], mode)
            self.assertEqual([o["ordType"] for o in exits], ["limit"], mode)
            self.assertEqual(float(exits[0]["px"]), tr.st["open"]["tp"])
            self.assertAlmostEqual(tr.st["open"]["tp"] / tr.st["open"]["px"], 1 + side * lv.TP)
            self.assertFalse(any(c[1] in ("/api/v5/trade/close-position", "/api/v5/trade/order-algo") for c in self.ex.calls))
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))


class WhaleExitTest(Base):
    """Правило владельца: кто-то из китов сигнала закрыл свою сделку по этой
    бумаге в убытке (стоп, ликвидация, «ситуация изменилась») — бот снимает
    тейк и закрывает свою позицию рыночным ордером только на закрытие. Кит
    закрыл в плюс или это не кит сигнала — сидим до тейка."""

    def enter(self, mode: str = "net_mode", side: int = 1) -> lv.Trader:
        self.ex = FakeOkx(mode)
        tr = self.trader(self.whales(side=side))
        self.on()
        tr.step()
        self.assertEqual(sorted(tr.st["open"]["wallets"]), ["0xa", "0xb"])
        return tr

    def trades(self) -> list[dict]:
        path = os.path.join(self.dir, "live", "trades.jsonl")
        if not os.path.exists(path):
            return []
        with open(path, encoding="utf-8") as f:
            return [json.loads(x) for x in f]

    def test_whale_closes_at_loss_bot_closes_by_market(self) -> None:
        for mode, side in (("net_mode", 1), ("long_short_mode", -1)):
            tr = self.enter(mode, side)
            self.ex.last["BTC-USDT-SWAP"] = 100_000 * (1 - side * 0.004)   # цена пошла против
            self.now += 20 * MIN
            close_fill(self.db, 50, "0xa", "BTC", 3 if side > 0 else 4, self.now - MIN, -120.0)
            tr.last_sync = 0
            tr.step()
            exits = [o for o in self.ex.orders if o["ordType"] == "market" and o["side"] == ("sell" if side > 0 else "buy")]
            self.assertEqual(len(exits), 1, mode)
            ex = exits[0]
            self.assertEqual(ex["sz"], "10", "закрываем ровно свой объём")
            if mode == "net_mode":
                self.assertTrue(ex["reduceOnly"], "только закрытие — не перевернуть позицию")
            else:
                self.assertEqual(ex["posSide"], "short")
            self.assertEqual(self.ex.pos, [], "позиции нет")
            self.assertEqual([o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"], [],
                             "тейк снят")
            [t] = self.trades()
            self.assertEqual(t["why"], "кит вышел в убыток")
            self.assertLess(t["pnl"], 0)
            self.assertIsNone(self.state()["open"])
            self.assertTrue(any("закрыл в убыток" in e for e in self.state()["events"]))
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))
            os.remove(os.path.join(self.dir, "live", "trades.jsonl"))

    def test_whale_profit_or_other_whale_or_other_coin_keeps_position(self) -> None:
        tr = self.enter()
        self.now += 20 * MIN
        close_fill(self.db, 50, "0xa", "BTC", 3, self.now - MIN, +300.0)     # кит сигнала — в плюс
        close_fill(self.db, 51, "0xzz", "BTC", 3, self.now - MIN, -900.0)    # не кит сигнала
        close_fill(self.db, 52, "0xb", "ETH", 3, self.now - MIN, -900.0)     # другая бумага
        close_fill(self.db, 53, "0xb", "BTC", 4, self.now - MIN, -900.0)     # закрытие шорта — не наша сторона
        for _ in range(3):
            tr.last_sync = 0
            self.now += MIN
            tr.step()
        self.assertEqual(len(self.market_orders()), 1, "только вход")
        self.assertEqual(self.state()["open"]["inst"], "BTC-USDT-SWAP")
        self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"]), 1)

    def test_partial_profits_then_loss_sum_decides(self) -> None:
        """Кит закрывал частями: итог закрытий в минусе — выходим; в плюсе — нет."""
        tr = self.enter()
        self.now += 20 * MIN
        close_fill(self.db, 50, "0xb", "BTC", 3, self.now - 3 * MIN, +50.0)
        close_fill(self.db, 51, "0xb", "BTC", 3, self.now - 2 * MIN, -20.0)
        tr.last_sync = 0
        tr.step()
        self.assertIsNotNone(self.state()["open"], "итог +30 — сидим")
        close_fill(self.db, 52, "0xb", "BTC", 3, self.now - MIN, -40.0)
        tr.last_sync = 0
        tr.step()
        self.assertIsNone(self.state()["open"], "итог −10 — выходим")

    def test_liquidation_and_flip_count_as_loss_close(self) -> None:
        for code, d in ((6, "Liquidated Isolated Long"), (5, "Long > Short")):
            tr = self.enter()
            self.now += 20 * MIN
            close_fill(self.db, 50, "0xa", "BTC", code, self.now - MIN, -500.0, d)
            tr.last_sync = 0
            tr.step()
            self.assertIsNone(self.state()["open"], code)
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))

    def test_old_loss_before_whale_entered_does_not_count(self) -> None:
        """Убыток кита по прошлой сделке (до его входа в окне сигнала) — не повод:
        у каждого кита — от его первой сделки (0xa вошёл в −6 мин, 0xb — в −5)."""
        tr = self.enter()
        close_fill(self.db, 50, "0xa", "BTC", 3, self.now - 60 * MIN, -500.0)
        close_fill(self.db, 51, "0xb", "BTC", 3, self.now - 5 * MIN - 30_000, -500.0)  # раньше входа 0xb
        self.now += 20 * MIN
        tr.last_sync = 0
        tr.step()
        self.assertIsNotNone(self.state()["open"])

    def test_whale_already_lost_before_entry_no_entry(self) -> None:
        fills = self.whales()
        make_db(self.db, fills)
        close_fill(self.db, 50, "0xa", "BTC", 3, self.now - MIN, -80.0)
        okx = lv.Okx("K", "S", "P", False, self.ex)
        tr = lv.Trader(okx, lv.Signals(self.db), self.dir, clock=lambda: self.now, log=self.logs.append)
        self.on()
        tr.step()
        self.assertEqual(self.market_orders(), [])
        st = self.state()
        self.assertEqual(st["last_skip"]["code"], "whale_exit")
        self.assertIn("уже закрыл позицию в убыток", st["last_skip"]["why"])

    def test_failed_close_keeps_trying_and_never_replaces_tp(self) -> None:
        tr = self.enter()
        self.now += 20 * MIN
        close_fill(self.db, 50, "0xa", "BTC", 3, self.now - MIN, -120.0)
        real = tr.okx.order
        tries = []

        def order(body):
            if body["ordType"] == "market":
                tries.append(body)
                raise lv.OkxError("50001", "сервис занят")
            return real(body)

        tr.okx.order = order  # type: ignore[method-assign]
        tr.last_sync = 0
        tr.step()
        self.assertIn("exit", self.state()["open"])
        self.assertTrue(any("не вышло" in e for e in self.state()["events"]))
        for _ in range(3):      # 15-секундные сверки: тейк не ставится заново, ордер — не чаще раза в минуту
            self.now += 15_000
            tr.last_sync = 0
            tr.step()
        self.assertEqual([o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"], [])
        self.assertEqual(len(tries), 1, "повтор — не чаще раза в минуту")
        tr.okx.order = real  # type: ignore[method-assign]
        self.now += 60_000
        tr.last_sync = 0
        tr.step()
        self.assertIsNone(self.state()["open"])
        self.assertEqual(self.trades()[-1]["why"], "кит вышел в убыток")

    def test_close_only_bots_share_when_owner_added(self) -> None:
        """Владелец докупил руками на той же бумаге (позиция 25, у бота 10):
        вслед за китом закрываются только 10 бота."""
        tr = self.enter()
        self.ex.pos[0]["pos"] = "25"
        self.now += 20 * MIN
        close_fill(self.db, 50, "0xa", "BTC", 3, self.now - MIN, -50.0)
        tr.last_sync = 0
        tr.step()
        [ex] = [o for o in self.ex.orders if o["ordType"] == "market" and o["side"] == "sell"]
        self.assertEqual(ex["sz"], "10")
        self.assertEqual(self.ex.pos[0]["pos"], "15.0", "ручная часть осталась")
        for _ in range(2):  # ручную часть бот своей не принимает (вход этой сделки уже закрыт)
            self.now += 2 * MIN
            tr.last_sync = 0
            tr._checked.clear()
            tr.step()
        self.assertIsNone(self.state()["open"])
        self.assertEqual(len(self.trades()), 1)

    def test_position_from_old_version_recovers_whales_from_db(self) -> None:
        """Позиция открыта прежней версией: китов в записи нет — бот находит
        их по базе (кто открывал за 2 часа до сигнала от 1000 $) и следит."""
        tr = self.enter()
        del tr.st["open"]["wallets"]
        con = sqlite3.connect(self.db)   # мелкий (100 $) в том же окне — не кит сигнала
        con.execute(INS, fill(49, "0xsmall", "BTC", 1, self.now - 7 * MIN, 100, 100_000))
        con.commit()
        con.close()
        tr.save()
        self.now += 20 * MIN
        tr.last_sync = 0
        tr.step()
        self.assertEqual(sorted(self.state()["open"]["wallets"]), ["0xa", "0xb"])
        close_fill(self.db, 50, "0xb", "BTC", 3, self.now - MIN, -30.0)
        tr.last_sync = 0
        tr.step()
        self.assertIsNone(self.state()["open"])
        self.assertTrue(any("восстановлены по базе" in e for e in self.state()["events"]))

    def test_flip_into_our_side_is_not_an_exit(self) -> None:
        """Ревизия: разворот кита «Short > Long» — он закрыл встречный шорт в
        минус и перешёл в лонг вместе с нами. Раньше бот закрывал свой лонг."""
        tr = self.enter()
        self.now += 20 * MIN
        close_fill(self.db, 50, "0xa", "BTC", 5, self.now - MIN, -300.0, "Short > Long")
        tr.last_sync = 0
        tr.step()
        self.assertIsNotNone(self.state()["open"])
        self.assertEqual(len(self.market_orders()), 1)

    def test_lost_reply_on_exit_does_not_close_owner_part(self) -> None:
        """Ревизия: ордер выхода исполнился, а ответ не дошёл (ошибка шлюза);
        владелец докупил руками (25, у бота 10). Повтор через минуту находит
        ордер по метке и не закрывает ещё 10 — из ручной части."""
        tr = self.enter()
        self.ex.pos[0]["pos"] = "25"
        self.now += 20 * MIN
        close_fill(self.db, 50, "0xa", "BTC", 3, self.now - MIN, -50.0)
        real = tr.okx.order

        def order(body):
            got = real(body)
            if body["clOrdId"].startswith("wtex"):
                raise lv.OkxError("502", "HTTP")
            return got

        tr.okx.order = order  # type: ignore[method-assign]
        tr.last_sync = 0
        tr.step()
        self.assertEqual(self.ex.pos[0]["pos"], "15.0")
        for _ in range(3):
            self.now += 61_000
            tr.last_sync = 0
            tr.step()
        exits = [o for o in self.ex.orders if o["ordType"] == "market" and o["side"] == "sell"]
        self.assertEqual(len(exits), 1, "второго ордера выхода нет")
        self.assertEqual(self.ex.pos[0]["pos"], "15.0", "ручная часть цела")
        self.assertIsNone(self.state()["open"])
        self.assertEqual(self.trades()[-1]["why"], "кит вышел в убыток")

    def test_stop_button_still_follows_whale_out(self) -> None:
        tr = self.enter()
        self.on(False)
        self.now += 20 * MIN
        close_fill(self.db, 50, "0xb", "BTC", 3, self.now - MIN, -10.0)
        tr.last_sync = 0
        tr.step()
        self.assertIsNone(self.state()["open"])

    def test_adopted_after_crash_keeps_whales(self) -> None:
        """Служба упала между входом и записью: принятая позиция помнит китов
        сигнала (из записи «входим») и тоже выходит вслед за ними."""
        tr = self.trader(self.whales())
        self.on()
        real = tr.okx.get_order
        tr.okx.get_order = mock.Mock(side_effect=OSError("обрыв"))  # type: ignore[method-assign]
        with self.assertRaises(OSError):
            tr.step()
        tr = lv.Trader(lv.Okx("K", "S", "P", False, self.ex), lv.Signals(self.db), self.dir,
                       clock=lambda: self.now, log=self.logs.append)
        del real
        tr.recover()
        self.assertEqual(sorted(self.state()["open"]["wallets"]), ["0xa", "0xb"])
        self.now += 20 * MIN
        close_fill(self.db, 50, "0xa", "BTC", 3, self.now - MIN, -10.0)
        tr.last_sync = 0
        tr.step()
        self.assertIsNone(self.state()["open"])


class SignalMomentTest(Base):
    """Ревизия: момент сигнала — сделка кита, и на момент входа китов от 2."""

    def test_small_trade_does_not_refresh_old_whales(self) -> None:
        """Киты вошли час назад (сигнал давний), сейчас кто-то купил на 50 $ —
        это не свежий сигнал. Раньше бот входил по такой сделке."""
        fills = [*self.whales(ago=60), fill(9, "0xdust", "BTC", 1, self.now - MIN, 50, 100_000)]
        tr = self.trader(fills)
        self.on()
        tr.step()
        self.assertEqual(self.market_orders(), [])

    def test_whale_count_rechecked_at_decision(self) -> None:
        """Кит A вошёл 125 минут назад (прочитан прошлым опросом), кит B — 14
        минут назад: от строки B оба в окне, но на момент решения A старше 2
        часов — китов 1, не входим. Раньше бот входил."""
        tr = self.trader([fill(1, "0xa", "BTC", 1, self.now - 100 * MIN, 5000, 100_000)])
        self.on()
        tr.step()
        self.now += 25 * MIN
        con = sqlite3.connect(self.db)
        con.execute(INS, fill(2, "0xb", "BTC", 1, self.now - 14 * MIN, 5000, 100_000))
        con.commit()
        con.close()
        tr.last_sync = 0
        tr.step()
        self.assertEqual(self.market_orders(), [])
        self.assertEqual(self.state()["last_skip"]["code"], "stale")


class BiggerSideTest(Base):
    """Правило владельца «входим с крупными»: лонг BTC запустили мелкие киты
    (~42 тыс. $), а за те же 2 часа против них открыли шорты на 1,41 млн $ —
    бот вошёл в лонг. Теперь: против сигнала киты крупнее — в слабую сторону
    не входим; крупных от 2 — входим с ними."""

    def shorts(self, n: int = 2, usd: float = 700_000, ago: int = 40) -> list[tuple]:
        return [fill(100 + i, f"0xbig{i}", "BTC", -1, self.now - (ago + i) * MIN, usd, 100_000) for i in range(n)]

    def test_small_longs_against_big_shorts_bot_goes_short(self) -> None:
        tr = self.trader(self.shorts() + self.whales())
        self.on()
        tr.step()
        [m] = self.market_orders()
        self.assertEqual(m["side"], "sell", "вошли с крупными — в шорт")
        op = self.state()["open"]
        self.assertEqual((op["side"], sorted(op["wallets"])), (-1, ["0xbig0", "0xbig1"]))
        self.assertEqual((op["usd"], op["usd_against"]), (1_400_000.0, 10_000.0))

    def test_one_big_short_blocks_long_but_is_no_signal(self) -> None:
        tr = self.trader(self.shorts(n=1) + self.whales())
        self.on()
        tr.step()
        self.assertEqual(self.market_orders(), [])
        sk = self.state()["last_skip"]
        self.assertEqual(sk["code"], "bigger_against")
        self.assertIn("шорты 700 000 $ против 10 000 $", sk["why"])

    def test_smaller_against_does_not_block(self) -> None:
        tr = self.trader(self.shorts(n=2, usd=2000) + self.whales())
        self.on()
        tr.step()
        [m] = self.market_orders()
        self.assertEqual(m["side"], "buy")
        self.assertEqual(self.state()["open"]["usd_against"], 4000.0)

    def test_equal_sides_no_entry(self) -> None:
        """Поровну — непонятно, кто сильнее: не входим ни в одну сторону."""
        tr = self.trader(self.shorts(n=2, usd=5000) + self.whales())
        self.on()
        tr.step()
        self.assertEqual(self.market_orders(), [])

    def test_against_older_than_two_hours_ignored(self) -> None:
        """Против — за 2 часа до «сейчас»: шорты, прочитанные прошлым опросом,
        через 25 минут (им уже 125 минут) не в счёт, хотя от строки сигнала
        (10 минут назад) они ещё в пределах 2 часов."""
        tr = self.trader(self.shorts(ago=100))
        self.on()
        tr.step()                      # шорты в окне; их сигнал давний — не входим
        self.assertEqual(self.market_orders(), [])
        self.now += 25 * MIN
        con = sqlite3.connect(self.db)
        con.executemany(INS, self.whales(ago=10))
        con.commit()
        con.close()
        tr.last_sync = 0
        tr.step()
        [m] = self.market_orders()
        self.assertEqual(m["side"], "buy")
        os.remove(self.db)
        os.remove(os.path.join(self.dir, "live", "state.json"))
        self.ex = FakeOkx()
        tr = self.trader(self.shorts(ago=130) + self.whales())
        self.on()
        tr.step()
        [m] = self.market_orders()
        self.assertEqual(m["side"], "buy")

    def test_small_against_wallets_under_1000_do_not_count(self) -> None:
        """Против — только киты (кошелёк от 1000 $): мелочь против не мешает."""
        crumbs = [fill(200 + i, f"0xc{i}", "BTC", -1, self.now - 30 * MIN + i, 900, 100_000) for i in range(50)]
        tr = self.trader(crumbs + self.whales())
        self.on()
        tr.step()
        self.assertEqual(len(self.market_orders()), 1)
        self.assertEqual(self.state()["open"]["usd_against"], 0.0)


class PartialEntryTest(Base):
    """Рыночный вход исполнился частично (6 из 10), потом доисполнился:
    запись и тейк — на весь объём, а не на первую часть."""

    def partial(self, mode: str = "net_mode") -> lv.Trader:
        self.ex = FakeOkx(mode)
        tr = self.trader(self.whales())
        self.on()
        real = tr.okx.order

        def order(body):
            got = real(body)
            if body["clOrdId"].startswith("wtin"):
                self.ex.orders[-1].update(state="partially_filled", accFillSz="6")
                self.ex.pos[0]["pos"] = "6"
            return got

        tr.okx.order = order  # type: ignore[method-assign]
        tr.step()
        op = self.state()["open"]
        self.assertEqual((op["size"], op["in_done"]), (6.0, False))
        [tp] = [o for o in self.ex.orders if o["ordType"] == "limit"]
        self.assertEqual(tp["sz"], "6")
        return tr

    def fill_rest(self, px: float = 100_000.0) -> None:
        entry = next(o for o in self.ex.orders if str(o.get("clOrdId", "")).startswith("wtin"))
        entry.update(state="filled", accFillSz="10", avgPx=str(px))
        self.ex.pos[0]["pos"] = "10"

    def test_late_fill_moves_record_and_tp_to_full_size(self) -> None:
        for mode in ("net_mode", "long_short_mode"):
            tr = self.partial(mode)
            self.fill_rest(px=100_100.0)  # средняя цена всего входа — выше
            self.now += MIN
            tr.last_sync = 0
            tr.step()
            op = self.state()["open"]
            self.assertEqual((op["size"], op["in_done"], op["px"]), (10.0, True, 100_100.0), mode)
            live = [o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"]
            self.assertEqual([(o["sz"], o["px"]) for o in live], [("10", "101101")], "тейк — на весь объём от новой средней")
            self.assertTrue(any("вход доисполнился" in e for e in self.state()["events"]))
            for _ in range(2):   # дальше ничего не меняется и тейк не дублируется
                self.now += MIN
                tr.last_sync = 0
                tr.step()
            self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"]), 1)
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))

    def test_no_late_fill_keeps_partial_and_stops_checking(self) -> None:
        tr = self.partial()
        entry = next(o for o in self.ex.orders if str(o.get("clOrdId", "")).startswith("wtin"))
        entry.update(state="canceled")   # остаток рыночного отменён — 6 и есть весь вход
        self.now += MIN
        tr.last_sync = 0
        tr.step()
        op = self.state()["open"]
        self.assertEqual((op["size"], op["in_done"]), (6.0, True))
        self.assertEqual([o["sz"] for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"], ["6"])

    def test_tp_already_filling_is_not_touched(self) -> None:
        tr = self.partial()
        tp = next(o for o in self.ex.orders if o["ordType"] == "limit")
        tp.update(state="partially_filled", accFillSz="2")
        self.fill_rest()
        self.now += MIN
        tr.last_sync = 0
        tr.step()
        self.assertEqual(tp["state"], "partially_filled", "тейк не снят")
        self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit"]), 1, "и не задвоен")
        self.assertTrue(any("тейк уже исполняется" in e for e in self.state()["events"]))

    def test_never_more_than_ordered(self) -> None:
        tr = self.partial()
        entry = next(o for o in self.ex.orders if str(o.get("clOrdId", "")).startswith("wtin"))
        entry.update(state="filled", accFillSz="1000")   # порча ответа
        self.now += MIN
        tr.last_sync = 0
        tr.step()
        self.assertEqual(self.state()["open"]["size"], 10.0)


class AverageTest(Base):
    """Правило владельца: позиция в минусе больше 1 % (цена против входа) и
    кит сигнала добирает позицию после нашего входа — один раз доливаем 5 %
    капитала × 10× рыночным; средняя и тейк (+1 % от средней) — на весь объём."""

    def setup_long(self, mode: str = "net_mode", side: int = 1) -> lv.Trader:
        self.ex = FakeOkx(mode)
        tr = self.trader(self.whales(side=side))
        self.on()
        tr.step()
        self.assertEqual(tr.st["open"]["size"], 10.0)
        return tr

    def move(self, pct: float) -> None:
        px = 100_000 * (1 + pct)
        self.ex.last["BTC-USDT-SWAP"] = px
        self.ex.pos[0]["markPx"] = str(px)

    def whale_adds(self, wallet: str = "0xa", usd: float = 3000, side: int = 1, ago: int = 1, tid: int = 300) -> None:
        con = sqlite3.connect(self.db)
        con.execute(INS, fill(tid, wallet, "BTC", side, self.now - ago * MIN, usd, 100_000))
        con.commit()
        con.close()

    def run_sync(self, tr: lv.Trader, n: int = 1) -> None:
        for _ in range(n):
            self.now += MIN
            tr.last_sync = 0
            tr.step()

    def avg_orders(self) -> list[dict]:
        return [o for o in self.ex.orders if str(o.get("clOrdId", "")).startswith("wtav")]

    def test_loss_over_1pct_and_whale_adds_average_once(self) -> None:
        for mode, side in (("net_mode", 1), ("long_short_mode", -1)):
            tr = self.setup_long(mode, side)
            self.now += 20 * MIN
            self.move(-side * 0.011)               # цена против нас на 1,1 %
            self.whale_adds(side=side)              # кит сигнала добрал 3 000 $
            self.run_sync(tr)
            [av] = self.avg_orders()
            # 5 % × 20 000 $ × 10 = 10 000 $ / (0,01 × 98 900) = 10,111 → 10,11 (шаг 0,01);
            # шорт: цена 101 100 → 9,89.
            px2 = 100_000 * (1 - side * 0.011)
            add = int(10_000 / (0.01 * px2) * 100) / 100
            self.assertEqual((av["ordType"], av["side"], av["sz"]),
                             ("market", "buy" if side > 0 else "sell", f"{add:g}"), mode)
            op = self.state()["open"]
            self.assertAlmostEqual(op["size"], 10 + add)
            self.assertAlmostEqual(op["px"], (100_000 * 10 + px2 * add) / (10 + add))
            self.assertTrue(op["avg"]["done"])
            live = [o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"]
            self.assertEqual(len(live), 1, "тейк один")
            self.assertEqual(live[0]["sz"], f"{10 + add:g}", "тейк на весь объём")
            self.assertAlmostEqual(float(live[0]["px"]), op["px"] * (1 + side * lv.TP), delta=0.1)
            self.assertTrue(any("усредняюсь на 5 %" in e for e in self.state()["events"]))
            # Только один раз: минус глубже, кит добирает ещё — второй доливки нет.
            self.move(-side * 0.03)
            self.whale_adds(side=side, tid=301)
            self.run_sync(tr, 3)
            self.assertEqual(len(self.avg_orders()), 1)
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))

    def test_no_average_without_all_conditions(self) -> None:
        cases = [("минус меньше 1 %", -0.009, "0xa", 3000, 1),
                 ("кит добрал меньше 1000 $", -0.02, "0xa", 900, 1),
                 ("добрал не кит сигнала", -0.02, "0xother", 50_000, 1),
                 ("кит добрал до нашего входа", -0.02, "0xa", 3000, 30),
                 ("в плюсе", +0.02, "0xa", 3000, 1)]
        for name, pct, wallet, usd, ago in cases:
            self.ex = FakeOkx()
            tr = self.setup_long()
            self.now += 20 * MIN
            self.move(pct)
            self.whale_adds(wallet=wallet, usd=usd, ago=ago)
            self.run_sync(tr, 2)
            self.assertEqual(self.avg_orders(), [], name)
            self.assertNotIn("avg", self.state()["open"] or {}, name)
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))

    def test_stop_button_no_average(self) -> None:
        tr = self.setup_long()
        self.on(False)
        self.now += 20 * MIN
        self.move(-0.02)
        self.whale_adds()
        self.run_sync(tr)
        self.assertEqual(self.avg_orders(), [])

    def test_lost_reply_found_by_tag_no_second_order(self) -> None:
        tr = self.setup_long()
        real = tr.okx.order

        def order(body):
            got = real(body)
            if body["clOrdId"].startswith("wtav"):
                raise lv.OkxError("502", "HTTP")
            return got

        tr.okx.order = order  # type: ignore[method-assign]
        self.now += 20 * MIN
        self.move(-0.02)
        self.whale_adds()
        self.run_sync(tr, 3)
        self.assertEqual(len(self.avg_orders()), 1)
        op = self.state()["open"]
        self.assertEqual((op["size"], op["avg"]["done"]), (20.2, True))   # 10 000 $ / 980 = 10,2

    def test_refused_is_not_retried(self) -> None:
        tr = self.setup_long()
        real = tr.okx.order

        def order(body):
            if body["clOrdId"].startswith("wtav"):
                raise lv.OkxError("51008", "мало маржи")
            return real(body)

        tr.okx.order = order  # type: ignore[method-assign]
        self.now += 20 * MIN
        self.move(-0.02)
        self.whale_adds()
        self.run_sync(tr, 3)
        op = self.state()["open"]
        self.assertEqual((op["size"], op["avg"]["done"]), (10.0, True))
        self.assertIn("51008", op["avg"]["skipped"])
        self.assertTrue(any("усреднение не принято" in e for e in self.state()["events"]))

    def test_restart_mid_average_applies_once(self) -> None:
        """Служба упала сразу после ордера усреднения: после перезапуска ордер
        найден по метке, учтён один раз, второго нет."""
        tr = self.setup_long()
        real = tr.okx.order

        def order(body):
            got = real(body)
            if body["clOrdId"].startswith("wtav"):
                raise SystemExit("упала")
            return got

        tr.okx.order = order  # type: ignore[method-assign]
        self.now += 20 * MIN
        self.move(-0.02)
        self.whale_adds()
        tr.last_sync = 0
        with self.assertRaises(SystemExit):
            tr.step()
        tr = lv.Trader(lv.Okx("K", "S", "P", False, self.ex), lv.Signals(self.db), self.dir,
                       clock=lambda: self.now, log=self.logs.append)
        self.on()
        self.run_sync(tr, 2)
        self.assertEqual(len(self.avg_orders()), 1)
        self.assertEqual(self.state()["open"]["size"], 20.2)

    def avg_with(self, tr: lv.Trader, patch) -> None:
        real = tr.okx.order

        def order(body):
            got = real(body)
            if body["clOrdId"].startswith("wtav"):
                patch(self.ex.orders[-1])
            return got

        tr.okx.order = order  # type: ignore[method-assign]
        self.now += 20 * MIN
        self.move(-0.02)
        self.whale_adds()

    def test_partial_then_final_applied_once(self) -> None:
        tr = self.setup_long()

        def partial(o):
            o.update(state="partially_filled", accFillSz="5")
            self.ex.pos[0]["pos"] = "15"

        self.avg_with(tr, partial)
        self.run_sync(tr)
        self.assertEqual(self.state()["open"]["size"], 15.0)
        self.avg_orders()[0]["state"] = "canceled"   # остаток рыночного отменён
        self.run_sync(tr, 2)
        op = self.state()["open"]
        self.assertEqual((op["size"], op["avg"]["done"]), (15.0, True))
        self.assertEqual(sum("усреднено" in e for e in self.state()["events"]), 1, "учтено один раз")
        self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"]), 1)

    def test_garbage_fill_never_more_than_ordered(self) -> None:
        tr = self.setup_long()
        self.avg_with(tr, lambda o: o.update(accFillSz="1000"))
        self.run_sync(tr)
        self.assertEqual(self.state()["open"]["size"], 20.2)

    def test_tp_already_filling_not_touched_on_average(self) -> None:
        tr = self.setup_long()
        tp = next(o for o in self.ex.orders if o["ordType"] == "limit")
        tp.update(state="partially_filled", accFillSz="2")
        self.avg_with(tr, lambda o: None)
        self.run_sync(tr)
        self.assertEqual(tp["state"], "partially_filled", "тейк не снят")
        self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit"]), 1, "и не задвоен")
        self.assertTrue(any("тейк уже исполняется" in e for e in self.state()["events"]))

    def test_whale_loss_exit_closes_averaged_size(self) -> None:
        tr = self.setup_long()
        self.now += 20 * MIN
        self.move(-0.02)
        self.whale_adds()
        self.run_sync(tr)
        close_fill(self.db, 400, "0xb", "BTC", 3, self.now, -100.0)
        self.run_sync(tr)
        [ex] = [o for o in self.ex.orders if str(o.get("clOrdId", "")).startswith("wtex")]
        self.assertEqual(ex["sz"], "20.2", "выход — весь усреднённый объём")
        self.assertIsNone(self.state()["open"])


class EeaTest(Base):
    def test_european_account_trades_xperp_not_usdt_swap(self) -> None:
        """OKX EEA: бессрочные — FUTURES «BTC-USD_UM_XPERP-310815», а
        BTC-USDT-SWAP в каталоге есть, но торговать им нельзя (50124). Бот
        берёт XPERP; размер контракта может быть в монете или в долларах."""
        self.ex.reject["BTC-USDT-SWAP"] = "50124"
        for ccy, ct_val in (("BTC", "0.0001"), ("USD", "100")):
            self.ex = FakeOkx("long_short_mode")
            self.ex.reject["BTC-USDT-SWAP"] = "50124"
            x = "BTC-USD_UM_XPERP-310815"
            self.ex.instr[x] = {"instId": x, "instType": "FUTURES", "state": "live", "ctVal": ct_val, "ctMult": "1",
                                "ctValCcy": ccy, "lotSz": "1", "minSz": "1", "tickSz": "0.1"}
            self.ex.instr["ETH-USD_UM_XPERP-310815"] = {**self.ex.instr[x], "instId": "ETH-USD_UM_XPERP-310815"}
            self.ex.last[x] = 100_000.0
            tr = self.trader(self.whales())
            self.on()
            tr.step()
            [m] = self.market_orders()
            # 10 000 $ позиции: контракт 0,0001 BTC × 100 000 = 10 $ → 1000; контракт 100 $ → 100.
            ct_usd = float(ct_val) * (1 if ccy == "USD" else 100_000)
            self.assertEqual((m["instId"], m["sz"]), (x, str(int(10_000 / ct_usd))), ccy)
            self.assertAlmostEqual(tr.st["open"]["notional"], 10_000, delta=1)
            [tp] = [o for o in self.ex.orders if o["ordType"] == "limit"]
            self.assertEqual((tp["instId"], tp["px"], tp["posSide"]), (x, "101000", "long"))
            self.assertFalse(any(c[2].get("instId") == "BTC-USDT-SWAP" for c in self.ex.calls if c[1] == "/api/v5/trade/order"))
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))

    def test_xperp_tp_is_seen_not_duplicated_and_close_recorded(self) -> None:
        """Европейский контракт (FUTURES): бот видит свой тейк и не ставит его
        заново каждые 15 секунд; после закрытия берёт PnL из истории FUTURES."""
        x = "BTC-USD_UM_XPERP-310815"
        self.ex = FakeOkx("long_short_mode")
        self.ex.instr[x] = {"instId": x, "instType": "FUTURES", "state": "live", "ctVal": "0.0001", "ctMult": "1",
                            "ctValCcy": "BTC", "lotSz": "1", "minSz": "1", "tickSz": "0.1"}
        self.ex.last[x] = 100_000.0
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        for _ in range(3):
            self.now += 2 * MIN
            tr.last_sync = 0
            tr.step()
        self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit"]), 1, "тейк один, не дубли")
        self.now += 10 * MIN
        self.ex.pos = []
        self.ex.history = [{"instId": x, "uTime": str(self.now), "realizedPnl": "77.0", "closeAvgPx": "101000"}]
        tr.last_sync = 0
        tr.step()
        with open(os.path.join(self.dir, "live", "trades.jsonl"), encoding="utf-8") as f:
            [t] = [json.loads(v) for v in f]
        self.assertEqual((t["inst"], t["pnl"], t["why"]), (x, 77.0, "тейк"))
        self.assertEqual(lv.inst_type(x), "FUTURES")
        self.assertEqual(lv.inst_type("BTC-USDT-SWAP"), "SWAP")

    def test_global_account_without_xperp_keeps_usdt_swap(self) -> None:
        tr = self.trader(self.whales())
        self.assertEqual(tr.inst_for("BTC"), "BTC-USDT-SWAP")


class RejectTest(Base):
    def test_refused_instrument_is_excluded_and_next_signal_trades(self) -> None:
        """OKX не даёт торговать самой бумагой (51155 — ограничение по стране):
        позиции нет, бумага исключена на неделю, бот не долбит её ордерами и
        входит по следующему сигналу в другую бумагу."""
        self.ex.reject["STX-USDT-SWAP"] = "51155"
        fills = [fill(1, "0xa", "STX", 1, self.now - 4 * MIN, 5000, 0.5),
                 fill(2, "0xb", "STX", 1, self.now - 3 * MIN, 5000, 0.5),
                 fill(3, "0xa", "BTC", 1, self.now - 2 * MIN, 5000, 100_000),
                 fill(4, "0xb", "BTC", 1, self.now - 1 * MIN, 5000, 100_000)]
        # BTC — раньше в списке «свежих первыми» нельзя: сначала пробуем STX.
        fills = [fills[2], fills[3], fills[0], fills[1]]
        fills = [(i + 1, *f[1:]) for i, f in enumerate(fills)]
        fills[2] = (3, "0xa", "STX", 1, self.now - MIN // 2, int(5000e9), str(5000 / 0.5))
        fills[3] = (4, "0xb", "STX", 1, self.now - MIN // 3, int(5000e9), str(5000 / 0.5))
        tr = self.trader(fills)
        self.on()
        tr.step()
        st = self.state()
        self.assertTrue(st["blocked"]["STX-USDT-SWAP"]["permanent"])
        self.assertNotIn("entering", st)
        self.assertEqual(st["open"]["inst"], "BTC-USDT-SWAP", "после отказа — следующий сигнал")
        tries = [c for c in self.ex.calls if c[1] == "/api/v5/trade/order" and c[2].get("instId") == "STX-USDT-SWAP"]
        self.assertEqual(len(tries), 1)
        self.assertTrue(any("исключена на 7 дней" in e for e in st["events"]))
        # Позицию закрыли — новые киты по STX: не пробуем (неделя), причина — в пропуске.
        self.ex.pos = []
        tr.st["open"] = None
        tr.positions = []
        self.assertIn("OKX не даёт торговать", tr.try_enter({"coin": "STX", "side": 1, "ts": self.now,
                                                             "whales": 2, "ref_px": 0.5}))
        self.assertEqual(len([c for c in self.ex.calls if c[1] == "/api/v5/trade/order"
                              and c[2].get("instId") == "STX-USDT-SWAP"]), 1)

    def test_refused_instrument_stays_excluded_for_a_week(self) -> None:
        self.ex.reject["BTC-USDT-SWAP"] = "51001"
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        del self.ex.reject["BTC-USDT-SWAP"]
        sig = {"coin": "BTC", "side": 1, "ts": self.now, "whales": 2, "ref_px": 100_000}
        tr.sig.windows.clear()  # сигналы ниже — новые (без окна прошлого опроса)
        self.now += 86_400_000  # сутки — ещё исключена
        self.assertIn("OKX не даёт торговать", tr.try_enter({**sig, "ts": self.now}))
        self.now += 7 * 86_400_000  # неделя прошла — снова пробуем
        self.assertEqual(tr.try_enter({**sig, "ts": self.now}), "")

    def test_key_closed_market_pauses_market_not_instruments(self) -> None:
        """Как было на сервере владельца: OKX отклоняет каждый ордер 50124
        («ключу не разрешена торговля на рынке»). Это про ключ, не про бумагу:
        ни одна бумага не исключается; рынок — пауза 30 минут (одна проверка
        ордером, а не по ордеру на каждый сигнал); ключ исправили — следующий
        сигнал входит, в том числе по BTC."""
        self.ex.reject["BTC-USDT-SWAP"] = self.ex.reject["STX-USDT-SWAP"] = "50124"
        stx = [fill(10, "0xc", "STX", 1, self.now - 3 * MIN, 5000, 0.5), fill(11, "0xd", "STX", 1, self.now - 2 * MIN, 5000, 0.5)]
        tr = self.trader(self.whales() + stx)
        self.on()
        tr.step()
        st = self.state()
        tries = [c for c in self.ex.calls if c[1] == "/api/v5/trade/order"]
        self.assertEqual(len(tries), 1, "второй сигнал — без ордера: рынок на паузе")
        self.assertEqual(st["blocked"], {}, "бумаги не исключены")
        self.assertIn("SWAP", st["market_block"])
        self.assertIn("ключу OKX закрыт рынок", st["last_skip"]["why"])
        self.assertEqual(st["last_skip"]["code"], "market")
        self.assertTrue(any("Бумаги не исключаются" in e for e in st["events"]))
        self.assertIn("50124", st["key_problem"]["why"])
        # Ключ исправили; через 31 минуту — снова пробуем, и BTC входит.
        del self.ex.reject["BTC-USDT-SWAP"]
        self.now += 31 * MIN
        sig = {"coin": "BTC", "side": 1, "ts": self.now, "whales": 2, "ref_px": 100_000}
        self.assertEqual(tr.try_enter(sig), "")
        self.assertEqual(tr.st["open"]["inst"], "BTC-USDT-SWAP")
        self.assertNotIn("key_problem", tr.st, "ордер прошёл — проблемы ключа нет")

    def test_closed_swap_market_does_not_stop_xperp(self) -> None:
        """Закрыт один рынок (USDT-SWAP на европейском счёте) — европейские
        бессрочные (FUTURES) торгуются дальше."""
        x = "BTC-USD_UM_XPERP-310815"
        self.ex.instr[x] = {"instId": x, "instType": "FUTURES", "state": "live", "ctVal": "0.0001", "ctMult": "1",
                            "ctValCcy": "BTC", "lotSz": "1", "minSz": "1", "tickSz": "0.1"}
        self.ex.last[x] = 100_000.0
        self.ex.reject["STX-USDT-SWAP"] = "50124"
        stx = [fill(10, "0xc", "STX", 1, self.now - 3 * MIN, 5000, 0.5), fill(11, "0xd", "STX", 1, self.now - MIN // 2, 5000, 0.5)]
        tr = self.trader(self.whales() + stx)
        self.on()
        tr.step()
        self.assertEqual(self.state()["open"]["inst"], x)
        self.assertIn("SWAP", self.state()["market_block"])

    def test_old_week_exclusions_for_50124_are_lifted(self) -> None:
        """Прежняя версия исключила BTC, ETH и др. на неделю за 50124 — при
        старте новой эти исключения снимаются; исключения по бумаге остаются."""
        os.makedirs(os.path.join(self.dir, "live"))
        with open(os.path.join(self.dir, "live", "state.json"), "w", encoding="utf-8") as f:
            json.dump({"blocked": {
                "BTC-USD_UM_XPERP-310404": {"at": self.now, "why": "OKX 50124: This API Key does not have trading permission",
                                            "permanent": True},
                "STX-USDT-SWAP": {"at": self.now, "why": "OKX 51155: compliance", "permanent": True}}}, f)
        tr = self.trader(self.whales())
        self.assertEqual(list(tr.st["blocked"]), ["STX-USDT-SWAP"])

    def test_skip_counter_by_reason_per_hour_for_a_day(self) -> None:
        """Счётчик пропусков: по причинам, один сигнал с одной причиной за
        час — один раз; старше суток — не считается."""
        self.ex.last["BTC-USDT-SWAP"] = 100_500  # цена ушла дальше 0,3 %
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        con = sqlite3.connect(self.db)
        con.executemany(INS,
                        [fill(20 + i, f"0x{i}", "BTC", 1, self.now + i * 1000, 5000, 100_000) for i in range(3)])
        con.commit()
        con.close()
        self.now += 5000
        tr.step()   # новые сделки китов по той же бумаге — тот же сигнал, счёт не растёт
        total = lambda: sum(sum(v.values()) for v in self.state()["skips"].values())  # noqa: E731
        self.assertEqual(total(), 1)
        self.assertEqual(next(iter(self.state()["skips"].values())), {"chase": 1})
        tr.count_skip({"coin": "ETH", "side": -1}, "no_inst")
        tr.save()
        self.assertEqual(total(), 2)
        self.now += 25 * 3_600_000
        tr.count_skip({"coin": "SOL", "side": 1}, "no_inst")
        tr.save()
        self.assertEqual(list(self.state()["skips"].values()), [{"no_inst": 1}], "старше суток — убрано")

    def test_other_order_error_pauses_coin_15_min(self) -> None:
        self.ex.reject["BTC-USDT-SWAP"] = "51008"  # мало маржи — не про бумагу
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        b = self.state()["blocked"]["BTC-USDT-SWAP"]
        self.assertFalse(b["permanent"])
        self.assertIn("маржи", b["why"])
        self.now += 16 * MIN
        del self.ex.reject["BTC-USDT-SWAP"]
        self.assertEqual(tr.try_enter({"coin": "BTC", "side": 1, "ts": self.now, "whales": 2, "ref_px": 100_000}), "")


class RecoverTest(Base):
    """Служба упала между рыночным ордером и записью сделки: позиция есть,
    бот её «не знает». После перезапуска — узнаёт свою по метке ордера и
    ставит тейк; ручную позицию владельца своей не считает."""

    def crash_after_order(self) -> lv.Trader:
        tr = self.trader(self.whales())
        self.on()
        real_order = tr.okx.order

        def order_then_crash(body):
            real_order(body)
            if body["ordType"] == "market":
                raise SystemExit("упала")
            return ""

        tr.okx.order = order_then_crash  # type: ignore[method-assign]
        with self.assertRaises(SystemExit):
            tr.step()
        return tr

    def test_restart_adopts_own_position_and_places_tp(self) -> None:
        self.crash_after_order()
        self.assertEqual(len(self.ex.pos), 1)
        self.assertEqual([o for o in self.ex.orders if o["ordType"] == "limit"], [], "тейка нет")
        st = self.state()
        self.assertIsNone(st["open"])
        self.assertEqual(st["entering"]["inst"], "BTC-USDT-SWAP")
        # Перезапуск службы: новый трейдер с тем же каталогом.
        tr = lv.Trader(lv.Okx("K", "S", "P", False, self.ex), lv.Signals(self.db), self.dir,
                       clock=lambda: self.now, log=self.logs.append)
        tr.recover()
        st = self.state()
        self.assertEqual((st["open"]["inst"], st["open"]["tp"], st["open"]["whales"]), ("BTC-USDT-SWAP", 101000.0, 2))
        self.assertNotIn("entering", st)
        [tp] = [o for o in self.ex.orders if o["ordType"] == "limit"]
        self.assertEqual((tp["side"], tp["px"]), ("sell", "101000"))
        tr.last_sync = 0
        tr.step()
        self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit"]), 1, "тейк не дублируется")
        self.assertEqual(len(self.market_orders()), 1, "повторного входа нет")

    def test_lost_record_found_by_order_tag(self) -> None:
        self.crash_after_order()
        os.remove(os.path.join(self.dir, "live", "state.json"))  # и запись «входим» потерялась
        tr = lv.Trader(lv.Okx("K", "S", "P", False, self.ex), lv.Signals(self.db), self.dir,
                       clock=lambda: self.now, log=self.logs.append)
        tr.recover()
        tr.step()
        self.assertEqual(self.state()["open"]["inst"], "BTC-USDT-SWAP")
        self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit"]), 1)

    def test_our_order_in_other_direction_is_not_adoption(self) -> None:
        """На счёте — лонг владельца; в истории — наш прошлый вход в шорт по той
        же бумаге (давно закрыт): чужую позицию своей не считать."""
        self.ex.pos = [{"instId": "BTC-USDT-SWAP", "pos": "3", "posSide": "net", "avgPx": "100000", "markPx": "100000"}]
        self.ex.orders.append({"instId": "BTC-USDT-SWAP", "ordId": "77", "clOrdId": "wtin1", "side": "sell",
                               "ordType": "market", "state": "filled", "cTime": str(T0)})
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        self.assertIsNone(self.state()["open"])
        self.assertEqual([o for o in self.ex.orders if o["ordType"] == "limit"], [])

    def test_manual_position_is_not_adopted(self) -> None:
        self.ex.pos = [{"instId": "ETH-USDT-SWAP", "pos": "-3", "posSide": "net", "avgPx": "3000"}]
        self.ex.orders.append({"instId": "ETH-USDT-SWAP", "ordId": "99", "clOrdId": "", "side": "sell",
                               "ordType": "market", "state": "filled", "cTime": str(T0)})  # руками — без нашей метки
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        # Ручная — не своя: не принята, тейк на неё не ставится; бот входит
        # своей сделкой (5 %), и тейк — только на неё.
        self.assertEqual(self.state()["open"]["inst"], "BTC-USDT-SWAP")
        self.assertEqual([o["instId"] for o in self.ex.orders if o["ordType"] == "limit"], ["BTC-USDT-SWAP"])
        self.assertFalse(any(o.get("instId") == "ETH-USDT-SWAP" and o.get("clOrdId", "").startswith("wt")
                             for o in self.ex.orders))


class ReviewTest(Base):
    """Ошибки, найденные построчной ревизией, — каждая закрыта проверкой."""

    def add_fills(self, rows: list[tuple]) -> None:
        con = sqlite3.connect(self.db)
        con.executemany(INS, rows)
        con.commit()
        con.close()

    def stx(self, tid: int = 10, ago: int = 2) -> list[tuple]:
        return [fill(tid, "0xc", "STX", 1, self.now - ago * MIN - MIN, 5000, 0.5),
                fill(tid + 1, "0xd", "STX", 1, self.now - ago * MIN, 5000, 0.5)]

    def test_network_failure_after_market_order_never_sends_second(self) -> None:
        """Рыночный ордер ушёл, а проверка исполнения упала обрывом сети: на
        следующем шаге новый сигнал по другой бумаге — второго рыночного
        ордера нет; своя позиция принята по метке, тейк поставлен."""
        tr = self.trader(self.whales())
        self.on()
        real = tr.okx.get_order
        tr.okx.get_order = mock.Mock(side_effect=OSError("обрыв"))  # type: ignore[method-assign]
        with self.assertRaises(OSError):
            tr.step()
        self.assertEqual(len(self.market_orders()), 1)
        self.assertEqual(self.state()["entering"]["inst"], "BTC-USDT-SWAP")
        tr.okx.get_order = real  # type: ignore[method-assign]
        # Биржа ещё не показывает позицию в списке — а сигнал по другой бумаге уже есть.
        shown, self.ex.pos = self.ex.pos, []
        self.now += MIN
        self.add_fills(self.stx())
        tr.last_sync = 0
        tr.step()
        self.assertEqual(len(self.market_orders()), 1, "второго рыночного ордера нет")
        self.ex.pos = shown
        tr.last_sync = 0
        tr.step()
        st = self.state()
        self.assertEqual(st["open"]["inst"], "BTC-USDT-SWAP")
        self.assertNotIn("entering", st)
        self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit"]), 1)

    def test_gateway_error_on_order_is_not_a_refusal(self) -> None:
        """Шлюз ответил 502 (не JSON), а ордер на бирже исполнился: это не
        отказ — бумага не исключается, следующий сигнал в том же шаге не
        входит, позиция принимается сверкой."""
        tr = self.trader(self.whales() + self.stx())
        self.on()
        real = tr.okx.order

        def order(body):
            got = real(body)
            if body["ordType"] == "market":
                raise lv.OkxError("502", "HTTP")
            return got

        tr.okx.order = order  # type: ignore[method-assign]
        tr.step()
        self.assertEqual(len(self.market_orders()), 1, "второй сигнал не вошёл")
        st = self.state()
        self.assertIn("entering", st)
        self.assertEqual(st.get("blocked", {}), {}, "бумага не исключена")
        self.assertIn("не подтверждён", st["last_skip"]["why"])
        tr.okx.order = real  # type: ignore[method-assign]
        tr.last_sync = 0
        tr.step()
        self.assertIn(self.state()["open"]["inst"], ("BTC-USDT-SWAP", "STX-USDT-SWAP"))
        self.assertEqual(len(self.market_orders()), 1)

    def test_canceled_market_order_is_not_a_position(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        real = tr.okx.order

        def order(body):
            got = real(body)
            if body["ordType"] == "market":
                self.ex.orders[-1].update(state="canceled", accFillSz="0")
                self.ex.pos.clear()
            return got

        tr.okx.order = order  # type: ignore[method-assign]
        tr.step()
        st = self.state()
        self.assertIsNone(st["open"])
        self.assertNotIn("entering", st)
        self.assertFalse(st["blocked"]["BTC-USDT-SWAP"]["permanent"])
        self.assertEqual([o for o in self.ex.orders if o["ordType"] == "limit"], [], "тейка без позиции нет")

    def test_unconfirmed_fill_is_not_recorded_as_position(self) -> None:
        """Биржа приняла рыночный ордер, но исполнение не подтверждает (state
        live, исполнено 0): позицию не записывать — ждать сверки."""
        tr = self.trader(self.whales())
        self.on()
        real = tr.okx.order

        def order(body):
            got = real(body)
            if body["ordType"] == "market":
                self.ex.orders[-1].update(state="live", accFillSz="0")
                self.ex.pos.clear()
            return got

        tr.okx.order = order  # type: ignore[method-assign]
        tr.step()
        st = self.state()
        self.assertIsNone(st["open"])
        self.assertEqual(st["entering"]["inst"], "BTC-USDT-SWAP")
        self.assertEqual([o for o in self.ex.orders if o["ordType"] == "limit"], [])

    def test_unconfirmed_entry_expires_after_10_minutes(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        tr.st["entering"] = {"inst": "BTC-USDT-SWAP", "cl": "wtin404", "at": self.now, "coin": "BTC", "side": 1}
        tr.sync()
        self.assertIn("entering", tr.st, "ещё ждём")
        self.now += 11 * MIN
        tr.sync()
        self.assertNotIn("entering", tr.st)

    def test_tp_filled_while_owner_added_to_position(self) -> None:
        """Режим net: бот в лонге 10, владелец докупил 15 руками; тейк бота
        исполнился — позиция 15 (ручная). Сделка записана, нового тейка на
        ручную часть нет."""
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        tp = next(o for o in self.ex.orders if o["ordType"] == "limit")
        tp.update(state="filled", avgPx="101000", accFillSz="10", pnl="100", fee="-5", fillTime=str(self.now + 5 * MIN))
        self.ex.pos[0]["pos"] = "15"
        self.now += 10 * MIN
        tr.last_sync = 0
        tr.step()
        with open(os.path.join(self.dir, "live", "trades.jsonl"), encoding="utf-8") as f:
            [t] = [json.loads(x) for x in f]
        self.assertEqual((t["why"], t["pnl"], t["px_out"]), ("тейк", 100.0, 101000.0))
        self.assertIsNone(self.state()["open"])
        self.assertEqual([o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"], [])

    def test_long_short_owner_opposite_position_is_not_bots(self) -> None:
        """Режим long/short: бот в лонге BTC, владелец открыл шорт BTC руками.
        Лонг бота закрыт (тейком или руками, тейк снят) — бот видит закрытие,
        а не принимает шорт владельца за свой и не ставит на него тейк."""
        for tp_state, why in (("filled", "тейк"), ("canceled", "закрыта")):
            self.ex = FakeOkx("long_short_mode")
            tr = self.trader(self.whales())
            self.on()
            tr.step()
            next(o for o in self.ex.orders if o["ordType"] == "limit")["state"] = tp_state
            self.ex.pos = [{"instId": "BTC-USDT-SWAP", "pos": "4", "posSide": "short", "avgPx": "100500",
                            "markPx": "100500"}]
            self.now += 10 * MIN
            px = "101000" if tp_state == "filled" else "100200"
            self.ex.history = [{"instId": "BTC-USDT-SWAP", "uTime": str(self.now), "realizedPnl": "9", "closeAvgPx": px}]
            tr.last_sync = 0
            tr.step()
            self.assertIsNone(self.state()["open"], tp_state)
            self.assertEqual([o for o in self.ex.orders if o["ordType"] == "limit" and o["state"] == "live"], [], tp_state)
            with open(os.path.join(self.dir, "live", "trades.jsonl"), encoding="utf-8") as f:
                self.assertEqual(json.loads(f.readlines()[-1])["why"], why)
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))

    def test_cancel_failure_after_close_records_trade_once(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        self.ex.pos = []
        self.now += 10 * MIN
        self.ex.history = [{"instId": "BTC-USDT-SWAP", "uTime": str(self.now), "realizedPnl": "-3", "closeAvgPx": "99990"}]
        tr.okx.cancel = mock.Mock(side_effect=lv.OkxError("50001", "сервис занят"))  # type: ignore[method-assign]
        for _ in range(3):
            tr.last_sync = 0
            tr.step()
        with open(os.path.join(self.dir, "live", "trades.jsonl"), encoding="utf-8") as f:
            self.assertEqual(len(f.readlines()), 1, "закрытие записано один раз")
        self.assertTrue(any("остаток тейка не снят" in x for x in self.logs))

    def test_history_failure_blocks_entries_until_checked(self) -> None:
        """Позиция на счёте, история ордеров недоступна: своя она или ручная —
        неизвестно; новых входов нет, пока не проверено."""
        self.ex.pos = [{"instId": "ETH-USDT-SWAP", "pos": "-3", "posSide": "net", "avgPx": "3000"}]
        tr = self.trader(self.whales())
        self.on()
        real = tr.okx.history
        tr.okx.history = mock.Mock(side_effect=lv.OkxError("50013", "занято"))  # type: ignore[method-assign]
        tr.step()
        self.assertEqual(self.market_orders(), [])
        self.assertIn("не удалось проверить", self.state()["last_skip"]["why"])
        tr.okx.history = real  # type: ignore[method-assign]
        self.now += MIN
        self.add_fills(self.stx(20))
        tr.last_sync = 0
        tr.step()
        self.assertEqual(len(self.market_orders()), 1)
        self.assertEqual(self.state()["open"]["margin_pct"], 5.0)

    def test_late_whale_fill_is_counted(self) -> None:
        """Сделка кита пришла в базу позже следующей (бот записал её с
        задержкой): она всё равно считается в окне 2 часов."""
        tr = self.trader([fill(1, "0xa", "BTC", 1, self.now - 2 * MIN, 5000, 100_000)])
        self.on()
        tr.step()
        self.assertEqual(self.market_orders(), [])
        # Время — на 12 минут раньше уже прочитанной: опоздала дольше любого «нахлёста».
        self.add_fills([fill(2, "0xb", "BTC", 1, self.now - 14 * MIN, 5000, 100_000)])
        self.now += MIN
        tr.last_sync = 0
        tr.step()
        self.assertEqual(len(self.market_orders()), 1)

    def test_window_keeps_time_order_for_late_items(self) -> None:
        import autotrade_core as core
        w = core.Window(1000 * 10**9)
        w.add(T0, "0xa", 5000 * 10**9, 0.05)
        w.add(T0 - 90 * MIN, "0xb", 5000 * 10**9, 0.05)  # опоздавшая — старше
        self.assertEqual(w.count, 2)
        w.expire(T0 - 60 * MIN)                          # уходит именно она, а не новая
        self.assertEqual(w.count, 1)
        w.expire(T0 + MIN)
        self.assertEqual(w.count, 0)

    def test_missing_db_is_an_error_not_exit(self) -> None:
        with self.assertRaises(OSError):
            lv.Signals(os.path.join(self.tmp.name, "нет.db")).poll(self.now)

    def test_corrupted_state_file_does_not_stop_trading(self) -> None:
        """state.json испорчен (правка руками, обрыв): служба не падает на
        каждом шаге; испорченная запись позиции забыта — своя позиция
        находится по метке ордера."""
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        good = self.state()
        for bad in ({"entered": [], "events": "x", "blocked": 5, "open": {"inst": 1}},
                    {"open": {**good["open"], "side": 0}}, {"open": "мусор", "entering": "x", "error": None}, []):
            with open(os.path.join(self.dir, "live", "state.json"), "w", encoding="utf-8") as f:
                json.dump(bad, f)
            tr = lv.Trader(lv.Okx("K", "S", "P", False, self.ex), lv.Signals(self.db), self.dir,
                           clock=lambda: self.now, log=self.logs.append)
            tr.last_sync = 0
            tr.step()
            self.assertEqual(self.state()["open"]["inst"], "BTC-USDT-SWAP", str(bad))
        self.assertEqual(len(self.market_orders()), 1)

    def test_janitor_survives_garbage_state_and_drops_expired_blocks(self) -> None:
        tr = self.trader(self.whales())
        now_ms = self.now
        tr.st["entered"] = {"BTC": "мусор", "ETH": now_ms}
        tr.st["blocked"] = {"A": {"at": now_ms - 20 * MIN, "permanent": False},
                            "B": {"at": now_ms - 20 * MIN, "permanent": True},
                            "C": {"at": now_ms - 8 * 86_400_000, "permanent": True}, "D": "мусор"}
        lv.Janitor(self.dir, rss=lambda: 10.0).run(tr, None, now_ms / 1000)
        self.assertEqual(tr.st["entered"], {"ETH": now_ms})
        self.assertEqual(sorted(tr.st["blocked"]), ["B"])


class JanitorTest(Base):
    def test_cleans_windows_lists_logs_cache_and_asks_restart_when_fat(self) -> None:
        tr = self.trader(self.whales())
        tr.sig.poll(self.now)
        self.assertTrue(tr.sig.windows)
        tr.st["entered"] = {"OLD": self.now - 4 * 3_600_000, "NEW": self.now - 3_600_000,
                            "FRESH": self.now + 2 * 3_600_000}  # за час до очистки — остаётся
        live_log = os.path.join(tr.dir, "live.log")
        with open(live_log, "wb") as f:
            f.write(b"x" * (lv.LOG_MAX_BYTES + 1))
        cache = os.path.join(self.dir, "cache")
        os.makedirs(cache)
        now = self.now / 1000 + 3 * 3600  # через 3 часа — окна старше 2 часов пусты
        for name, age_days, size in (("old.json", 40, 10), ("a.json", 3, 300 << 20), ("b.json", 1, 300 << 20)):
            path = os.path.join(cache, name)
            with open(path, "wb") as f:
                f.truncate(size)
            os.utime(path, (now - age_days * 86400, now - age_days * 86400))
        jan = lv.Janitor(self.dir, rss=lambda: 120.0)
        self.assertTrue(jan.due(now))
        self.assertFalse(jan.run(tr, None, now))
        self.assertFalse(jan.due(now + 1))
        self.assertEqual(tr.sig.windows, {})
        self.assertEqual(list(tr.st["entered"]), ["FRESH"])  # пауза 3 часа прошла — убраны; свежий вход — на месте
        self.assertTrue(os.path.exists(live_log + ".1"))
        self.assertFalse(os.path.exists(live_log))
        self.assertEqual(sorted(os.listdir(cache)), ["b.json"])  # старше 30 дней и сверх 500 МБ — убраны, старые первыми
        self.assertEqual(tr.st["janitor"]["rss_mb"], 120.0)
        # Разрослась — просит перезапуск; но не посреди пересчёта и не посреди входа.
        fat = lv.Janitor(self.dir, rss=lambda: lv.RSS_MAX_MB + 1)
        self.assertTrue(fat.run(tr, None, now))

        class Busy:
            proc = object()

        os.utime(os.path.join(cache, "b.json"), (now - 40 * 86400, now - 40 * 86400))
        self.assertFalse(fat.run(tr, Busy(), now))
        self.assertTrue(os.path.exists(os.path.join(cache, "b.json")), "кеш во время пересчёта не трогаем")
        tr.st["entering"] = {"inst": "X"}
        self.assertFalse(fat.run(tr, None, now))

    def test_old_cache_removed_even_when_small(self) -> None:
        cache = os.path.join(self.dir, "cache")
        os.makedirs(cache)
        now = self.now / 1000
        for name, age in (("old.json", 31), ("new.json", 29)):
            path = os.path.join(cache, name)
            with open(path, "w") as f:
                f.write("{}")
            os.utime(path, (now - age * 86400, now - age * 86400))
        self.assertEqual(lv.Janitor.clean_cache(cache, now), 2)
        self.assertEqual(os.listdir(cache), ["new.json"])

    def test_rss_is_measured(self) -> None:
        self.assertGreater(lv.rss_mb(), 1)


FAKE_BT = """
import json, os, sys, time
out = sys.argv[sys.argv.index("--out") + 1]
mode = open(os.path.join(os.path.dirname(out), "mode")).read().strip()
if mode == "sleep":
    time.sleep(60)
if mode == "progress":
    os.makedirs(out, exist_ok=True)
    json.dump({"stage": "повтор", "done": 250, "total": 1000, "trades": 3, "requests": 17},
              open(os.path.join(out, "progress.json"), "w"))
    time.sleep(60)
code = int(mode)
if code == 0:
    os.makedirs(out, exist_ok=True)
    json.dump({"verdict": True, "args": sys.argv[1:]}, open(os.path.join(out, "summary.json"), "w"))
sys.exit(code)
"""


class BacktestServiceTest(unittest.TestCase):
    """Бэктест — в службе торговли, не в API: по запросу и ночью, пока
    модуль включён; итог — latest/prev; «Стоп» прерывает; предел памяти
    службы меньше нужного — не запускается и говорит, что делать."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = self.tmp.name
        self.script = os.path.join(self.dir, "fake_bt.py")
        with open(self.script, "w") as f:
            f.write(FAKE_BT)

    def bts(self, mode: str, limit: float = 4096) -> lv.Backtests:
        with open(os.path.join(self.dir, "mode"), "w") as f:
            f.write(mode)
        b = lv.Backtests(self.dir, "/db/hyperliquid.db", self.script, limit_mb=lambda: limit)
        b.st["next"] = 2**40  # ночь далеко — только по запросу
        return b

    def request(self) -> None:
        with open(os.path.join(self.dir, "run.request"), "w") as f:
            f.write("{}")

    def status(self) -> dict:
        with open(os.path.join(self.dir, "backtest_status.json")) as f:
            return json.load(f)

    def finish(self, b: lv.Backtests) -> None:
        b.proc.wait(timeout=30)
        b.tick(True)

    def test_request_runs_and_success_becomes_latest(self) -> None:
        b = self.bts("0")
        b.tick(True)
        self.assertIsNone(b.proc, "без запроса и не ночью — не запускается")
        self.request()
        b.tick(True)
        self.assertIsNotNone(b.proc)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "run.request")))
        self.assertTrue(self.status()["running"])
        self.finish(b)
        with open(os.path.join(self.dir, "latest", "summary.json")) as f:
            args = json.load(f)["args"]
        self.assertEqual(args[args.index("--pause") + 1], str(lv.BT_PAUSE_SEC))
        self.assertEqual(args[args.index("--max-mem-mb") + 1], str(lv.BT_MEM_MB))
        self.assertEqual((self.status()["running"], self.status()["error"]), (False, ""))
        # Следующий — с ошибкой: прежний итог на месте; ещё удачный — прежний в prev.
        with open(os.path.join(self.dir, "mode"), "w") as f:
            f.write("2")
        self.request()
        b.tick(True)
        self.finish(b)
        self.assertIn("кодом 2", self.status()["error"])
        self.assertTrue(os.path.isfile(os.path.join(self.dir, "latest", "summary.json")))
        with open(os.path.join(self.dir, "mode"), "w") as f:
            f.write("3")
        self.request()
        b.tick(True)
        self.finish(b)
        self.assertIn("предела памяти", self.status()["error"])
        with open(os.path.join(self.dir, "mode"), "w") as f:
            f.write("0")
        self.request()
        b.tick(True)
        self.finish(b)
        self.assertTrue(os.path.isfile(os.path.join(self.dir, "prev", "summary.json")))

    def test_stop_interrupts_and_off_ignores_request(self) -> None:
        b = self.bts("sleep")
        self.request()
        b.tick(True)
        proc = b.proc
        b.tick(False)  # «Стоп»
        self.assertIsNone(b.proc)
        self.assertIsNotNone(proc.poll())
        self.assertIn("Стоп", self.status()["error"])
        self.assertFalse(os.path.exists(os.path.join(self.dir, "latest")))
        self.request()
        b.tick(False)
        self.assertIsNone(b.proc)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "run.request")))

    def test_nightly_and_memory_guard(self) -> None:
        b = self.bts("0", limit=1792)
        b.st["next"] = 1
        b.tick(True, now=1000.0)
        self.assertIsNotNone(b.proc, "ночь наступила — пересчёт")
        self.assertEqual(b.st["next"], lv.next_run(1000.0))
        self.finish(b)
        small = self.bts("0", limit=384)  # старая служба: MemoryMax=384M
        self.request()
        small.tick(True)
        self.assertIsNone(small.proc)
        self.assertIn("переустановите", self.status()["error"])

    def test_corrupted_status_file_is_sanitized(self) -> None:
        with open(os.path.join(self.dir, "backtest_status.json"), "w") as f:
            json.dump({"next": "завтра", "started": [1], "finished": None, "error": 5}, f)
        b = lv.Backtests(self.dir, "/db/hyperliquid.db", self.script, limit_mb=lambda: 4096)
        self.assertIsInstance(b.st["next"], int)
        self.assertGreater(b.st["next"], 0)
        self.assertEqual((b.st["started"], b.st["finished"], b.st["error"]), (0, 0, ""))
        b.tick(True, now=1000.0)  # не падает на сравнении строки с числом

    def test_stubborn_process_is_killed(self) -> None:
        """Пересчёт не вышел по terminate за 30 с — kill, а не исключение из службы."""
        calls = []

        class P:
            def terminate(self) -> None:
                calls.append("term")

            def kill(self) -> None:
                calls.append("kill")

            def wait(self, timeout: float) -> int:
                calls.append("wait")
                if calls.count("wait") == 1:
                    raise lv.subprocess.TimeoutExpired("bt", timeout)
                return -9

        lv.Backtests._stop(P(), terminate=True)  # type: ignore[arg-type]
        self.assertEqual(calls, ["term", "wait", "kill", "wait"])

    def test_cgroup_garbage_is_no_limit(self) -> None:
        import io
        files = [io.StringIO("0::/system.slice/x\n"), io.StringIO("мусор\n")]
        with mock.patch("builtins.open", side_effect=lambda *a, **k: files.pop(0)):
            self.assertEqual(lv.cgroup_limit_mb(), float("inf"))

    def test_progress_shown_and_runs_replay_with_live_rules(self) -> None:
        """Ход пересчёта — строкой во вкладку (раньше — только «считается»
        часами); бэктест — повтор по правилам живого бота, капитал — как у
        счёта сейчас, последние 30 дней."""
        os.makedirs(os.path.join(self.dir, "live"))
        with open(os.path.join(self.dir, "live", "state.json"), "w") as f:
            json.dump({"equity": 124.83}, f)
        b = self.bts("progress")
        self.request()
        b.tick(True)
        args = b.proc.args
        self.assertEqual((args[args.index("--since-days") + 1], args[args.index("--deposit") + 1]), ("30", "124.83"))
        for _ in range(100):
            b.tick(True)
            if "25 %" in self.status().get("progress", ""):
                break
            lv.time.sleep(0.05)
        self.assertEqual(self.status()["progress"], "повтор: 25 % (250 из 1 000 сделок китов), сделок 3, запросов к OKX 17")
        b.tick(False)
        self.assertEqual(self.status()["progress"], "")
        self.assertTrue(lv.Backtests(self.dir, "/db").script.endswith("autotrade_replay.py"))
        self.assertTrue(os.path.isfile(lv.Backtests(self.dir, "/db").script))

    def test_deposit_unknown_is_1000(self) -> None:
        self.assertEqual(lv.Backtests(self.dir, "/db").deposit(), 1000.0)

    def test_next_run_nightly_utc(self) -> None:
        day = 1_800_000_000 // 86400 * 86400
        at = day + 3 * 3600 + 17 * 60
        self.assertEqual((lv.next_run(day), lv.next_run(at), lv.next_run(at - 1)), (at, at + 86400, at))

    def test_unit_gives_room_for_backtest(self) -> None:
        with open(os.path.join(ROOT, "deploy", "whale-autotrade.service"), encoding="utf-8") as f:
            unit = f.read()
        mem = int(next(ln for ln in unit.splitlines() if ln.startswith("MemoryMax=")).split("=")[1].rstrip("M"))
        self.assertGreaterEqual(mem, lv.BT_NEED_MB)
        self.assertGreater(lv.BT_NEED_MB, lv.BT_MEM_MB)


if __name__ == "__main__":
    r = unittest.main(exit=False, verbosity=1).result
    if r.wasSuccessful():
        print(f"autotrade_live: {r.testsRun} проверок на поддельной OKX — без сети, ключей и денег")
    sys.exit(0 if r.wasSuccessful() else 1)
