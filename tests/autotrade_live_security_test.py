#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Реальная торговля — атаки и порча (таблица атак — docs/autotrade-how-it-works.md,
«Модель угроз»). Без сети, ключей и денег: поддельная OKX из autotrade_live_test.

| Атака / порча                                   | Чем закрыто                                   |
|-------------------------------------------------|-----------------------------------------------|
| перенаправление на чужой адрес                  | _NoRedirect — отказ, заголовки не уходят      |
| запрос не на OKX (подменённый base)             | http_transport — только адреса OKX_BASES      |
| прокси из окружения                             | ProxyHandler({}) — напрямую                   |
| огромный ответ                                  | MAX_REPLY — больше не читаем                  |
| не JSON, не объект, data не список              | OkxError / пустой список                      |
| NaN, бесконечность, «1e999», bool, мусор в числах| num/dec — не число                            |
| мусор в каталоге бумаги (шаг, размер, минимум)  | вход — пропуск, ордера нет                    |
| цена тикера мусор или 0                         | вход — пропуск                                |
| цена исполнения мусор, объём больше заказанного | цена тикера, объём — заказанный               |
| тейк по нулевой цене или «в рынок»              | _place_tp — отказ                             |
| секреты в файлах состояния и журнале            | только в памяти Okx; файлы — права 600        |
| clOrdId не по правилам OKX                      | буквы и цифры, ≤ 32                           |
"""
import email.message
import io
import json
import os
import re
import stat
import sys
import unittest
import urllib.request
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import autotrade_live as lv  # noqa: E402
from autotrade_live_test import MIN, Base, FakeOkx  # noqa: E402


class TransportTest(unittest.TestCase):
    def test_redirect_is_refused(self) -> None:
        h = lv._NoRedirect()
        req = urllib.request.Request("https://www.okx.com/api/v5/account/balance", headers={"OK-ACCESS-KEY": "K"})
        with self.assertRaisesRegex(lv.OkxError, "перенаправление на evil.example — отказ"):
            h.redirect_request(req, io.BytesIO(), 302, "Found", email.message.Message(),
                               "https://evil.example/steal")

    def test_only_okx_hosts_and_no_env_proxy(self) -> None:
        for url in ("https://evil.example/api/v5/x", "http://www.okx.com/api/v5/x",
                    "https://www.okx.com.evil.example/api", "https://eea.okx.com@evil.example/api"):
            with self.assertRaisesRegex(lv.OkxError, "не из списка OKX"):
                lv.http_transport("GET", url, {}, None)
        # Прокси из окружения (HTTPS_PROXY) не подхватывается: обработчика
        # прокси с адресами в открывателе нет вовсе.
        with mock.patch.dict(os.environ, {"HTTPS_PROXY": "http://evil.example:8080"}):
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), lv._NoRedirect())
        for o in (lv._OPENER, opener):
            self.assertFalse([h for h in o.handlers if isinstance(h, urllib.request.ProxyHandler) and h.proxies])
        self.assertTrue(any(isinstance(h, lv._NoRedirect) for h in lv._OPENER.handlers))

    def test_huge_or_garbage_reply(self) -> None:
        with self.assertRaisesRegex(lv.OkxError, "больше"):
            lv._read_json(io.BytesIO(b"[" + b"1," * 600 + b"1]"), 1000)
        with self.assertRaisesRegex(lv.OkxError, "не JSON"):
            lv._read_json(io.BytesIO(b"\xff<html>"), 1000)
        self.assertEqual(lv._read_json(io.BytesIO(b'{"code":"0"}'), 1000), {"code": "0"})
        for bad in ([], "x", 1, None, {"code": "0", "data": "x"}):
            okx = lv.Okx("K", "S", "P", False, lambda *_a, b=bad: b)
            if isinstance(bad, dict):
                self.assertEqual(okx.positions(), [])
            else:
                with self.assertRaises(lv.OkxError):
                    okx.positions()

    def test_numbers(self) -> None:
        for v in ("NaN", "nan", "inf", "-inf", "1e999", True, False, None, [], {}, "abc", ""):
            self.assertEqual(lv.num(v), 0.0, repr(v))
            self.assertIsNone(lv.dec(v), repr(v))
        self.assertEqual((lv.num("1.5"), lv.num(2), lv.num("-3")), (1.5, 2.0, -3.0))
        for v in ("0", "-1", 0, -0.5):
            self.assertIsNone(lv.dec(v), repr(v))
        self.assertEqual(str(lv.dec("0.0001")), "0.0001")
        self.assertEqual(lv.round_step(float("nan"), "0.1", "ROUND_DOWN"), 0)
        self.assertEqual(lv.round_step(5.0, "abc", "ROUND_DOWN"), 0)
        self.assertEqual(lv.round_step(5.0, "0", "ROUND_DOWN"), 0)


class GarbageTradeTest(Base):
    def order_count(self) -> int:
        return len([c for c in self.ex.calls if c[1] == "/api/v5/trade/order" and c[0] == "POST"])

    def test_bad_catalog_or_price_no_order(self) -> None:
        for field, value in (("lotSz", "0"), ("ctVal", "NaN"), ("minSz", "abc"), ("tickSz", "-1"), ("ctVal", None)):
            self.ex = FakeOkx()
            self.ex.instr["BTC-USDT-SWAP"][field] = value
            tr = self.trader(self.whales())
            self.on()
            tr.step()
            self.assertEqual(self.order_count(), 0, (field, value))
            self.assertIn("неверные параметры", self.state()["last_skip"]["why"])
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))
        for last in ("NaN", "0", "inf", "-5"):
            self.ex = FakeOkx()
            self.ex.last["BTC-USDT-SWAP"] = last  # type: ignore[assignment]
            tr = self.trader(self.whales())
            self.on()
            tr.step()
            self.assertEqual(self.order_count(), 0, last)
            os.remove(self.db)
            os.remove(os.path.join(self.dir, "live", "state.json"))

    def test_bad_fill_data_uses_ticker_and_ordered_size(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        real = self.ex.__call__

        def spoil(method, url, headers, body):
            got = real(method, url, headers, body)
            if url.split("?")[0].endswith("/api/v5/trade/order") and method == "GET":
                got["data"][0].update(avgPx="NaN", accFillSz="999999")
            return got

        tr.okx._t = spoil
        tr.step()
        op = self.state()["open"]
        self.assertEqual((op["px"], op["size"], op["tp"]), (100_000.0, 10.0, 101_000.0))

    def test_absurd_fill_price_never_puts_tp_into_market(self) -> None:
        """Шорт, биржа вернула цену исполнения 10^20: тейк (покупка) встал бы
        выше рынка и исполнился сразу. Цена дальше 5 % от тикера — порча:
        берётся тикер."""
        tr = self.trader(self.whales(side=-1))
        self.on()
        real = self.ex.__call__

        def spoil(method, url, headers, body):
            got = real(method, url, headers, body)
            if url.split("?")[0].endswith("/api/v5/trade/order") and method == "GET":
                got["data"][0] = {**got["data"][0], "avgPx": str(10**20)}
            return got

        tr.okx._t = spoil
        tr.step()
        [tp] = [o for o in self.ex.orders if o["ordType"] == "limit"]
        self.assertEqual((tp["side"], tp["px"]), ("buy", "99000"))

    def test_adopt_with_absurd_entry_price_places_nothing(self) -> None:
        tr = self.trader(self.whales())
        p = {"instId": "BTC-USDT-SWAP", "pos": "10", "posSide": "net", "avgPx": "1e20", "markPx": "100000"}
        tr.adopt(p, {"ordId": "1", "avgPx": "1e20"})
        self.assertEqual(self.ex.orders, [])
        self.assertIsNone(tr.st.get("open"))
        self.assertTrue(any("не сходится с рынком" in e for e in self.logs))

    def test_tp_never_at_zero_or_into_market(self) -> None:
        tr = self.trader(self.whales())
        for px, tp, side in ((0, 0, 1), (100.0, 0.0, 1), (100.0, 100.5, 1), (100.0, 99.0, 1), (100.0, 101.0, -1)):
            tr._place_tp({"inst": "BTC-USDT-SWAP", "side": side, "px": px, "tp": tp, "tp_s": str(tp)}, 10)
        self.assertEqual([o for o in self.ex.orders if o["ordType"] == "limit"], [])
        tr._place_tp({"inst": "BTC-USDT-SWAP", "side": 1, "px": 100.0, "tp": 101.0, "tp_s": "101"}, 10)
        self.assertEqual(len([o for o in self.ex.orders if o["ordType"] == "limit"]), 1)

    def test_secrets_never_in_files_and_files_private(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        self.now += 30 * MIN
        self.ex.pos = []
        tr.last_sync = 0
        tr.step()
        for name in os.listdir(os.path.join(self.dir, "live")):
            with open(os.path.join(self.dir, "live", name), encoding="utf-8") as f:
                text = f.read()
            for secret in ("SECRET-very-private", "PASS-x", "KEY-x"):
                self.assertNotIn(secret, text, name)
        for line in self.logs:
            self.assertNotIn("SECRET-very-private", line)
        # Служба ставит umask 077: файлы состояния — только владельцу.
        env = {k: v for k, v in os.environ.items() if not k.startswith("OKX_")}
        old = os.umask(0o022)
        try:
            with mock.patch.dict(os.environ, env, clear=True), mock.patch("sys.stdout"):
                lv.main(["--db", self.db, "--dir", self.dir])
        finally:
            os.umask(old)
        mode = stat.S_IMODE(os.stat(os.path.join(self.dir, "live", "state.json")).st_mode)
        self.assertEqual(mode & 0o077, 0, oct(mode))

    def test_client_order_ids_are_okx_valid(self) -> None:
        tr = self.trader(self.whales())
        self.on()
        tr.step()
        ids = [o["clOrdId"] for o in self.ex.orders]
        self.assertTrue(ids)
        for c in ids:
            self.assertRegex(c, r"^[A-Za-z0-9]{1,32}$")
        self.assertTrue(all(re.match(lv.TAG, c) for c in ids))

    def test_signature_has_no_host_and_body_is_exact(self) -> None:
        seen: list = []
        okx = lv.Okx("K", "S", "P", False, lambda m, u, h, b: seen.append((m, u, h, b)) or {"code": "0", "data": []},
                     base="https://eea.okx.com")
        okx.order({"instId": "BTC-USDT-SWAP", "sz": "1"})
        m, u, h, b = seen[0]
        self.assertTrue(u.startswith("https://eea.okx.com/api/v5/trade/order"))
        self.assertEqual(json.loads(b), {"instId": "BTC-USDT-SWAP", "sz": "1"})
        self.assertNotIn("eea.okx.com", h["OK-ACCESS-TIMESTAMP"] + "POST/api/v5/trade/order" + b.decode())


if __name__ == "__main__":
    r = unittest.main(exit=False, verbosity=1).result
    if r.wasSuccessful():
        print(f"autotrade_live_security: {r.testsRun} атак и порч — закрыты")
    sys.exit(0 if r.wasSuccessful() else 1)
