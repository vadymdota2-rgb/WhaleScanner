#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Реальная торговля автокопирования (tools/autotrade_live.py) — на поддельной
OKX и синтетической базе: без сети, без ключей, без денег.

Проверяется: подпись запросов OKX; сигнал — 2 кита от 1000 $ за 2 часа;
размер — 10 % капитала × 10×; тейк +1 % лимитным ордером reduce-only; одна
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
        self.eq = 10_000.0
        self.pos: list[dict] = []
        self.orders: list[dict] = []
        self.history: list[dict] = []
        self.calls: list[tuple[str, str, dict]] = []
        self.pos_mode = pos_mode
        self.n = 0

    def __call__(self, method: str, url: str, headers: dict, body: bytes | None):
        u = urllib.parse.urlparse(url)
        q = dict(urllib.parse.parse_qsl(u.query))
        b = json.loads(body) if body else {}
        self.calls.append((method, u.path, {**q, **b}))
        ok = lambda data: {"code": "0", "msg": "", "data": data}  # noqa: E731
        p = u.path
        if p == "/api/v5/public/instruments":
            return ok(list(self.instr.values()))
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
        if p == "/api/v5/trade/order" and method == "POST":
            self.n += 1
            o = {**b, "ordId": str(self.n), "state": "live"}
            if b["ordType"] == "market":
                o.update(state="filled", avgPx=str(self.last[b["instId"]]), accFillSz=b["sz"])
                sign = 1 if b["side"] == "buy" else -1
                self.pos.append({"instId": b["instId"], "pos": str(sign * float(b["sz"]) if self.pos_mode == "net_mode"
                                                                      else float(b["sz"])),
                                 "posSide": b.get("posSide", "net"), "avgPx": o["avgPx"], "markPx": o["avgPx"],
                                 "upl": "0", "lever": "10", "mgnMode": "cross", "cTime": "1"})
            self.orders.append(o)
            return ok([{"ordId": o["ordId"], "clOrdId": b.get("clOrdId", ""), "sCode": "0"}])
        if p == "/api/v5/trade/order":
            o = next(o for o in self.orders if o["ordId"] == q.get("ordId") or o.get("clOrdId") == q.get("clOrdId"))
            return ok([o])
        if p == "/api/v5/trade/orders-pending":
            return ok([o for o in self.orders if o["state"] == "live" and o["instId"] == q["instId"]])
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
                "ts INTEGER, notional_nanos INTEGER, sz TEXT)")
    con.executemany("INSERT INTO hl_fills VALUES (?,?,?,?,?,?,?)", fills)
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
        # 10 % от 10 000 $ × 10 = 10 000 $; контракт 0,01 BTC × 100 000 = 1 000 $ → 10 контрактов.
        self.assertEqual((m["instId"], m["side"], m["tdMode"], m["sz"]), ("BTC-USDT-SWAP", "buy", "cross", "10"))
        self.assertIn(("POST", "/api/v5/account/set-leverage", {"instId": "BTC-USDT-SWAP", "lever": "10", "mgnMode": "cross"}),
                      self.ex.calls)
        [tp] = [o for o in self.ex.orders if o["ordType"] == "limit"]
        self.assertEqual((tp["side"], tp["px"], tp["sz"], tp["reduceOnly"]), ("sell", "101000", "10", True))
        st = self.state()
        self.assertEqual((st["open"]["inst"], st["open"]["tp"], st["on"]), ("BTC-USDT-SWAP", 101000.0, True))

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
        self.assertFalse(self.state()["on"])

    def test_any_position_blocks_even_manual(self) -> None:
        self.ex.pos = [{"instId": "ETH-USDT-SWAP", "pos": "-3", "posSide": "net", "avgPx": "3000"}]  # руками владельца
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        self.assertEqual(self.market_orders(), [])
        self.assertEqual(self.state()["last_skip"]["why"], "на счёте уже есть позиция")

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


if __name__ == "__main__":
    r = unittest.main(exit=False, verbosity=1).result
    if r.wasSuccessful():
        print(f"autotrade_live: {r.testsRun} проверок на поддельной OKX — без сети, ключей и денег")
    sys.exit(0 if r.wasSuccessful() else 1)
