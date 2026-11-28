#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Автокопирование китов — РЕАЛЬНАЯ торговля на OKX (этап 3, по команде владельца).

Отдельная служба (whale-autotrade.service): ключи OKX есть только у неё, API
и бот их не видят. Ключи — в /etc/whale-autotrade.env (root, 600), systemd
передаёт их через окружение:

    OKX_API_KEY, OKX_API_SECRET, OKX_API_PASSPHRASE   — ключ OKX (права: только торговля)
    OKX_SIMULATED=1                                    — демо-счёт OKX (по умолчанию 0 — реальный)

Правила владельца:
- сигнал — от 2 разных китов Hyperliquid в одну сторону на одной бумаге за
  2 часа, у каждого от 1000 $ (сделки китов — из базы бота, только чтение);
- ПРОТИВ КИТОВ (правило владельца 28.11.2026): сделка — в обратную сторону от
  сигнала (киты в лонг — бот в шорт); цена входа, MA30, стоп и тейк — по
  стороне сделки; киты — тоже толпа, кроме крупных против толпы: с другой
  стороны киты (от 2), а сигнальные крупнее — с ними, без разворота; выхода вслед за китом в убытке нет (его убыток — наш плюс);
- только фьючерсы (крипта, акции, металлы), бумага должна быть на OKX и цена
  сходиться с ценой китов (±10 %, было ±2 %) — иначе это другой актив;
- цена входа — CHASE от средней цены китов по стороне сделки: против китов
  шорт — от +1 % выше их входа, лонг — от 1 % ниже (CHASE = −0,01; правило
  владельца 28.11.2026); было — не хуже их цены (CHASE = 0; до того — больше
  чем на 0,3 %) — пропуск; вход — по рынку ордером IOC с границей «средняя
  цена китов × (1 + сторона × CHASE)»: исполняется сразу, но не хуже границы
  (правило владельца 27.11.2026);
- маржа — всегда 5 % капитала счёта, плечо 10×, кросс (сделки владельца
  руками на счёте входу не мешают);
  на ту же бумагу, где ручная позиция, — не входит;
- своих позиций у бота — до MAX_OPEN = 3 одновременно (правило владельца
  27.11.2026), на разных бумагах: на бумагу, где уже есть своя, — не входит;
- выход — тейк +1,5 % движения цены (лимитный ордер на бирже; правило
  владельца 27.11.2026, было +0,8 %) или стоп; выхода вслед за китом нет
  (убран по правилу владельца 28.11.2026); фильтра «кит уже вышел в убыток»
  тоже нет (убран 28.11.2026: убыток толпы — наш плюс);
- замедление: прибыль хоть раз дошла до +0,8 %, а потом 15 минут нет нового
  максимума — закрыть по рынку, но не меньше +0,8 % (IOC с границей; цена
  ниже — выхода нет, тейк обратно; правило владельца 28.11.2026);
- стоп — 1,5 % от цены входа (лонг — ниже, шорт — выше; по бэктесту
  27.11.2026, было 3 %); на входе и не меняется;
  цена позиции (mark) дошла — снять тейк и закрыть по рынку (только на
  закрытие); работает и при «Стоп» во вкладке; усреднения нет;
- в одну и ту же бумагу — не чаще раза в 3 часа;
- MA30 15-минутного графика (средняя 30 закрытий, идущая — по цене): лонг —
  только цена выше MA30, шорт — ниже — по стороне СДЕЛКИ (правило владельца
  28.11.2026: в шорт не входим, когда цена выше MA30); свечей нет — как без MA; свечи OKX не
  получены (сбой) — не входим до следующего опроса (правило владельца 27.11.2026);
- рейтинг китов по нашим сделкам (правило владельца 27.11.2026): сделка
  засчитывается всем китам её сигнала; винрейт — от 5 сделок; сигналы с
  китами выше винрейтом — раньше; кит с винрейтом выше 90 % —
  приоритетный: с ним входим и в одиночку, без «против крупнее» и MA30
  (цена входа и стоп — как всегда);
- уровней графика 4ч нет (убраны по правилу владельца 27.11.2026);
- входим с крупными: за те же 2 часа по бумаге против сигнала стоят киты
  крупнее по объёму (кошельки от 1000 $) — в слабую сторону не входим; если
  крупных от 2 — входим в их сторону.

Кнопки «Старт»/«Стоп» во вкладке пишут switch.json: «Стоп» — новых входов
нет; открытая позиция, её тейк и стоп остаются.

Состояние — в каталоге модуля (WT_AUTOTRADE_DIR/live): state.json (для
вкладки), trades.jsonl (закрытые сделки), live.log. Ключи туда не пишутся.

    python3 tools/autotrade_live.py --db ~/WhaleScanner/hyperliquid.db
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import gc
import hashlib
import hmac
import json
import math
import os
import shutil
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable
from datetime import datetime, timezone
from decimal import ROUND_CEILING, ROUND_DOWN, ROUND_FLOOR, Decimal
from typing import Any, TextIO

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autotrade_core as core

# Ключ живёт на одном адресе: глобальный OKX, европейский (OKX EEA, вход
# через my.okx.com — API на eea.okx.com) или американский. Чужой адрес
# отвечает 50119 «ключа нет» — бот пробует по очереди и запоминает рабочий.
# OKX_API_BASE в /etc/whale-autotrade.env — проверяется первым.
OKX_BASES = ("https://www.okx.com", "https://eea.okx.com", "https://us.okx.com")
# Понятные причины частых отказов OKX (код → текст во вкладку).
OKX_HINTS = {
    "50119": "ключ не найден ни на одном адресе OKX (www, eea, us) — удалён или введён с ошибкой",
    "50111": "неверный API key", "50113": "неверная подпись — проверьте Secret key",
    "50105": "неверный Passphrase", "50110": "IP сервера не разрешён в настройках ключа OKX",
    "50101": "ключ от другого режима: демо-ключ на реальном счёте или наоборот (OKX_SIMULATED)",
    "50120": "у ключа нет права «Торговля»", "51008": "не хватает маржи на счёте",
    "50124": "ключу API закрыт этот рынок (фьючерсы): в OKX → API откройте ключ и разрешите торговлю "
             "фьючерсами; если такой настройки нет — создайте новый ключ (права «Торговля», без вывода)",
}
NANOS = core.NANOS

# Правила владельца.
MIN_WHALES = 2
MIN_WHALE_USD = 1000            # кит — от 1000 $ в окне (правило владельца 28.11.2026: снова 1000 $, 10 000 $ было днём)
WINDOW_MS = 2 * 3_600_000
# Против китов (правило владельца 28.11.2026): сигнал — как прежде, сделка — в
# обратную сторону; выхода вслед за китом в убытке нет (его убыток — наш плюс).
AGAINST = True
# Цена входа относительно средней цены китов, по стороне сделки: > 0 — не хуже
# неё больше чем на CHASE, 0 — не хуже, < 0 — лучше неё хотя бы на |CHASE|.
# Против китов (правило владельца 28.11.2026): шорт — от +1 % выше их входа,
# лонг — от 1 % ниже (было 0 — не хуже их цены; до того 0,003).
CHASE = -0.01
# Средняя цена китов — дробь с погрешностью (99 999,99999999 вместо 100 000):
# при CHASE = 0 цена ровно на цене китов давала бы «хуже китов», а граница IOC
# уходила бы на шаг цены внутрь. Допуск — 1e-9 (0,0001 $ на 100 000 $).
PX_EPS = 1e-9

# Цена OKX дальше этого от средней цены китов — другой актив с тем же именем
# (пропуск). 10 % (было 2 %): против китов цена дальше +1 % от их входа — наш
# плюс, не повод пропускать (правило владельца 28.11.2026).
MATCH_TOL = 0.10
# Маржа входа — всегда 5 % капитала (правило владельца 22.11.2026; раньше —
# 10 %, а при ручной сделке на счёте — 5 %). Своя позиция у бота — одна.
MARGIN_FRAC = 0.05
LEVERAGE = 10
# Своих позиций бота — до MAX_OPEN одновременно, на разных бумагах: каждая — 5 %
# маржи × 10×.
MAX_OPEN = 3   # правило владельца 28.11.2026: снова 3 позиции (на час было 1)
# Замедление (правило владельца 28.11.2026): прибыль хоть раз дошла до +0,8 %
# (LOCK_PCT), а потом 15 минут (STALL_MS) нет нового максимума — закрыть по
# рынку, но не меньше +0,8 % (ордер IOC с границей; ниже — ждём тейк или стоп).
LOCK_PCT = 0.008
STALL_MS = 15 * 60_000
TP = 0.015                  # тейк +1,5 % движения цены (правило владельца 27.11.2026; было +0,8 %, до того +1 %)
COIN_PAUSE_MS = 3 * 3_600_000
FILL_TOL = 0.05            # цена исполнения входа — не дальше 5 % от границы цены ордера
ENTRY_WAIT_MS = 60_000      # ордер входа (IOC) не исполнен за минуту — снять остаток и решить по исполненному
ADOPT_TOL = 0.5            # подхват позиции: средняя цена — не дальше 50 % от рынка
TP_SLACK = 0.005           # тейк на шаге цены — не дальше +1 % (иначе шаг цены слишком крупный)

FRESH_MS = 15 * 60_000     # сигнал старше — не входим (бот мог отстать, момент ушёл)
POLL_SEC = 5               # опрос базы
SYNC_SEC = 15              # сверка счёта
INSTR_TTL = 3600           # каталог OKX — раз в час
TAG = "wt"                 # приставка clOrdId наших ордеров
# Отказы OKX по самой бумаге (не по счёту): этой бумагой торговать нельзя —
# исключить её на неделю и искать следующие сигналы, а не долбить ордерами.
# 51001 — бумаги нет, 51155/51156 — ограничения по стране (compliance),
# 51010 — режим счёта не позволяет эту бумагу.
BLOCK_CODES = {"51001", "51155", "51156", "51010"}
# Отказы по КЛЮЧУ, а не по бумаге: 50124 — «ключу не разрешена торговля на
# этом рынке». На сервере владельца так OKX отклонил подряд AAPL, BTC, MSFT,
# NVDA, HYPE, ETH, PUMP — исключать бумаги на неделю было неверно (после
# исправления ключа BTC и ETH ещё неделю пропускались бы). Теперь — пауза
# всего рынка (SWAP или FUTURES) на 30 минут: одна проверка ордером раз в
# полчаса, бумаги не исключаются; ключ исправлен — следующий сигнал входит.
MARKET_CODES = {"50124"}
MARKET_PAUSE_MS = 30 * 60_000
BLOCK_MS = 7 * 86_400_000
FAIL_PAUSE_MS = 15 * 60_000  # другой отказ ордера — эту бумагу не трогать 15 минут


def chase_rule() -> str:
    """Правило цены входа относительно средней цены китов — для сообщений."""
    if CHASE < 0:
        return f"на {-CHASE * 100:g} % лучше средней цены китов (шорт — выше, лонг — ниже)"
    if CHASE > 0:
        return f"не хуже средней цены китов больше чем на {CHASE * 100:g} %"
    return "не хуже средней цены китов"


def now_ms() -> int:
    return int(time.time() * 1000)


# ── OKX ──────────────────────────────────────────────────────────────────────

class OkxError(Exception):
    def __init__(self, code: str, msg: str) -> None:
        hint = OKX_HINTS.get(code)
        super().__init__(f"OKX {code}: {msg}" + (f" — {hint}" if hint else ""))
        self.code = code


Transport = Callable[[str, str, dict[str, str], bytes | None], Any]


MAX_REPLY = 8 << 20  # ответ OKX больше — не ответ (каталог бумаг — ~0,6 МБ)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Перенаправление — отказ: иначе подписанные заголовки (ключ и
    Passphrase) ушли бы на адрес, который назвал чужой ответ."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # type: ignore[no-untyped-def]
        raise OkxError(str(code), f"перенаправление на {urllib.parse.urlsplit(newurl).netloc} — отказ")


# Без прокси из окружения (ключ — только напрямую на OKX) и без перенаправлений.
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())


def _read_json(r: Any, limit: int) -> Any:
    raw = r.read(limit + 1)
    if len(raw) > limit:
        raise OkxError("size", f"ответ больше {limit >> 20} МБ — не ответ")
    try:
        return json.loads(raw)
    except ValueError:
        raise OkxError("json", "ответ не JSON") from None


def http_transport(method: str, url: str, headers: dict[str, str], body: bytes | None) -> Any:
    if not url.startswith(tuple(f"{b}/" for b in OKX_BASES)):
        raise OkxError("host", "адрес не из списка OKX — запрос не отправлен")
    req = urllib.request.Request(url, data=body, method=method,
                                 headers={**headers, "Accept-Encoding": "identity"})
    try:
        with _OPENER.open(req, timeout=15) as r:
            return _read_json(r, MAX_REPLY)
    except urllib.error.HTTPError as e:
        try:
            return _read_json(e, 1 << 20)
        except OkxError:
            raise OkxError(str(e.code), "HTTP") from None


def num(v: Any, default: float = 0.0) -> float:
    """Число из ответа OKX (строка или число): мусор, NaN, бесконечность,
    булево — default. Ответ биржи не должен ронять службу или попасть в ордер."""
    if isinstance(v, bool) or not isinstance(v, (int, float, str)):
        return default
    try:
        x = float(v)
    except (ValueError, OverflowError):
        return default
    return x if math.isfinite(x) else default


DEC_MAX = Decimal("1e15")  # параметр бумаги больше — не параметр, а порча


def size_of(v: Any) -> Decimal:
    """Объём позиции из ответа OKX (со знаком в режиме net) — по модулю,
    точно; мусор — 0."""
    if isinstance(v, bool) or not isinstance(v, (int, float, str)):
        return Decimal(0)
    try:
        d = Decimal(str(v))
    except ArithmeticError:
        return Decimal(0)
    return abs(d) if d.is_finite() and abs(d) <= DEC_MAX else Decimal(0)


def dec(v: Any) -> Decimal | None:
    """Параметр бумаги (размер контракта, шаг, минимум) — конечное
    положительное десятичное; иначе None."""
    if isinstance(v, bool) or not isinstance(v, (int, float, str)):
        return None
    try:
        d = Decimal(str(v))
    except ArithmeticError:
        return None
    return d if d.is_finite() and 0 < d <= DEC_MAX else None


class Okx:
    """Подписанный REST OKX v5. Секреты — только в памяти этого объекта."""

    def __init__(self, key: str, secret: str, passphrase: str, simulated: bool,
                 transport: Transport | None = None, base: str = core.OKX_API) -> None:
        self._key, self._secret, self._pass = key, secret, passphrase
        self.simulated = simulated
        self._t = transport or http_transport
        self.base = base.rstrip("/")

    def detect(self, first: str = "") -> str:
        """Найти адрес, на котором живёт ключ: «ключа нет» (50119) — следующий.
        Другая ошибка (подпись, IP, Passphrase) — сразу наружу: адрес верный,
        неверен ключ."""
        order = [b.rstrip("/") for b in (first, *OKX_BASES) if b]
        for b in dict.fromkeys(order):
            self.base = b
            try:
                self.call("GET", "/api/v5/account/config")
            except OkxError as e:
                if e.code == "50119":
                    continue
                raise
            return b
        self.base = order[0]
        raise OkxError("50119", "API key doesn't exist")

    def _sign(self, ts: str, method: str, path: str, body: str) -> str:
        mac = hmac.new(self._secret.encode(), (ts + method + path + body).encode(), hashlib.sha256)
        return base64.b64encode(mac.digest()).decode()

    def call(self, method: str, path: str, params: dict[str, Any] | None = None, body: dict[str, Any] | None = None,
             private: bool = True) -> list[Any]:
        # Только объекты: строка или число в списке ответа — порча, не запись.
        return [x for x in self.call_raw(method, path, params, body, private) if isinstance(x, dict)]

    def call_raw(self, method: str, path: str, params: dict[str, Any] | None = None, body: dict[str, Any] | None = None,
                 private: bool = True) -> list[Any]:
        """Ответ OKX — список data как есть (у свечей записи — списки, а не
        объекты); отказ — OkxError."""
        if params:
            path += "?" + urllib.parse.urlencode(params)
        raw = json.dumps(body, separators=(",", ":")) if body is not None else ""
        headers = {"Content-Type": "application/json", "User-Agent": "wt-autotrade"}
        if private:
            t = datetime.now(timezone.utc)
            ts = t.strftime("%Y-%m-%dT%H:%M:%S.") + f"{t.microsecond // 1000:03d}Z"
            headers.update({"OK-ACCESS-KEY": self._key, "OK-ACCESS-SIGN": self._sign(ts, method, path, raw),
                            "OK-ACCESS-TIMESTAMP": ts, "OK-ACCESS-PASSPHRASE": self._pass})
        if self.simulated:
            headers["x-simulated-trading"] = "1"
        got = self._t(method, self.base + path, headers, raw.encode() if body is not None else None)
        if not isinstance(got, dict):
            raise OkxError("?", "ответ не JSON-объект")
        if str(got.get("code")) != "0":
            data = got.get("data") or [{}]
            d0 = data[0] if isinstance(data, list) and data and isinstance(data[0], dict) else {}
            err = OkxError(str(d0.get("sCode") or got.get("code")), str(d0.get("sMsg") or got.get("msg") or ""))
            err.args = (f"{err.args[0]} [{method} {path.split('?')[0]}]",)  # какой запрос — для разбора
            raise err
        data = got.get("data")
        return data if isinstance(data, list) else []

    # Обёртки — только то, что нужно трейдеру.
    def instruments(self, inst_type: str = "SWAP") -> list[dict[str, Any]]:
        return self.call("GET", "/api/v5/public/instruments", {"instType": inst_type}, private=False)

    def ticker(self, inst: str) -> float:
        d = self.call("GET", "/api/v5/market/ticker", {"instId": inst}, private=False)
        return max(0.0, num(d[0].get("last"))) if d and isinstance(d[0], dict) else 0.0

    def ma30_15m(self, inst: str, last: float) -> float:
        """MA30 15-минутного графика: средняя 30 закрытий (идущая свеча — по
        цене last). Свечей меньше 30 или мусор — 0 (MA нет)."""
        d = self.call_raw("GET", "/api/v5/market/candles",
                          {"instId": inst, "bar": "15m", "limit": str(core.MA_BARS)}, private=False)
        closes = [num(r[4], math.nan) for r in d if isinstance(r, list) and len(r) >= 5]
        if len(closes) < core.MA_BARS or not all(math.isfinite(c) and c > 0 for c in closes) or last <= 0:
            return 0.0
        return (sum(closes[1:core.MA_BARS]) + last) / core.MA_BARS   # первая — идущая (OKX: свежие первыми)

    def config(self) -> dict[str, Any]:
        d = self.call("GET", "/api/v5/account/config")
        return d[0] if d and isinstance(d[0], dict) else {}

    def equity(self) -> float:
        d = self.call("GET", "/api/v5/account/balance")
        return max(0.0, num(d[0].get("totalEq"))) if d and isinstance(d[0], dict) else 0.0

    def positions(self) -> list[dict[str, Any]]:
        """ВСЕ позиции счёта — бессрочные, срочные фьючерсы, маржа, опционы:
        ручная сделка владельца любого вида видна боту (на её бумагу бот не входит;
        своя ли позиция — по метке ордера); раньше бот видел только бессрочные."""
        return [p for p in self.call("GET", "/api/v5/account/positions")
                if isinstance(p, dict) and num(p.get("pos")) != 0]

    def set_leverage(self, inst: str, lever: int) -> None:
        self.call("POST", "/api/v5/account/set-leverage", body={"instId": inst, "lever": str(lever), "mgnMode": "cross"})

    def order(self, body: dict[str, Any]) -> str:
        d = self.call("POST", "/api/v5/trade/order", body=body)
        return str(d[0].get("ordId") or "") if d and isinstance(d[0], dict) else ""

    def get_order(self, inst: str, ord_id: str = "", cl_id: str = "") -> dict[str, Any]:
        p = {"instId": inst, **({"ordId": ord_id} if ord_id else {"clOrdId": cl_id})}
        d = self.call("GET", "/api/v5/trade/order", p)
        return d[0] if d and isinstance(d[0], dict) else {}

    def pending(self, inst: str) -> list[dict[str, Any]]:
        # Вид контракта — по имени: европейский бессрочный (…_XPERP-…) у OKX — FUTURES.
        return self.call("GET", "/api/v5/trade/orders-pending", {"instType": inst_type(inst), "instId": inst})

    def cancel(self, inst: str, ord_id: str) -> None:
        self.call("POST", "/api/v5/trade/cancel-order", body={"instId": inst, "ordId": ord_id})

    def history(self, inst: str) -> list[dict[str, Any]]:
        """Последние ордера по бумаге (за 7 дней) — чтобы узнать свои по метке clOrdId."""
        return self.call("GET", "/api/v5/trade/orders-history", {"instType": inst_type(inst), "instId": inst, "limit": "50"})

    def closed(self, inst: str) -> list[dict[str, Any]]:
        return self.call("GET", "/api/v5/account/positions-history", {"instType": inst_type(inst), "instId": inst})


def inst_type(inst: str) -> str:
    """Вид контракта OKX по имени: «…-SWAP» — SWAP, остальное (в т. ч.
    европейский бессрочный «BTC-USD_UM_XPERP-310815») — FUTURES."""
    return "SWAP" if inst.endswith("-SWAP") else "FUTURES"


# ── округление под правила бумаги ────────────────────────────────────────────

def contract_size(ins: dict[str, Any], notional: float, px: float) -> Decimal:
    """Объём в контрактах на notional $ по цене px — как ордер: десятичными
    (10 000 / (10 000 000 × 0,00001) — ровно 100, а не 99,999…), вниз по шагу
    бумаги; размер контракта — в монете или уже в долларах (ctValCcy).
    Параметры бумаги — мусор, или меньше минимального ордера — 0."""
    ct_val, ct_mult = dec(ins.get("ctVal")), dec(ins.get("ctMult") or "1")
    step, min_sz = dec(ins.get("lotSz")), dec(ins.get("minSz"))
    if ct_val is None or ct_mult is None or step is None or min_sz is None or not notional > 0 or not px > 0:
        return Decimal(0)
    ct_d = ct_val * ct_mult
    in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
    qty = Decimal(repr(round(notional, 6))) / (ct_d if in_usd else ct_d * Decimal(repr(px)))
    sz = (qty / step).to_integral_value(rounding=ROUND_DOWN) * step
    return sz if sz >= min_sz else Decimal(0)


def fmt_dec(x: Decimal) -> str:
    s = format(x.normalize(), "f")
    return s if s not in ("-0", "") else "0"


def round_step(value: float | Decimal, step: Any, mode: str) -> Decimal:
    """Округлить до шага бумаги. Шаг или значение — мусор (не конечное
    положительное) — 0: вызывающий такое не отправляет. Decimal (размер
    позиции) — без потери точности; float (цена) — до 12 значащих цифр."""
    st = step if isinstance(step, Decimal) and step.is_finite() and step > 0 else dec(step)
    if isinstance(value, Decimal):
        if st is None or not value.is_finite() or value <= 0:
            return Decimal(0)
        return (value / st).to_integral_value(rounding=mode) * st
    if st is None or not math.isfinite(value) or value <= 0:
        return Decimal(0)
    # 12 значащих цифр: хвост двоичной дроби (101000.00000000001) не должен
    # сдвигать цену тейка на шаг вверх.
    return (Decimal(f"{value:.12g}") / st).to_integral_value(rounding=mode) * st


# ── сигналы: сделки китов из базы бота (только чтение) ───────────────────────

class Signals:
    """Окна китов по бумаге и стороне — по ходу, из новых строк базы бота.
    Куски одного ордера складываются в сумму кита (как в бэктесте)."""

    def __init__(self, db: str, keep: bool = False) -> None:
        self.db = db
        self.keep = keep
        self._kept: sqlite3.Connection | None = None
        self.windows: dict[tuple[str, int], core.Window] = {}
        self.seen: dict[int, int] = {}   # tid → ts (против повторов при перекрытии опроса)
        self.priority: set[str] = set()  # приоритетные киты (наш винрейт с ними > 90 % от 5 сделок)
        self.limit = MIN_WHALE_USD * NANOS   # кит — кошелёк от MIN_WHALE_USD в окне

    def _con(self) -> sqlite3.Connection:
        return core.open_ro(self.db)  # нет базы — OSError: ошибка шага, а не выход службы

    def _rows(self, sql: str, args: tuple[Any, ...]) -> list[Any]:
        """Запрос к базе бота (только чтение). Служба — соединение на запрос
        (бот мог пересоздать базу); бэктест (keep) — одно на весь проход:
        десятки тысяч запросов о выходе китов без открытия базы на каждый."""
        if self.keep:
            if self._kept is None:
                self._kept = self._con()
            return self._kept.execute(sql, args).fetchall()
        con = self._con()
        try:
            return con.execute(sql, args).fetchall()
        finally:
            con.close()

    def close(self) -> None:
        if self._kept is not None:
            self._kept.close()
            self._kept = None

    def poll(self, now: int) -> list[dict[str, Any]]:
        """Новые строки базы → окна; сигналы: бумага и сторона, где сейчас ≥ 2
        китов от 1000 $ за 2 часа, и строка, на которой это случилось."""
        # Всё окно 2 часа каждый раз: бот записывает сделки китов с опозданием
        # (опрос кошельков), и опоздавшая сделка должна попасть в окно, а не
        # потеряться. Повторы отсекает seen (tid).
        since = now - WINDOW_MS
        rows = self._rows(
            "SELECT tid, wallet, coin, dir_code, ts, notional_nanos, ABS(CAST(sz AS REAL)) FROM hl_fills "
            "WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ? ORDER BY ts, tid", (since, now))
        out = self.feed(rows)
        cut = now - WINDOW_MS - 20 * 60_000
        self.seen = {t: s for t, s in self.seen.items() if s >= cut}
        return out

    def feed(self, rows: Any) -> list[dict[str, Any]]:
        """Строки сделок открытия (tid, кошелёк, бумага, код, время, нано $,
        штук) → окна и сигналы. Тот же код — и у живого бота (poll), и у
        бэктеста-повтора (autotrade_replay.py): правила одни."""
        out: list[dict[str, Any]] = []
        for tid, wallet, coin, code, ts, nanos, units in rows:
            if tid in self.seen or not core.is_perp(coin):
                continue
            self.seen[tid] = ts
            side = 1 if code == core.OPEN_LONG else -1
            w = self.windows.setdefault((coin, side), core.Window(self.limit))
            w.add(ts, wallet, int(nanos or 0), float(units or 0.0))
            # Окно — 2 часа до самой свежей сделки (опоздавшая старше окна
            # тут же выходит); момент сигнала — самая свежая сделка окна.
            newest = w.orders[-1][0]
            w.expire(newest - WINDOW_MS)
            # Встречная сторона той же бумаги за те же 2 часа: киты против.
            opp = self.windows.get((coin, -side))
            if opp is not None:
                opp.expire(newest - WINDOW_MS)
            # Момент сигнала — сделка КИТА (кошелёк от 1000 $ в окне). Мелкая
            # сделка (не кит) сигнал не «освежает»: иначе вход через полтора
            # часа после китов по чьей-то сделке на 50 $ выглядел бы свежим.
            v = w.per.get(wallet)
            if v is None or v[0] < self.limit:
                continue
            if (w.count >= MIN_WHALES or wallet in self.priority) and w.big_u > 0:
                # Приоритетный кит — сигнал и в одиночку (правило владельца 27.11.2026).
                out.append(self._signal(coin, side, w, opp, newest))
            # Против этих китов стоят киты крупнее (по объёму) и их тоже от 2 —
            # сигнал и в их сторону: входим с крупными (правило владельца).
            if opp is not None and opp.count >= MIN_WHALES and opp.big_u > 0 \
                    and self.whale_usd(opp) > self.whale_usd(w):
                out.append(self._signal(coin, -side, opp, w, newest))
        return out

    def whale_usd(self, w: core.Window | None) -> float:
        """Объём китов окна (кошельки от 1000 $), $."""
        return float(w.whale_n) / NANOS if w is not None else 0.0   # сумма — по ходу (core.Window)

    def priority_in(self, coin: str, side: int, wallets: object, now: int) -> str:
        """Приоритетный кит сигнала, что ещё в окне за 2 часа до «сейчас» (от
        1000 $): кошелёк или ''."""
        w = self.windows.get((coin, side))
        if w is None or not isinstance(wallets, dict):
            return ""
        w.expire(now - WINDOW_MS)
        return next((x for x in sorted(wallets) if x in self.priority and (v := w.per.get(x)) is not None
                     and v[0] >= self.limit), "")

    def balance(self, coin: str, side: int, now: int) -> tuple[float, float, int]:
        """На момент решения (окна за 2 часа до «сейчас»), а не на момент строки
        сигнала: объём китов за и против и сколько китов за (за время опроса
        могли прийти киты в другую сторону, а старые — выйти из окна)."""
        out = []
        for sd in (side, -side):
            w = self.windows.get((coin, sd))
            if w is not None:
                w.expire(now - WINDOW_MS)
            out.append(self.whale_usd(w))
        w = self.windows.get((coin, side))
        return out[0], out[1], (w.count if w is not None else 0)

    def _signal(self, coin: str, side: int, w: core.Window, opp: core.Window | None, ts: int) -> dict[str, Any]:
        """Сигнал: бумага, сторона, киты (от 1000 $) и их первая сделка в окне
        (от неё смотрим, не закрыли ли они в убыток), средняя цена китов,
        объём китов сигнала и китов против (та же бумага, те же 2 часа)."""
        big = {wl for wl, v in w.per.items() if v[2] > 0 and v[0] >= self.limit}
        first: dict[str, int] = {}
        for t, wl, _n, _u in w.orders:
            if wl in big and wl not in first:
                first[wl] = t
        # Крупные против толпы: с другой стороны тоже киты (от 2), а эти — крупнее
        # по объёму. Против китов бот такой сигнал не разворачивает — идёт с
        # крупными (правило владельца 28.11.2026: киты — тоже толпа, кроме этих).
        big_side = opp is not None and opp.count >= MIN_WHALES and self.whale_usd(w) > self.whale_usd(opp)
        return {"coin": coin, "side": side, "ts": ts, "whales": w.count, "wallets": first,
                "ref_px": (w.big_n / NANOS) / w.big_u, "usd": round(self.whale_usd(w), 2),
                "usd_against": round(self.whale_usd(opp), 2), **({"big": True} if big_side else {})}

    def sweep(self, now: int) -> None:
        """Очистка: окна бумаг, по которым давно не было сделок, сами не
        пустеют (выбрасывают старое только на новой сделке) — вычистить
        старше 2 часов и убрать пустые."""
        for w in self.windows.values():
            w.expire(now - WINDOW_MS)
        self.windows = {k: w for k, w in self.windows.items() if w.orders}


# ── бэктест: ночью и по кнопке, в этой службе (не в API) ─────────────────────

BT_HOUR_UTC, BT_MINUTE = 3, 17
# Бэктест — повтор по правилам живого бота (autotrade_replay.py): свечи OKX
# (публичное API) только под кандидатов и сделки. Раньше — сетка прежнего
# бэктеста с паузой 15 с к Hyperliquid: часы без видимого хода («завис»).
BT_PAUSE_SEC = 0.11               # между запросами к OKX на все потоки (публичное API: до 20 в 2 с)
BT_DAYS = 30                      # сколько последних дней повторять
BT_MEM_MB = 1200                  # предел памяти самого бэктеста (RLIMIT_AS)
BT_NEED_MB = 1600                 # столько должна разрешать служба (MemoryMax) — иначе не запускаем
BT_TIMEOUT_SEC = 6 * 3600


def next_run(now: float) -> int:
    day = int(now) // 86400 * 86400
    at = day + BT_HOUR_UTC * 3600 + BT_MINUTE * 60
    return at if at > now else at + 86400


def cgroup_limit_mb() -> float:
    """Предел памяти своей службы (cgroup v2 memory.max), МБ; нет — бесконечность."""
    try:
        with open("/proc/self/cgroup", encoding="utf-8") as f:
            path = next((ln.split("::", 1)[1].strip() for ln in f if ln.startswith("0::")), "")
        with open(f"/sys/fs/cgroup{path}/memory.max", encoding="utf-8") as f:
            raw = f.read().strip()
    except (OSError, StopIteration):
        return math.inf
    try:
        return math.inf if raw == "max" else int(raw) / 2**20
    except ValueError:
        return math.inf


class Backtests:
    """Бэктест отдельным процессом с низким приоритетом: ночью в 03:17 UTC и
    по запросу из вкладки (run.request от API), пока модуль включён. Удачный
    итог — latest/ (прежний — prev/), неудачный прежний не портит. «Стоп» —
    прерывает. Ход — в backtest_status.json (его показывает вкладка).
    Раньше бэктест шёл внутри API и ронял её по памяти: здесь у службы свой
    предел, и если он меньше BT_NEED_MB — не запускаем, а говорим почему."""

    def __init__(self, data_dir: str, db: str, script: str | None = None,
                 limit_mb: Callable[[], float] = cgroup_limit_mb) -> None:
        self.dir, self.db = data_dir, db
        self.script = script or os.path.join(os.path.dirname(os.path.abspath(__file__)), "autotrade_replay.py")
        self.limit_mb = limit_mb
        self.proc: subprocess.Popen[bytes] | None = None
        self.log: TextIO | None = None
        self.st = self._read()
        self.st.update(running=False)
        # Файл мог быть испорчен руками или обрывом записи: поля — числа и строка.
        self.st["next"] = int(num(self.st.get("next"))) or next_run(time.time())
        for k in ("started", "finished"):
            self.st[k] = int(num(self.st.get(k)))
        self.st["error"] = self.st["error"] if isinstance(self.st.get("error"), str) else ""
        self.st["progress"] = ""

    def _read(self) -> dict[str, Any]:
        try:
            with open(os.path.join(self.dir, "backtest_status.json"), encoding="utf-8") as f:
                got = json.load(f)
                return got if isinstance(got, dict) else {}
        except (OSError, ValueError):
            return {}

    def _write(self) -> None:
        tmp = os.path.join(self.dir, "backtest_status.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.st, f, ensure_ascii=False)
        os.replace(tmp, os.path.join(self.dir, "backtest_status.json"))

    def tick(self, on: bool, now: float | None = None) -> None:
        now = time.time() if now is None else now
        req = os.path.join(self.dir, "run.request")
        if self.proc is not None:
            rc = self.proc.poll()
            self._progress()
            if not on and rc is None:
                self._stop(self.proc, terminate=True)
                self._done(None, "остановлено кнопкой «Стоп»")
            elif not on:
                self._done(rc, "")
            elif rc is None and now - num(self.st.get("started"), now) > BT_TIMEOUT_SEC:
                self._stop(self.proc, terminate=False)
                self._done(None, "бэктест шёл дольше 6 часов — остановлен")
            elif rc is not None:
                self._done(rc, "")
            return
        wanted = os.path.exists(req) or now >= self.st["next"]
        if not (on and wanted):
            if not on and os.path.exists(req):
                os.remove(req)
            return
        if os.path.exists(req):
            os.remove(req)
        self.st["next"] = next_run(now)
        if self.limit_mb() < BT_NEED_MB:
            self.st.update(error=f"службе whale-autotrade разрешено {self.limit_mb():.0f} МБ, бэктесту нужно "
                                 f"{BT_NEED_MB} — переустановите: sudo bash /home/vadymdota2/WhaleScanner/tools/autotrade-install.sh",
                           finished=int(now))
            self._write()
            return
        out = os.path.join(self.dir, "run")
        shutil.rmtree(out, ignore_errors=True)
        self.log = open(os.path.join(self.dir, "last_run.log"), "w", encoding="utf-8")  # noqa: SIM115 — закрывается в _done
        self.proc = subprocess.Popen(  # свой скрипт, аргументы списком — без оболочки
            [sys.executable, "-I", self.script, "--db", self.db, "--out", out, "--cache",
             os.path.join(self.dir, "cache"), "--pause", str(BT_PAUSE_SEC), "--max-mem-mb", str(BT_MEM_MB),
             "--since-days", str(BT_DAYS), "--deposit", str(self.deposit())],
            stdout=self.log, stderr=subprocess.STDOUT, preexec_fn=lambda: os.nice(19))  # noqa: PLW1509 — однопоточная служба
        self.st.update(running=True, started=int(now), error="", progress="запуск")
        self._write()

    def deposit(self) -> float:
        """Капитал для бэктеста — как у счёта сейчас (из состояния торговли);
        неизвестен — 1000 $."""
        try:
            with open(os.path.join(self.dir, "live", "state.json"), encoding="utf-8") as f:
                eq = num(json.load(f).get("equity"))
        except (OSError, ValueError, AttributeError):
            eq = 0.0
        return round(eq, 2) if eq > 0 else 1000.0

    def _progress(self) -> None:
        """Ход пересчёта (progress.json бэктеста) — во вкладку строкой."""
        try:
            with open(os.path.join(self.dir, "run", "progress.json"), encoding="utf-8") as f:
                p = json.load(f)
        except (OSError, ValueError):
            return
        if not isinstance(p, dict):
            return
        total, done = num(p.get("total")), num(p.get("done"))
        text = f"{p.get('stage', '')}".strip()
        if total > 0:
            text += f": {done / total * 100:.0f} % ({int(done):,} из {int(total):,} сделок китов)".replace(",", " ")
            spent = time.time() - num(self.st.get("started"), time.time())
            if 0 < done < total and spent > 60:   # осталось — по скорости с начала пересчёта
                text += f", осталось ≈ {max(1, round(spent * (total - done) / done / 60))} мин"
        text += f", сделок {int(num(p.get('trades')))}, запросов к OKX {int(num(p.get('requests')))}"
        if text != self.st.get("progress"):
            self.st["progress"] = text[:200]
            self._write()

    def restart_for_update(self) -> None:
        """Код обновился посреди пересчёта: остановить его и заказать заново —
        новая служба сразу начнёт пересчёт новым кодом."""
        if self.proc is None:
            return
        self._stop(self.proc, terminate=True)
        self._done(None, "код обновился — пересчёт начат заново новой версией")
        with open(os.path.join(self.dir, "run.request"), "w", encoding="utf-8") as f:
            json.dump({"at": int(time.time()), "why": "update"}, f)

    @staticmethod
    def _stop(proc: subprocess.Popen[bytes], terminate: bool) -> None:
        """Остановить пересчёт: вежливо (terminate), не вышел за 30 с — kill."""
        if terminate:
            proc.terminate()
            try:
                proc.wait(timeout=30)
                return
            except subprocess.TimeoutExpired:
                pass
        proc.kill()
        proc.wait(timeout=30)

    def _done(self, rc: int | None, why: str) -> None:
        self.proc = None
        if self.log:
            self.log.close()
            self.log = None
        out = os.path.join(self.dir, "run")
        if not why:
            if rc == 3:
                why = f"бэктесту не хватило предела памяти {BT_MEM_MB} МБ — остановлен сам, сервер не тронут"
            elif rc != 0 or not os.path.isfile(os.path.join(out, "summary.json")):
                why = f"бэктест завершился с кодом {rc} (last_run.log)"
        if not why:
            prev, cur = os.path.join(self.dir, "prev"), os.path.join(self.dir, "latest")
            shutil.rmtree(prev, ignore_errors=True)
            if os.path.isdir(cur):
                os.replace(cur, prev)
            os.replace(out, cur)
        self.st.update(running=False, finished=int(time.time()), error=why, progress="")
        self._write()


# ── автоочистка: память и диск не растут ─────────────────────────────────────

CLEAN_SEC = 600                    # раз в 10 минут
LOG_MAX_BYTES = 5 << 20            # live.log больше — в live.log.1 (одна старая копия)
CACHE_MAX_DAYS = 30                # кеш свечей бэктеста — не старше
CACHE_MAX_BYTES = 500 << 20        # и не больше 500 МБ (старые — первыми)
RSS_MAX_MB = 400                   # служба (без бэктеста) больше — перезапуск через systemd


def rss_mb() -> float:
    """Сколько памяти занимает этот процесс (RSS), МБ; неизвестно — 0."""
    try:
        with open("/proc/self/statm", encoding="utf-8") as f:
            return int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE") / 2**20
    except (OSError, ValueError, IndexError):
        return 0.0


class Janitor:
    """Раз в 10 минут: окна китов и списки — только нужное; журнал — с
    ротацией; кеш свечей бэктеста — не старше 30 дней и не больше 500 МБ
    (не во время пересчёта); сборка мусора. Если служба всё равно разрослась
    больше RSS_MAX_MB — выход: systemd поднимет её заново (позиция и тейк —
    на бирже, состояние — в файле, ничего не теряется)."""

    def __init__(self, data_dir: str, rss: Callable[[], float] = rss_mb) -> None:
        self.dir = data_dir
        self.rss = rss
        self.last = 0.0

    def due(self, now: float) -> bool:
        return now - self.last >= CLEAN_SEC

    def run(self, tr: Trader, bts: Backtests | None, now: float) -> bool:
        """Очистка; True — пора перезапуститься (память не вернулась)."""
        self.last = now
        tr.sig.sweep(int(now * 1000))
        cut = int(now * 1000) - COIN_PAUSE_MS
        entered = tr.st.get("entered")
        tr.st["entered"] = {c: t for c, t in entered.items() if num(t) >= cut} if isinstance(entered, dict) else {}
        # Истёкшие запреты бумаг (15 минут — сбой, неделя — OKX не даёт) — убрать.
        blocked = tr.st.get("blocked")
        ms = int(now * 1000)
        tr.st["blocked"] = {i: b for i, b in blocked.items() if isinstance(b, dict)
                            and ms - num(b.get("at")) < (BLOCK_MS if b.get("permanent") else FAIL_PAUSE_MS)} \
            if isinstance(blocked, dict) else {}
        mb = tr.st.get("market_block")
        tr.st["market_block"] = {k: v for k, v in mb.items() if isinstance(v, dict) and ms - num(v.get("at")) < MARKET_PAUSE_MS} \
            if isinstance(mb, dict) else {}
        self.rotate(os.path.join(tr.dir, "live.log"))
        busy = bts is not None and bts.proc is not None   # идёт пересчёт — его лог и кеш не трогаем
        if not busy:
            self.rotate(os.path.join(self.dir, "last_run.log"))
            self.clean_cache(os.path.join(self.dir, "cache"), now)
        gc.collect()
        mb = self.rss()
        tr.st["janitor"] = {"at": int(now * 1000), "rss_mb": round(mb, 1)}
        return mb > RSS_MAX_MB and not busy and "entering" not in tr.st

    @staticmethod
    def rotate(path: str) -> None:
        try:
            if os.path.getsize(path) > LOG_MAX_BYTES:
                os.replace(path, path + ".1")
        except OSError:
            pass

    @staticmethod
    def clean_cache(cache: str, now: float) -> int:
        """Кеш свечей бэктеста: старше 30 дней — удалить; всё равно больше
        500 МБ — удалять самые старые. Сколько байт освобождено."""
        try:
            files = [(e.stat().st_mtime, e.stat().st_size, e.path) for e in os.scandir(cache) if e.is_file()]
        except OSError:
            return 0
        freed = 0
        keep = []
        for mtime, size, path in sorted(files):
            if now - mtime > CACHE_MAX_DAYS * 86400:
                try:
                    os.remove(path)
                    freed += size
                except OSError:
                    pass
            else:
                keep.append((mtime, size, path))
        total = sum(size for _m, size, _p in keep)
        for _mtime, size, path in keep:  # старые первыми
            if total <= CACHE_MAX_BYTES:
                break
            try:
                os.remove(path)
                freed += size
                total -= size
            except OSError:
                pass
        return freed


# ── трейдер ──────────────────────────────────────────────────────────────────

OPEN_KEYS = ("coin", "inst", "side", "opened", "px", "size", "tp", "tp_s", "notional")  # поля записи своей позиции


class Trader:
    def __init__(self, okx: Okx, signals: Signals, data_dir: str, clock: Callable[[], int] = now_ms,
                 log: Callable[[str], None] | None = None) -> None:
        self.okx, self.sig, self.clock = okx, signals, clock
        self.dir = os.path.join(data_dir, "live")
        self.switch = os.path.join(data_dir, "switch.json")
        os.makedirs(self.dir, exist_ok=True)
        self._log = log or (lambda s: print(s, flush=True))
        self.st = self._load()
        self.instr: dict[str, dict[str, Any]] = {}
        self.instr_at = 0.0
        self.xperp: dict[str, str] = {}   # бумага → европейский бессрочный (XPERP), где он есть
        self.pos_mode = ""
        self.lever_set: set[str] = set()
        self.positions: list[dict[str, Any]] = []
        self.equity = 0.0
        self.last_sync = 0.0
        self.unverified: set[str] = set()   # позиции, которые не удалось проверить по истории ордеров
        self._checked: dict[str, float] = {}
        self.skip_code = "other"
        self._skip_seen: set[tuple[str, int, str, int]] = set()
        self.retry: list[dict[str, Any]] = []   # сигналы, пропущенные из-за сбоя свечей — повторить
        self.rating = core.Rating()   # наш винрейт с китами — по trades.jsonl
        self._load_rating()

    # состояние
    def _load(self) -> dict[str, Any]:
        st: dict[str, Any] = {}
        try:
            with open(os.path.join(self.dir, "state.json"), encoding="utf-8") as f:
                got = json.load(f)
                st = got if isinstance(got, dict) else {}
        except (OSError, ValueError):
            pass
        # Файл пишет сама служба, но правка руками или порча не должны ронять
        # каждый шаг: поля — своих типов, иначе пустые.
        for k, kind in (("entered", dict), ("events", list), ("blocked", dict), ("own_unknown", dict),
                        ("market_block", dict), ("skips", dict), ("closed_ids", list)):
            if not isinstance(st.get(k), kind):
                st[k] = kind()
        st["events"] = [e for e in st["events"] if isinstance(e, str)]
        # Свои позиции: opens (список); у прежней версии — одна, open.
        got_opens, got_open = st.get("opens"), st.get("open")
        raw: list[Any] = got_opens if isinstance(got_opens, list) else [got_open] if got_open is not None else []
        opens: list[dict[str, Any]] = []
        for op in raw:
            if not (isinstance(op, dict) and all(k in op for k in OPEN_KEYS)
                    and op["side"] in (1, -1) and isinstance(op["inst"], str)
                    and num(op["px"]) > 0 and num(op["tp"]) > 0 and num(op["size"]) > 0) \
                    or any(o["inst"] == op["inst"] for o in opens):
                # Запись позиции испорчена (или повтор бумаги): забыть её — find_own
                # узнает свою позицию по метке ордера.
                self._log(f"[AUTO-LIVE] запись позиции в state.json испорчена — сверка по счёту: {str(op)[:200]}")
                continue
            if not isinstance(op.get("exit", {}), dict):
                op.pop("exit")
            opens.append(op)
        st["opens"] = opens[:MAX_OPEN]
        st["open"] = st["opens"][0] if st["opens"] else None
        if not isinstance(st.get("entering"), dict):
            st.pop("entering", None)
        st["error"] = st["error"] if isinstance(st.get("error"), str) else ""
        return st

    def save(self) -> None:
        # Во вкладку (wt_autotrade.live) — только то, что она читает; open —
        # первая позиция для вкладки прежней версии.
        self.st["open"] = self.st["opens"][0] if self.st["opens"] else None
        self.st.update(heartbeat=self.clock(), simulated=self.okx.simulated, equity=self.equity,
                       positions=[{k: p.get(k) for k in ("instId", "upl")} for p in self.positions])
        tmp = os.path.join(self.dir, "state.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.st, f, ensure_ascii=False)
        os.replace(tmp, os.path.join(self.dir, "state.json"))

    def event(self, text: str) -> None:
        line = f"{datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')} {text}"
        self._log(f"[AUTO-LIVE] {text}")
        self.st["events"] = [*self.st.get("events", [])[-49:], line]
        try:
            with open(os.path.join(self.dir, "live.log"), "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except OSError:
            pass

    def is_on(self) -> bool:
        try:
            with open(self.switch, encoding="utf-8") as f:
                got = json.load(f)
        except (OSError, ValueError):
            return False
        return isinstance(got, dict) and got.get("on") is True

    # каталог и счёт
    def instruments(self) -> dict[str, dict[str, Any]]:
        """Каталог бессрочных контрактов. Глобальный OKX — «BTC-USDT-SWAP»;
        европейский OKX (EEA) торгует бессрочными как FUTURES вида
        «BTC-USD_UM_XPERP-310815», а SWAP в каталоге видны, но торговать ими
        нельзя (50124). Где есть XPERP — берём его."""
        if not self.instr or time.time() - self.instr_at > INSTR_TTL:
            got: dict[str, dict[str, Any]] = {}
            for r in self.okx.instruments("SWAP"):
                if r.get("state") == "live" and str(r.get("instId", "")).endswith("-USDT-SWAP"):
                    got[r["instId"]] = r
            try:
                futs = self.okx.instruments("FUTURES")
            except OkxError:
                futs = []
            for r in futs:
                if r.get("state") == "live" and "_XPERP" in str(r.get("instId", "")):
                    got[r["instId"]] = r
            if got:
                self.instr, self.instr_at = got, time.time()
                xperp: dict[str, str] = {}
                for i in sorted(got, key=lambda x: ("_UM_XPERP" not in x, x)):  # USD-маржинальные — первыми
                    if "_XPERP" in i:
                        xperp.setdefault(i.split("-", 1)[0], i)
                self.xperp = xperp
        return self.instr

    def inst_for(self, base: str) -> str:
        """Контракт для бумаги: европейский бессрочный (XPERP), если есть, иначе USDT-SWAP."""
        self.instruments()
        return self.xperp.get(base) or f"{base}-USDT-SWAP"

    def sync(self) -> None:
        if not self.pos_mode:
            cfg = self.okx.config()
            self.pos_mode = str(cfg.get("posMode") or "net_mode")
        self.positions = self.okx.positions()
        self.equity = self.okx.equity()
        self.last_sync = time.time()
        held = {str(p.get("instId")) for p in self.positions}
        if self.st.get("own_unknown"):
            self.st["own_unknown"] = {k: v for k, v in self.st["own_unknown"].items() if k in held}
        self.resolve_entering()
        self.find_own()
        self._track_open()
        self.follow_whales()

    def resolve_entering(self) -> None:
        """Вход начат (ордер, возможно, ушёл), но не записан: сбой между
        рыночным ордером и записью сделки. Пока не ясно, исполнился ли он, —
        новых входов нет (иначе второй рыночный ордер). Ордер найден по метке
        и исполнен, позиция есть — она своя: принять и поставить тейк; ордер
        отменён или за 10 минут не подтвердился позицией — входа не было."""
        e = self.st.get("entering")
        if not isinstance(e, dict) or any(o["inst"] == e.get("inst") for o in self.st["opens"]):
            self.st.pop("entering", None)
            return
        if e.get("limit"):
            self._resolve_entry(e)
            return
        # Запись прежней версии (вход по рынку).
        try:
            o = self.okx.get_order(str(e.get("inst")), cl_id=str(e.get("cl")))
        except OkxError:
            o = {}
        side = int(num(e.get("side"), 1)) or 1
        live = [p for p in self.positions if p.get("instId") == e.get("inst") and self._side_of(p) == side]
        if o.get("state") in ("filled", "partially_filled") and live:
            self.adopt(live[0], o, e)
            self.st.pop("entering", None)
        elif o.get("state") in ("canceled", "mmp_canceled") or self.clock() - num(e.get("at")) > 600_000:
            # 10 минут ордер не подтвердился позицией: снять запись. Если позиция
            # всё же наша — find_own узнает её по метке ордера.
            self.event(f"вход {e.get('inst')} не состоялся ({o.get('state') or 'ордера нет'}) — запись снята")
            self.st.pop("entering", None)

    def _track_open(self) -> None:
        """Наши позиции: есть — тейк на месте? нет — закрылась (тейк или
        руками): записать сделку."""
        for op in list(self.st["opens"]):
            self._track_one(op)

    def _track_one(self, op: dict[str, Any]) -> None:
        # Своя позиция — та же бумага и та же сторона (в режиме long/short на
        # бумаге может быть и встречная ручная позиция владельца).
        live = [p for p in self.positions if p.get("instId") == op["inst"] and self._side_of(p) == op["side"]]
        if live:
            p = live[0]
            op.update(mark=num(p.get("markPx")), upl=num(p.get("upl")))
            if op.get("exit"):
                return  # выходим по стопу — тейк не ставить; закрытие повторит follow_whales
            if op.get("tp_id") and any(o.get("ordId") == op["tp_id"] for o in self.okx.pending(op["inst"])):
                return
            # Тейка в ожидающих нет: исполнен — сделка закрыта, хотя позиция на
            # бумаге осталась (её докупили руками) — новый тейк закрыл бы часть
            # ручной; снят — поставить заново, не больше своего объёма.
            tp = self.okx.get_order(op["inst"], str(op.get("tp_id") or "")) if op.get("tp_id") else {}
            if tp.get("state") == "filled":
                self._record_close(op, {"uTime": tp.get("fillTime") or tp.get("uTime"), "closeAvgPx": tp.get("avgPx"),
                                        "realizedPnl": tp.get("pnl"), "fee": tp.get("fee")})
                return
            self._place_tp(op, min(size_of(p.get("pos")), size_of(op.get("size"))))
            return
        if self.clock() - num(op.get("opened")) < 60_000:
            return  # биржа ещё не показала только что открытую позицию
        hist = [h for h in self.okx.closed(op["inst"]) if num(h.get("uTime")) >= num(op.get("opened"))]
        if not hist and self.clock() - num(op.get("gone_at"), self.clock()) < 300_000:
            op.setdefault("gone_at", self.clock())  # позиции нет, а истории ещё нет — подождать до 5 минут
            return
        self._record_close(op, hist[0] if hist else {})

    @staticmethod
    def _rated(t: dict[str, Any]) -> bool:
        """Сделка — в рейтинг, только если она того же вида, что торгует бот
        сейчас (правило владельца 28.11.2026): против китов (AGAINST) — только
        сделки с пометкой fade; по китам — только без неё. Иначе высокий
        винрейт с китом значил бы обратное: «шли за ним и выиграли»."""
        return (t.get("fade") is True) == AGAINST

    def _load_rating(self) -> None:
        """Рейтинг китов — по нашим закрытым сделкам (trades.jsonl) того же
        вида, что сейчас (_rated): сделка — всем китам её сигнала; плюсовая —
        pnl > 0. Приоритетные — в сигналы."""
        self.rating = core.Rating()
        try:
            with open(os.path.join(self.dir, "trades.jsonl"), encoding="utf-8") as f:
                for line in f:
                    try:
                        t = json.loads(line)
                    except ValueError:
                        continue
                    if isinstance(t, dict) and self._rated(t):
                        self.rating.add(t.get("wallets"), num(t.get("pnl")) > 0)
        except OSError:
            pass
        self.sig.priority = self.rating.priority()

    def _record_close(self, op: dict[str, Any], h: dict[str, Any]) -> None:
        """Сделка закрыта: запись — один раз (сначала «позиции нет» в состоянии,
        потом снятие остатка тейка: сбой снятия не повторит запись)."""
        pnl = num(h.get("realizedPnl"), num(h.get("pnl")))
        out_px = num(h.get("closeAvgPx"))
        trade = {"coin": op["coin"], "inst": op["inst"], "side": op["side"], "opened": op["opened"],
                 "closed": int(num(h.get("uTime"), self.clock())) or self.clock(), "px_in": op["px"], "px_out": out_px,
                 "tp": op["tp"], "size": op["size"], "pnl": round(pnl, 4), "fee": num(h.get("fee")),
                 "funding": num(h.get("fundingFee")), "notional": op["notional"],
                 # Тейк — лимит: исполнение ровно по его цене (0,05 % — запас на
                 # среднюю цену); выход по рынку рядом с тейком — не тейк.
                 "why": "тейк" if out_px > 0 and abs(out_px / op["tp"] - 1) < 0.0005
                 else {"stop": "стоп", "stall": "замедление"}.get(str(op["exit"].get("kind")), "кит вышел в убыток")
                 if op.get("exit")
                 else "закрыта",
                 "wallets": sorted(w for w in op.get("wallets") or {} if isinstance(w, str)),
                 **({"fade": True} if op.get("fade") else {})}   # против китов — для рейтинга
        with open(os.path.join(self.dir, "trades.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps(trade, ensure_ascii=False) + "\n")
        if self._rated(trade):
            self.rating.add(trade["wallets"], pnl > 0)
            self.sig.priority = self.rating.priority()
        self.st["opens"] = [o for o in self.st["opens"] if (o["inst"], o["side"]) != (op["inst"], op["side"])]
        # Вход этой сделки — уже закрыт: позицию на той же бумаге (докупленную
        # владельцем руками) find_own не примет за свою по метке этого ордера.
        self.st["closed_ids"] = [*self.st.get("closed_ids", [])[-99:], str(op.get("in_id") or "")]
        self.event(f"закрыта {op['inst']} {'лонг' if op['side'] > 0 else 'шорт'}: {trade['why']}, PnL {pnl:+.2f} $")
        self.save()
        try:
            for o in self.okx.pending(op["inst"]):  # оставшийся тейк бота — снять
                if str(o.get("clOrdId", "")).startswith(TAG):
                    self.okx.cancel(op["inst"], str(o.get("ordId")))
        except OkxError as e:
            self.event(f"{op['inst']}: остаток тейка не снят — {e}; проверьте ордера в OKX")

    # выход по рынку: стоп (выхода вслед за китом нет — правило владельца 28.11.2026)
    def follow_whales(self) -> None:
        """Стоп (цена позиции дошла до стопа входа) — закрыть свою: снять
        тейк и рыночным ордером только на закрытие. Не вышло (сбой, отказ) —
        запись «выходим» остаётся, тейк заново не ставится, попытка — раз в
        минуту. Выход вслед за китом в убытке убран: бот торгует против китов."""
        for op in list(self.st["opens"]):
            self._follow_one(op)

    def _follow_one(self, op: dict[str, Any]) -> None:
        now = self.clock()
        ex = op.get("exit")
        if isinstance(ex, dict) and ex.get("kind") == "stall":
            # Выход по замедлению ещё идёт, а цена дошла до стопа — стоп важнее.
            why = self._stop_hit(op)
            if why:
                ex.update(kind="stop", why=why, tried=0)
                self.event(f"{op['inst']}: {why} — закрываю позицию бота по рынку")
        if not op.get("exit"):
            why, kind = self._stop_hit(op), "stop"
            if not why:
                why, kind = self._stall_hit(op, now), "stall"
            if not why:
                return
            op["exit"] = {"at": now, "tried": 0, "why": why, "kind": kind}
            self.event(f"{op['inst']}: {why} — закрываю позицию бота "
                       + ("по рынку" if kind == "stop" else f"по рынку, не меньше +{LOCK_PCT * 100:g} %"))
        if now - num(op["exit"].get("tried")) < 60_000:
            return
        op["exit"]["tried"] = now
        self.save()
        try:
            # Сначала — сколько уже закрыто прежними ордерами выхода (ответ мог
            # не дойти, исполнение — частичным): повтор закрывает только
            # остаток своего объёма, а не ручную докупку владельца.
            got = self._exit_fills(op)
            if got is None:
                return  # прежний ордер выхода ещё исполняется
            remaining = size_of(op.get("size")) - got[0]
            if remaining <= 0:
                self._record_close(op, got[1])
                return
            live = [p for p in self.positions if p.get("instId") == op["inst"] and self._side_of(p) == op["side"]]
            if not live:
                return  # позиции уже нет — закрытие запишет _track_open по истории
            for po in self.okx.pending(op["inst"]):  # снять свой тейк
                if str(po.get("clOrdId", "")).startswith(TAG):
                    self.okx.cancel(op["inst"], str(po.get("ordId")))
            ins = self.instruments().get(op["inst"], {})
            sz = round_step(min(size_of(live[0].get("pos")), remaining), ins.get("lotSz", "1"), ROUND_DOWN)
            if sz <= 0:
                return
            cl = f"{TAG}ex{now % 10**12}"
            body = {"instId": op["inst"], "tdMode": "cross", "side": "sell" if op["side"] > 0 else "buy",
                    "ordType": "market", "sz": fmt_dec(sz), "clOrdId": cl, **self._ps(op["side"])}
            if op["exit"].get("kind") == "stall":
                # Замедление — по рынку, но не меньше +0,8 %: IOC с границей (лонг
                # продаёт не ниже, шорт покупает не выше; на шаге цены в нашу пользу).
                lock = round_step(op["px"] * (1 + op["side"] * LOCK_PCT), ins.get("tickSz", "0.0001"),
                                  ROUND_CEILING if op["side"] > 0 else ROUND_FLOOR)
                body.update(ordType="ioc", px=fmt_dec(lock))
            if self.pos_mode != "long_short_mode":
                body["reduceOnly"] = True  # только закрытие: не перевернуть позицию
            # Метка ордера — в запись ДО отправки: ответ не дошёл — ордер всё
            # равно найдётся по метке, и повтор не закроет лишнего.
            op["exit"].setdefault("ids", []).append(cl)
            self.save()
            self.okx.order(body)
            for _ in range(10):
                if self.okx.get_order(op["inst"], cl_id=cl).get("state") in ("filled", "canceled", "mmp_canceled"):
                    break
                time.sleep(0.5)
            got = self._exit_fills(op)
        except OkxError as e:
            self.event(f"{op['inst']}: закрыть по рынку не вышло — {e}; повтор через минуту")
            return
        if got is not None and got[0] >= size_of(op.get("size")):
            self._record_close(op, got[1])
        elif got is not None and got[0] <= 0 and op["exit"].get("kind") == "stall":
            # Цена ушла ниже +0,8 % — ничего не продано: выхода нет, тейк обратно;
            # дальше — как прежде (тейк, стоп или снова замедление выше +0,8 %).
            op.pop("exit", None)
            self.event(f"{op['inst']}: выход по замедлению не исполнился — цена ниже +{LOCK_PCT * 100:g} %; тейк обратно")
            self._place_tp(op, size_of(op.get("size")))
            self.save()

    def _stall_hit(self, op: dict[str, Any], now: int) -> str:
        """Замедление: прибыль хоть раз дошла до +LOCK_PCT (с этого момента
        помним лучшую цену — op["best"] и когда она была — op["best_at"]), а
        потом STALL_MS нет нового максимума и прибыль сейчас не меньше
        +LOCK_PCT (цена позиции и цена сделки) — причина строкой; нет — пусто."""
        live = [p for p in self.positions if p.get("instId") == op["inst"] and self._side_of(p) == op["side"]]
        px, side = num(op.get("px")), op["side"]
        mark = num(live[0].get("markPx")) if live else 0.0
        if not live or px <= 0 or mark <= 0:
            return ""
        gain = side * (mark / px - 1)
        best = num(op.get("best"))
        if gain >= LOCK_PCT and (best <= 0 or side * (mark - best) > 0):
            op["best"], op["best_at"] = mark, now   # новый максимум прибыли — отсчёт заново
            return ""
        if best <= 0 or now - num(op.get("best_at")) < STALL_MS or gain < LOCK_PCT:
            return ""
        last = self.okx.ticker(op["inst"])   # подтвердить ценой сделки: одна испорченная цена не закрывает
        if last <= 0 or side * (last / px - 1) < LOCK_PCT:
            return ""
        return (f"движение замедлилось: {STALL_MS // 60_000} минут без нового максимума "
                f"(лучшая {best:.6g}), прибыль {gain * 100:.2f} %")

    def _stop_hit(self, op: dict[str, Any]) -> str:
        """Стоп входа: цена позиции (mark) и цена сделки (тикер) обе дошли до
        op["stop"] (лонг — ниже или на нём, шорт — выше или на нём; mark нет —
        по тикеру) — причина строкой; нет — пусто.
        Стопа в записи нет (позиция прежней версии) или позиции на счёте нет —
        не стоп (закрытие запишет _track_open)."""
        stop = num(op.get("stop"))
        live = [p for p in self.positions if p.get("instId") == op["inst"] and self._side_of(p) == op["side"]]
        if not live or stop <= 0:
            return ""
        mark = num(live[0].get("markPx"))
        if mark > 0 and op["side"] * (mark - stop) > 0:
            return ""
        # Цена позиции за стопом (или её нет) — подтвердить ценой сделки
        # (тикер): одна испорченная цена в ответе OKX не закрывает позицию.
        last = self.okx.ticker(op["inst"])
        if last <= 0 or op["side"] * (last - stop) > 0:
            return ""
        mark = mark if mark > 0 else last
        return f"стоп: цена {mark:.6g} дошла до стопа {stop:.6g} ({'ниже' if op['side'] > 0 else 'выше'} входа {op['px']:.6g})"

    def _exit_fills(self, op: dict[str, Any]) -> tuple[Decimal, dict[str, Any]] | None:
        """Ордера выхода по рынку (стоп): сколько исполнено и итог для записи
        (средняя цена, PnL и комиссии — суммой). Какой-то ещё исполняется — None."""
        done, val, pnl, fee, last = Decimal(0), 0.0, 0.0, 0.0, 0.0
        for cl in op["exit"].get("ids", []):
            try:
                o = self.okx.get_order(op["inst"], cl_id=str(cl))
            except OkxError as e:
                if e.code != "51603":
                    raise
                o = {}  # 51603 «ордера нет» — до биржи не дошёл, исполнения нет
            if o.get("state") in ("live", "partially_filled"):
                return None
            q = size_of(o.get("accFillSz"))
            done += q
            val += float(q) * num(o.get("avgPx"))
            pnl += num(o.get("pnl"))
            fee += num(o.get("fee"))
            last = max(last, num(o.get("fillTime")) or num(o.get("uTime")))
        return done, {"uTime": last or None, "closeAvgPx": val / float(done) if done > 0 else 0.0,
                      "realizedPnl": pnl, "fee": fee}

    # вход
    def _ps(self, side: int) -> dict[str, Any]:
        return {"posSide": "long" if side > 0 else "short"} if self.pos_mode == "long_short_mode" else {}

    def _place_tp(self, op: dict[str, Any], size: Decimal) -> None:
        ins = self.instruments().get(op["inst"], {})
        sz = round_step(size, ins.get("lotSz", "1"), ROUND_DOWN)
        # Тейк — только по ту сторону от входа, не ближе тейка (ни нулевой цены,
        # ни цены «в рынок»: такой лимит исполнился бы сразу с убытком).
        if sz <= 0 or not (op.get("px", 0) > 0 and op.get("tp", 0) > 0
                           and op["side"] * (op["tp"] / op["px"] - 1) >= TP * 0.999):
            self.event(f"тейк {op.get('inst')} не поставлен: цена {op.get('tp')} или объём {fmt_dec(sz)} неверны")
            return
        body = {"instId": op["inst"], "tdMode": "cross", "side": "sell" if op["side"] > 0 else "buy",
                "ordType": "limit", "px": op["tp_s"], "sz": fmt_dec(sz),
                "clOrdId": f"{TAG}tp{self.clock() % 10**12}", **self._ps(op["side"])}
        if self.pos_mode != "long_short_mode":
            body["reduceOnly"] = True
        op["tp_id"] = self.okx.order(body)
        self.event(f"тейк {op['inst']} {op['tp_s']} × {fmt_dec(sz)}")

    def candidates(self, sigs: list[dict[str, Any]]) -> list[dict[str, Any]]:
        now = self.clock()
        seen: set[tuple[str, int]] = set()
        out = []
        for s in reversed(sigs):  # свежие первыми, по бумаге и стороне — один
            key = (s["coin"], s["side"])
            if key in seen or now - s["ts"] > FRESH_MS:
                continue
            seen.add(key)
            out.append(s)
        # Выше наш винрейт с китами сигнала — раньше (свежесть — при равном).
        out.sort(key=lambda s: -self.rating.best(s.get("wallets")))
        return out

    def _skip(self, code: str, why: str) -> str:
        """Причина пропуска: текст — вкладке, код — в счётчик за сутки."""
        self.skip_code = code
        return why

    def count_skip(self, s: dict[str, Any], code: str) -> None:
        """Счётчик пропусков по причинам — по часам, 24 часа. Один сигнал
        (бумага и сторона) с одной причиной за час — один раз: новые сделки
        китов по той же бумаге не раздувают счёт."""
        hour = self.clock() // 3_600_000 * 3_600_000
        key = (s["coin"], s["side"], code, hour)
        if key in self._skip_seen:
            return
        self._skip_seen = {k for k in self._skip_seen if k[3] == hour} | {key}
        skips = {h: v for h, v in self.st["skips"].items() if isinstance(v, dict) and num(h) > hour - 24 * 3_600_000}
        bucket = skips.setdefault(str(hour), {})
        bucket[code] = int(num(bucket.get(code))) + 1
        self.st["skips"] = skips

    def ma_check(self, inst: str, last: float, side: int) -> str:
        """MA30 15-минутного графика: '' — цена
        по ту же сторону MA30, что и сделка, или у бумаги нет 30 свечей
        (входим), иначе — причина пропуска. Свечи не получены (сбой) —
        пропуск до следующего опроса."""
        try:
            ma = self.okx.ma30_15m(inst, last)
        except (OkxError, OSError) as e:
            return self._skip("candles", f"{inst}: свечи OKX не получены ({str(e)[:120]}) — повтор на следующем опросе")
        if core.ma_ok(ma, last, side) is not False:
            return ""
        return self._skip("ma30", f"{inst}: цена {last:.6g} {'ниже' if side > 0 else 'выше'} MA30 15м {ma:.6g} — "
                                  f"{'лонг' if side > 0 else 'шорт'} не входит")

    def try_enter(self, s: dict[str, Any]) -> str:
        """Вход по сигналу; возвращает причину пропуска ('' — вошли)."""
        now = self.clock()
        mine = {o["inst"] for o in self.st["opens"]}
        if len(mine) >= MAX_OPEN:
            return self._skip("own_open", f"у бота уже {len(mine)} позиции ({', '.join(sorted(mine))}) — больше {MAX_OPEN} не открываем")
        if self.st.get("entering"):
            return self._skip("entering", "прошлый ордер входа ещё не исполнен — по одному")
        if self.unverified:
            return self._skip("unverified", f"не удалось проверить позиции ({', '.join(sorted(self.unverified))}) — свои они или ваши")
        unknown = [str(p.get("instId")) for p in self.positions if str(p.get("instId")) in self.st.get("own_unknown", {})]
        if unknown:
            return self._skip("own_unknown", f"своя позиция без записи ({', '.join(unknown)}) — проверьте её в OKX; пока она есть, бот не входит")
        # Позиции на счёте: свои (mine) проверяются раньше («own_same»), остальные —
        # владельца (руками): на их бумаги бот не входит.
        foreign = [str(p.get("instId")) for p in self.positions]
        if now - num(self.st["entered"].get(s["coin"])) < COIN_PAUSE_MS:
            return self._skip("coin_pause", "в эту бумагу входили меньше 3 часов назад")
        usd, against, count = self.sig.balance(s["coin"], s["side"], now)
        s = {**s, "usd": round(usd, 2), "usd_against": round(against, 2)}
        # Приоритетный кит (наш винрейт с ним > 90 % от 5 сделок): с ним входим
        # и в одиночку, без «против крупнее» и MA30.
        prio = self.sig.priority_in(s["coin"], s["side"], s.get("wallets"), now)
        if prio:
            s["priority"] = prio
        if (s["coin"], s["side"]) in self.sig.windows and count < MIN_WHALES and not prio:
            return self._skip("stale", f"китов за 2 часа до «сейчас» осталось {count} — сигнал устарел")
        if against > 0 and against >= usd and not prio:
            # Против сигнала — киты крупнее: в их сторону не идём (их сигнал,
            # если их от 2, — отдельный кандидат, он и войдёт).
            return self._skip("bigger_against", f"против сигнала киты крупнее: {'шорты' if s['side'] > 0 else 'лонги'} "
                                                f"{against:,.0f} $ против {usd:,.0f} $ — входим только с крупными".replace(",", " "))
        if AGAINST and not s.get("big"):
            # Против китов (толпы): дальше (цена входа, MA30, стоп, тейк, ордер) —
            # сторона сделки. Крупные против толпы (big) — идём с ними, не разворачиваем.
            s = {**s, "side": -s["side"], "fade": True}
        base, scale = core.okx_candidate(s["coin"])
        inst = self.inst_for(base)
        blocked = self.st.setdefault("blocked", {})
        b = blocked.get(inst)
        if isinstance(b, dict) and now - num(b.get("at")) < (BLOCK_MS if b.get("permanent") else FAIL_PAUSE_MS):
            return self._skip("blocked", f"{inst}: OKX не даёт торговать ({b.get('why', '')})")
        mb = self.st["market_block"].get(inst_type(inst))
        if isinstance(mb, dict) and now - num(mb.get("at")) < MARKET_PAUSE_MS:
            left = int((MARKET_PAUSE_MS - (now - num(mb.get("at")))) // 60_000) + 1
            return self._skip("market", f"ключу OKX закрыт рынок {inst_type(inst)} — проверка снова через {left} мин "
                                        f"({mb.get('why', '')})")
        if inst in mine:
            return self._skip("own_same", f"на {inst} у бота уже позиция — вторую на ту же бумагу не открываем")
        if inst in foreign:
            # Своя сделка бота на той же бумаге смешалась бы с ручной (в режиме
            # net — вычлась бы из неё), а тейк бота закрыл бы часть ручной.
            return self._skip("manual_same", f"на {inst} уже ваша позиция — бот на эту бумагу не входит")
        ins = self.instruments().get(inst)
        if not ins:
            return self._skip("no_inst", f"{inst} нет на OKX")
        last = self.okx.ticker(inst)
        if last <= 0:
            return self._skip("no_price", "нет цены OKX")
        hl_px = last * scale
        if abs(hl_px / s["ref_px"] - 1) > MATCH_TOL:
            return self._skip("other_asset", f"{inst} — другой актив (цена {last} против {s['ref_px'] / scale:.6g})")
        if s["side"] * (hl_px / s["ref_px"] - 1) > CHASE + PX_EPS:
            return self._skip("chase", f"цена {last:.6g} при средней китов {s['ref_px'] / scale:.6g} — вход только {chase_rule()}")
        # MA30 15м — по стороне сделки (правило владельца 28.11.2026): в шорт не
        # входим, когда цена выше MA30, в лонг — когда ниже.
        why = "" if prio else self.ma_check(inst, last, s["side"])
        if why:
            return why
        # Стоп — STOP_PCT от цены входа (уровни 4ч убраны 27.11.2026: правило владельца).
        s = {**s, "stop": core.fixed_stop(last, s["side"])}
        if self.equity <= 0:
            return self._skip("no_equity", "капитал счёта 0")
        notional = self.equity * MARGIN_FRAC * LEVERAGE
        # Параметры бумаги из каталога — только конечные положительные: мусор
        # в каталоге не должен дать ордер неверного размера.
        ct_val, ct_mult, tick = dec(ins.get("ctVal")), dec(ins.get("ctMult") or "1"), dec(ins.get("tickSz"))
        if ct_val is None or ct_mult is None or dec(ins.get("lotSz")) is None or dec(ins.get("minSz")) is None \
                or tick is None:
            return self._skip("bad_params", f"{inst}: неверные параметры бумаги в каталоге OKX")
        sz = contract_size(ins, notional, last)
        # Вход — по рынку, но не дальше CHASE от средней цены китов (правило
        # владельца 27.11.2026: «вход маркет ордером, не гонимся за ценой»;
        # против китов CHASE = −1 % — лучше их цены): ордер IOC с границей цены — исполняется
        # сразу по лучшим ценам стакана, как рыночный, но не хуже границы (лонг —
        # средняя китов × (1 + CHASE), шорт — × (1 − CHASE), на шаге цены внутрь);
        # остаток — снимается биржей. Простой рыночный ордер при проскальзывании ушёл бы дальше.
        lim = round_step(s["ref_px"] / scale * (1 + s["side"] * (CHASE + PX_EPS)), tick, ROUND_FLOOR if s["side"] > 0 else ROUND_CEILING)
        # Тейк должен встать на шаг цены не ближе TP и не дальше TP + 0,5 %: шаг
        # слишком крупный для этой цены — тейк невозможен, без выхода не входим.
        tp_try = round_step(last * (1 + s["side"] * TP), tick, ROUND_CEILING if s["side"] > 0 else ROUND_FLOOR)
        if lim <= 0 or tp_try <= 0 or not TP * 0.999 <= s["side"] * (float(tp_try) / last - 1) <= TP + TP_SLACK:
            return self._skip("tick", f"{inst}: шаг цены {fmt_dec(tick)} слишком крупный для тейка +{TP * 100:g} % при цене {last}")
        if sz <= 0:
            return self._skip("min_size", f"позиция {notional:.0f} $ меньше минимальной на OKX")
        cl = f"{TAG}in{now % 10**12}"
        try:
            if inst not in self.lever_set:
                self.okx.set_leverage(inst, LEVERAGE)
                self.lever_set.add(inst)
            # Сначала — запись «входим» (если служба упадёт посреди, при старте
            # ордер найдётся по clOrdId, а не будет повторён).
            self.st["entering"] = {"inst": inst, "cl": cl, "at": now, "coin": s["coin"], "side": s["side"],
                                   "whales": s["whales"], "ref_px": s["ref_px"] / scale, "signal_ts": s["ts"],
                                   "wallets": s.get("wallets") or {}, "usd": s.get("usd"), "usd_against": s.get("usd_against"),
                                   "stop": s.get("stop", 0.0), "limit": fmt_dec(lim), "last": last, "until": now + ENTRY_WAIT_MS,
                                   "ordered": fmt_dec(sz), **({"priority": s["priority"]} if s.get("priority") else {}),
                                   **({"fade": True} if s.get("fade") else {}),
                                   **({"big": True} if s.get("big") else {})}
            self.save()
            self.okx.order({"instId": inst, "tdMode": "cross", "side": "buy" if s["side"] > 0 else "sell",
                            "ordType": "ioc", "px": fmt_dec(lim), "sz": fmt_dec(sz), "clOrdId": cl, **self._ps(s["side"])})
        except OkxError as e:
            if self.st.get("entering") and not (e.code.isdigit() and len(e.code) == 5):
                # Ответ не от биржи (ошибка шлюза, обрыв, порча ответа): ордер
                # мог уйти. Запись «входим» остаётся — сверка найдёт его по метке
                # и примет исполнение или снимет; до тех пор новых входов нет.
                self.event(f"{inst}: ответ на ордер не получен — {e}; сверка по счёту")
                self.save()
                return self._skip("unconfirmed", f"{inst}: ответ на ордер не получен — сверка")
            # Ордер не принят биржей — позиции нет.
            self.st.pop("entering", None)
            why = str(e).split(" [")[0][:400]
            if e.code in MARKET_CODES:
                # Закрыто ключу, а не бумаге: пауза всего рынка, бумаги не исключаются.
                self.st["market_block"][inst_type(inst)] = {"at": now, "why": why}
                # Во вкладку — пока ордер на этом рынке не пройдёт (пауза рынка короче).
                self.st["key_problem"] = {"at": now, "why": why, "market": inst_type(inst)}
                self.event(f"{inst}: ордер не принят — {why}. Бумаги не исключаются: бот проверит рынок снова "
                           f"через {MARKET_PAUSE_MS // 60_000} минут")
                return self._skip("market", f"{inst}: ключу OKX закрыт рынок ({e.code})")
            # По самой бумаге — в исключённые на неделю, иначе — на 15 минут; сигналы — дальше.
            permanent = e.code in BLOCK_CODES
            blocked[inst] = {"at": now, "why": why, "permanent": permanent}
            self.event(f"{inst}: ордер не принят — {e}; "
                       + ("бумага исключена на 7 дней" if permanent else "пауза по бумаге 15 минут"))
            return self._skip("rejected", f"{inst}: ордер не принят ({e.code})")
        if (self.st.get("key_problem") or {}).get("market") == inst_type(inst):
            self.st.pop("key_problem", None)  # ордер на этом рынке принят — ключ в порядке
        self.event(f"вход {inst} {'лонг' if s['side'] > 0 else 'шорт'} по рынку {fmt_dec(sz)}, не хуже {fmt_dec(lim)} "
                   f"(вход {chase_rule()}; маржа {MARGIN_FRAC * 100:g} %, китов {s['whales']})")
        self.save()
        # Исполнился (обычно сразу) — позиция и тейк сейчас; иначе — на сверке.
        if self._resolve_entry(self.st["entering"]) == "miss":
            return self._skip("limit_miss", f"{inst}: цена ушла — вход только {chase_rule()}, не исполнился")
        return ""

    def _resolve_entry(self, e: dict[str, Any]) -> str:
        """Ордер входа (IOC с границей цены; у записи прежней версии — лимит):
        исполнен (или снят с частью) — позиция и тейк ('open'); висит и время
        не вышло — 'wait'; вышло — снять и решить по исполненному; снят без
        исполнения (цена ушла за границу) — входа нет ('miss'). Ордера не
        найти дольше 10 минут — запись снята ('miss')."""
        inst, cl = str(e.get("inst")), str(e.get("cl"))
        try:
            o = self.okx.get_order(inst, cl_id=cl)
            # Висит, а время вышло — снять остаток и решить по исполненному.
            if o.get("state") in ("live", "partially_filled") and self.clock() >= num(e.get("until")):
                with contextlib.suppress(OkxError):   # уже исполнен или снят — решит повторный запрос
                    self.okx.cancel(inst, str(o.get("ordId")))
                o = self.okx.get_order(inst, cl_id=cl)
                if o.get("state") in ("live", "partially_filled") and size_of(o.get("accFillSz")) > 0:
                    self._open_from_fill(e, o)   # снять не вышло — исполненное и есть вход
                    return "open"
        except OkxError as err:
            if err.code != "51603":   # 51603 «ордера нет» — как не найден
                self.event(f"{inst}: ордер входа не проверен — {err}; повтор на сверке")
                return "wait"
            o = {}
        state = o.get("state")   # висит (live, partially_filled) — ниже ничего не подходит: ждём
        if state in ("filled", "canceled", "mmp_canceled") and size_of(o.get("accFillSz")) > 0:
            self._open_from_fill(e, o)
            return "open"
        if state in ("canceled", "mmp_canceled") or (not state and self.clock() - num(e.get("at")) > 600_000):
            self.event(f"{inst}: ордер входа не исполнился ({state or 'ордера нет'}) — входа нет")
            self.st.pop("entering", None)
            return "miss"
        return "wait"

    def _open_from_fill(self, e: dict[str, Any], o: dict[str, Any]) -> None:
        """Ордер входа исполнен: запись позиции (цена и объём — с биржи, не
        больше заказанного; цена дальше 5 % от цены сделки на решении — порча:
        цена сделки; её нет (запись прежней версии) — граница ордера) и тейк
        на весь исполненный объём."""
        inst, side = str(e["inst"]), 1 if num(e.get("side")) > 0 else -1
        ins = self.instruments().get(inst, {})
        ref = num(e.get("last")) or num(e.get("limit"))
        avg = num(o.get("avgPx"))
        fill_px = avg if avg > 0 and abs(avg / ref - 1) <= FILL_TOL else ref
        ordered = size_of(e.get("ordered"))
        got = min(size_of(o.get("accFillSz")), ordered) if ordered > 0 else size_of(o.get("accFillSz"))
        tp = round_step(fill_px * (1 + side * TP), ins.get("tickSz", "0.0001"), ROUND_CEILING if side > 0 else ROUND_FLOOR)
        in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
        ct = num(ins.get("ctVal")) * num(ins.get("ctMult"), 1.0)
        now = self.clock()
        op = {"coin": e.get("coin") or inst.split("-", 1)[0], "inst": inst, "side": side,
              "opened": int(num(o.get("fillTime")) or num(o.get("uTime")) or now), "px": fill_px, "size": float(got),
              "notional": round(float(got) * ct * (1 if in_usd else fill_px), 2), "tp": float(tp), "tp_s": fmt_dec(tp),
              "whales": e.get("whales"), "ref_px": e.get("ref_px"), "signal_ts": e.get("signal_ts"),
              "in_id": str(o.get("ordId") or ""), "margin_pct": round(MARGIN_FRAC * 100, 2),
              "wallets": e.get("wallets") if isinstance(e.get("wallets"), dict) else {},
              "usd": e.get("usd"), "usd_against": e.get("usd_against"), "stop": num(e.get("stop")),
              **({"priority": e["priority"]} if isinstance(e.get("priority"), str) else {}),
              **({"fade": True} if e.get("fade") else {}), **({"big": True} if e.get("big") else {})}
        self.st["opens"] = [*(x for x in self.st["opens"] if x["inst"] != inst), op]
        self.st["entered"][op["coin"]] = now
        self.st.pop("entering", None)
        prio = op.get("priority")
        self.event(f"вход {inst} {'лонг' if side > 0 else 'шорт'} {fmt_dec(got)} по {fill_px} "
                   f"(≈{op['notional']:.0f} $, маржа {MARGIN_FRAC * 100:g} %, китов {op['whales']}), тейк {op['tp_s']}"
                   + (f"; приоритетный кит {prio[:10]}… (наш винрейт с ним "
                      f"{(self.rating.rate(prio) or 0) * 100:.0f} %)" if prio else ""))
        self._place_tp(op, got)
        self.save()

    def _side_of(self, p: dict[str, Any]) -> int:
        ps = str(p.get("posSide") or "net")
        if ps in ("long", "short"):
            return 1 if ps == "long" else -1
        return 1 if num(p.get("pos")) > 0 else -1

    def adopt(self, p: dict[str, Any], order: dict[str, Any], info: dict[str, Any] | None = None) -> bool:
        """Позиция открыта нашим ордером, но не записана (служба упала между
        ордером и записью): принять как свою и поставить тейк."""
        inst = str(p["instId"])
        side = self._side_of(p)
        ins = self.instruments().get(inst, {})
        px = num(p.get("avgPx")) or num(order.get("avgPx"))
        mark = num(p.get("markPx")) or self.okx.ticker(inst)
        if not (px > 0 and mark > 0 and abs(px / mark - 1) <= ADOPT_TOL):
            self.event(f"{inst}: своя позиция без записи, но цена входа {px} не сходится с рынком {mark} — "
                       "тейк не ставлю, проверьте позицию в OKX")
            # Своя, но не принята: пока она есть — новых входов нет (иначе у бота
            # было бы две позиции, а эту он считал бы ручной).
            self.st.setdefault("own_unknown", {})[inst] = self.clock()
            return False
        size_d = size_of(p.get("pos"))
        size = float(size_d)
        tp = round_step(px * (1 + side * TP), ins.get("tickSz", "0.0001"), ROUND_CEILING if side > 0 else ROUND_FLOOR)
        in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
        ct = num(ins.get("ctVal")) * num(ins.get("ctMult"), 1.0)
        info = info or {}
        opened = int(num(order.get("fillTime")) or num(order.get("cTime")) or num(info.get("at")) or self.clock())
        coin = info.get("coin") or inst.split("-", 1)[0]
        op: dict[str, Any] = {"coin": coin, "inst": inst, "side": side, "opened": opened, "px": px, "size": size,
              "notional": round(size * ct * (1 if in_usd else px), 2), "tp": float(tp), "tp_s": fmt_dec(tp),
              "whales": info.get("whales"), "ref_px": info.get("ref_px"), "signal_ts": info.get("signal_ts"),
              "in_id": str(order.get("ordId") or ""), "adopted": True, "stop": num(info.get("stop")),
              "wallets": info.get("wallets") if isinstance(info.get("wallets"), dict) else {},
              **({"fade": True} if info.get("fade") else {}), **({"big": True} if info.get("big") else {})}
        self.st["opens"] = [*(o for o in self.st["opens"] if o["inst"] != inst), op]
        self.st["entered"][coin] = opened
        self.event(f"найдена своя позиция без записи: {inst} {'лонг' if side > 0 else 'шорт'} {fmt_dec(Decimal(str(size)))} "
                   f"по {px} — принята, тейк {op['tp_s']}")
        pend = [o for o in self.okx.pending(inst) if str(o.get("clOrdId", "")).startswith(TAG)]
        if pend:  # тейк уже стоит (упали после него) — не дублировать
            op["tp_id"] = str(pend[0].get("ordId"))
        else:
            self._place_tp(op, size_d)
        self.save()
        return True

    def find_own(self) -> None:
        """Позиции на счёте, которые бот не ведёт: открыты нашим рыночным
        ордером (метка clOrdId «wtin…» за последние 7 дней) — свои,
        принять. Нет такой метки — чужие (владельца): не трогать. Каждую
        бумагу — не чаще раза в час."""
        if len(self.st["opens"]) >= MAX_OPEN:
            return   # больше своих быть не может: остальные — владельца
        mine = {o["inst"] for o in self.st["opens"]}
        e = self.st.get("entering")
        waiting = str(e.get("inst")) if isinstance(e, dict) and e.get("limit") else ""
        checked = self._checked
        held = {str(p.get("instId") or "") for p in self.positions}
        self.unverified &= held
        for p in self.positions:
            inst = str(p.get("instId") or "")
            # Бумага висящего ордера входа — его решит _resolve_entry (позиция
            # частью исполненного ордера — ещё не вся: принять её рано).
            if not inst or inst in mine or inst == waiting or time.time() - checked.get(inst, 0) < 3600:
                continue
            side = self._side_of(p)
            try:
                hist = self.okx.history(inst)
            except OkxError as e:
                # Не проверено — не «ручная»: пока не проверим, новых входов нет
                # (иначе своя непринятая позиция стала бы второй позицией бота).
                self.unverified.add(inst)
                self.event(f"{inst}: история ордеров недоступна — {e}")
                continue
            checked[inst] = time.time()
            self.unverified.discard(inst)
            tagged = [o for o in hist
                      if str(o.get("clOrdId", "")).startswith(f"{TAG}in") and o.get("state") in ("filled", "partially_filled")
                      and (1 if o.get("side") == "buy" else -1) == side
                      and self.clock() - num(o.get("cTime")) < 7 * 86_400_000
                      and str(o.get("ordId") or "") not in self.st["closed_ids"]]
            if tagged:
                info = self.st.get("entering") if (self.st.get("entering") or {}).get("inst") == inst else None
                self.adopt(p, tagged[0], info)
                self.st.pop("entering", None)
                return

    def step(self) -> None:
        new = self.sig.poll(self.clock())
        if time.time() - self.last_sync >= SYNC_SEC or new:
            self.sync()
        # Сигналы, пропущенные из-за сбоя свечей, — снова, пока свежие (15
        # минут): сигнал рождается только на сделке кита и сам не повторится.
        # Первыми в списке — новый сигнал той же бумаги и стороны важнее.
        sigs, self.retry = [*self.retry, *new], []
        if not self.is_on():
            self.save()
            return
        for s in self.candidates(sigs):
            self.skip_code = "other"
            why = self.try_enter(s)
            if self.skip_code == "candles":
                self.retry.append(s)
            if not why:
                continue   # вошли — следующие сигналы опроса тоже (до MAX_OPEN позиций)
            self.st["last_skip"] = {"coin": s["coin"], "side": s["side"], "why": why, "at": self.clock(),
                                    "code": self.skip_code}
            self.count_skip(s, self.skip_code)
        self.save()


def mtimes(paths: list[str]) -> list[float]:
    return [os.path.getmtime(p) if os.path.exists(p) else 0.0 for p in paths]


def code_updated(me: list[str], stamp: list[float], bts: Backtests, tr: Trader) -> bool:
    """Код обновился (автодеплой) — True: выйти, systemd поднимет с новым.
    Идущий пересчёт — остановить и заказать заново (новая служба начнёт его
    новым кодом); раньше служба ждала его до 6 часов — торговля и бэктест
    шли старым кодом."""
    if mtimes(me) == stamp:
        return False
    if bts.proc is not None:
        bts.restart_for_update()
    tr.event("код обновился — перезапуск")
    tr.save()
    return True


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Автокопирование китов — реальная торговля на OKX")
    ap.add_argument("--db", default=os.environ.get("WHALE_HL_DB_FILE", "hyperliquid.db"), help="база бота (только чтение)")
    ap.add_argument("--dir", default=os.environ.get("WT_AUTOTRADE_DIR")
                    or os.path.expanduser("~/WalletTracker/autotrade_data"), help="каталог модуля")
    a = ap.parse_args(argv)
    os.umask(0o077)  # состояние и журнал — только владельцу службы (позиции, капитал)
    live_dir = os.path.join(a.dir, "live")
    with contextlib.suppress(OSError):  # файлы прежних версий — тоже только владельцу
        for e in os.scandir(live_dir):
            if e.is_file(follow_symlinks=False):
                os.chmod(e.path, 0o600)
    key, secret, pw = (os.environ.get(k, "") for k in ("OKX_API_KEY", "OKX_API_SECRET", "OKX_API_PASSPHRASE"))
    os.makedirs(os.path.join(a.dir, "live"), exist_ok=True)
    if not (key and secret and pw):
        msg = "нет ключа OKX (OKX_API_KEY, OKX_API_SECRET, OKX_API_PASSPHRASE в /etc/whale-autotrade.env)"
        tmp = os.path.join(a.dir, "live", "state.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"error": msg, "heartbeat": now_ms(), "open": None, "opens": [], "entered": {}, "events": []}, f,
                      ensure_ascii=False)
        os.replace(tmp, os.path.join(a.dir, "live", "state.json"))
        print(f"[AUTO-LIVE] {msg}", flush=True)
        time.sleep(300)  # служба перезапустится — проверит снова
        return 1
    okx = Okx(key, secret, pw, os.environ.get("OKX_SIMULATED") == "1")
    del key, secret, pw
    tr = Trader(okx, Signals(a.db), a.dir)
    try:
        base = okx.detect(os.environ.get("OKX_API_BASE", "") or str(tr.st.get("base") or ""))
        tr.st["base"] = base
        tr.event(f"ключ OKX принят: {base}")
        cfg = okx.config()
        lv_ = str(cfg.get("acctLv") or "?")
        modes = {"1": "Простой (Spot) — фьючерсы по API недоступны", "2": "Спот и фьючерсы",
                 "3": "Мультивалютная маржа", "4": "Портфельная маржа"}
        tr.event(f"счёт OKX: режим {lv_} — {modes.get(lv_, 'неизвестный')}; позиции: {cfg.get('posMode')}; "
                 f"уровень доступа: {cfg.get('level') or '?'}; права ключа: {cfg.get('perm') or '?'}")
        held = okx.positions()
        tr.event("открытые позиции на счёте: " + (", ".join(f"{p.get('instId')} ({p.get('instType', '?')})"
                                                            for p in held) or "нет"))
    except (OkxError, OSError) as e:
        tr.st["error"] = str(e)[:300]
        tr.event(f"ключ OKX не принят: {e}")
        tr.save()
        time.sleep(300)  # служба перезапустится и попробует снова
        return 1
    # Код обновился (и торговли, и бэктеста) — перезапуск службы.
    me = [os.path.abspath(__file__), core.__file__, os.path.join(os.path.dirname(os.path.abspath(__file__)), "autotrade_replay.py")]
    stamp = mtimes(me)
    # Вход, начатый до перезапуска и не записанный, разберёт первая сверка шага
    # (resolve_entering; своя позиция без записи — find_own по метке ордера).
    tr.event(f"старт ({'демо-счёт' if okx.simulated else 'РЕАЛЬНЫЙ счёт'})")
    bts = Backtests(a.dir, a.db)
    jan = Janitor(a.dir)
    fails = 0
    while True:
        if jan.due(time.time()):
            try:
                if jan.run(tr, bts, time.time()):
                    tr.event(f"служба заняла {tr.st['janitor']['rss_mb']:.0f} МБ (больше {RSS_MAX_MB}) — перезапуск")
                    tr.save()
                    return 0
            except Exception as e:  # noqa: BLE001 — очистка не должна ронять торговлю
                tr.event(f"очистка: {e}")
        try:
            bts.tick(tr.is_on())
        except Exception as e:  # noqa: BLE001 — сбой бэктеста не должен ронять торговлю
            tr.event(f"бэктест: {e}")
        try:
            tr.step()
            tr.st["error"] = ""
            fails = 0
        except Exception as e:  # noqa: BLE001 — шаг не должен ронять службу: сбой — в журнал и пауза
            fails += 1
            tr.st["error"] = str(e)[:300]
            tr.event(f"сбой: {e}")
            with contextlib.suppress(OSError):
                tr.save()
            time.sleep(min(300, 5 * 2 ** min(fails, 6)))
        if code_updated(me, stamp, bts, tr):
            return 0
        time.sleep(POLL_SEC)


if __name__ == "__main__":
    sys.exit(main())
