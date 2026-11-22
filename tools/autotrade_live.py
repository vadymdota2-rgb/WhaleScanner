#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Автокопирование китов — РЕАЛЬНАЯ торговля на OKX (этап 3, по команде владельца).

Отдельная служба (whale-autotrade.service): ключи OKX есть только у неё, API
и бот их не видят. Ключи — в /etc/whale-autotrade.env (root, 600), systemd
передаёт их через окружение:

    OKX_API_KEY, OKX_API_SECRET, OKX_API_PASSPHRASE   — ключ OKX (права: только торговля)
    OKX_SIMULATED=1                                    — демо-счёт OKX (по умолчанию 0 — реальный)

Правила владельца:
- кит — только кошелёк из топ-1000 по винрейту за 30 дней (та же доска,
  что «Топ трейдеров» в приложении: от 5 до 200 закрытий, без забаненных;
  список — раз в 15 минут); сделки остальных не считаются ни за, ни против;
- сигнал — от 1 кита Hyperliquid в одну сторону на одной бумаге за 2 часа,
  от 1000 $ (сделки китов — из базы бота, только чтение);
- только фьючерсы (крипта, акции, металлы), бумага должна быть на OKX и цена
  сходиться с ценой китов (±2 %) — иначе это другой актив;
- не догоняем: цена OKX хуже средней цены китов больше чем на 0,3 % — пропуск;
- маржа — всегда 5 % капитала счёта, плечо 10×, кросс (сделки владельца
  руками на счёте входу не мешают);
  на ту же бумагу, где ручная позиция, — не входит;
- у бота своя позиция — всегда одна: пока её не закрыли, вторую не открываем;
- выход — тейк +1 % движения цены (лимитный ордер на бирже); и второй
  выход: кто-то из китов сигнала закрыл свою сделку по этой бумаге В УБЫТКЕ
  (стоп, ликвидация или он увидел, что ситуация изменилась) — бот снимает
  тейк и закрывает свою позицию рыночным ордером (только на закрытие). Кит
  закрыл в плюс — сидим до тейка. Кит уже вышел в убыток до входа — не входим;
- разовое усреднение: позиция бота в минусе больше 1 % (цена против входа)
  и кит сигнала добирает позицию после нашего входа (от 1000 $) — один раз
  доливаем ещё 5 % капитала × 10× рыночным; средняя цена и тейк (+1 % от
  новой средней) — на весь объём; при «Стоп» не усредняемся;
- в одну и ту же бумагу — не чаще раза в 3 часа;
- входим с крупными: за те же 2 часа по бумаге против сигнала стоят киты
  крупнее по объёму (кошельки от 1000 $) — в слабую сторону не входим, а
  входим в их сторону.

Кнопки «Старт»/«Стоп» во вкладке пишут switch.json: «Стоп» — новых входов
нет; открытая позиция и её тейк остаются, выход вслед за китом — тоже.

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

OKX = "https://www.okx.com"
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
MIN_WHALES = 1              # от 1 кита (правило владельца, 22.11.2026; было от 2)
TOP_TTL_MS = 15 * 60_000    # список китов (топ по винрейту) — пересчёт раз в 15 минут
MIN_WHALE_USD = 1000
WINDOW_MS = 2 * 3_600_000
CHASE = 0.003
MATCH_TOL = 0.02
# Маржа входа — всегда 5 % капитала (правило владельца 22.11.2026; раньше —
# 10 %, а при ручной сделке на счёте — 5 %). Своя позиция у бота — одна.
MARGIN_FRAC = 0.05
# Разовое усреднение: позиция в минусе больше 1 % (цена против входа) и кит
# сигнала добирает позицию после нашего входа (от 1000 $) — один раз доливаем
# ещё 5 % капитала × 10×; средняя и тейк (+1 % от новой средней) — на весь объём.
AVG_LOSS = 0.01
AVG_FRAC = 0.05
LEVERAGE = 10
TP = 0.01
COIN_PAUSE_MS = 3 * 3_600_000
FILL_TOL = 0.05            # цена исполнения рыночного входа — не дальше 5 % от тикера
ADOPT_TOL = 0.5            # подхват позиции: средняя цена — не дальше 50 % от рынка
TP_SLACK = 0.005           # тейк на шаге цены — не дальше +1,5 % (иначе шаг цены слишком крупный)

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
                 transport: Transport | None = None, base: str = OKX) -> None:
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
        # Только объекты: строка или число в списке ответа — порча, не запись.
        return [x for x in data if isinstance(x, dict)] if isinstance(data, list) else []

    # Обёртки — только то, что нужно трейдеру.
    def instruments(self, inst_type: str = "SWAP") -> list[dict[str, Any]]:
        return self.call("GET", "/api/v5/public/instruments", {"instType": inst_type}, private=False)

    def ticker(self, inst: str) -> float:
        d = self.call("GET", "/api/v5/market/ticker", {"instId": inst}, private=False)
        return max(0.0, num(d[0].get("last"))) if d and isinstance(d[0], dict) else 0.0

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

    def __init__(self, db: str) -> None:
        self.db = db
        self.windows: dict[tuple[str, int], core.Window] = {}
        self.seen: dict[int, int] = {}   # tid → ts (против повторов при перекрытии опроса)
        self.limit = MIN_WHALE_USD * NANOS   # кит — кошелёк от 1000 $ в окне
        # Киты — только топ-1000 по винрейту за 30 дней (как доска «Топ
        # трейдеров»); None — список ещё не загружен (тогда фильтра нет).
        self.top: set[str] | None = None
        self.top_at = -10**15

    def refresh_top(self, now: int, ttl: int = TOP_TTL_MS) -> None:
        """Список китов на момент now — не чаще раза в ttl. Сбой базы —
        исключение (ошибка шага), старый список остаётся, повтор — на
        следующем опросе."""
        if now - self.top_at < ttl:
            return
        con = self._con()
        try:
            self.top = core.top_winrate(con, now)
        finally:
            con.close()
        self.top_at = now

    def _con(self) -> sqlite3.Connection:
        return core.open_ro(self.db)  # нет базы — OSError: ошибка шага, а не выход службы

    def poll(self, now: int) -> list[dict[str, Any]]:
        """Новые строки базы → окна; сигналы: бумага и сторона, где сейчас
        есть кит (топ по винрейту) от 1000 $ за 2 часа, и строка, на которой
        это случилось."""
        # Всё окно 2 часа каждый раз: бот записывает сделки китов с опозданием
        # (опрос кошельков), и опоздавшая сделка должна попасть в окно, а не
        # потеряться. Повторы отсекает seen (tid).
        since = now - WINDOW_MS
        self.refresh_top(now)
        con = self._con()
        try:
            rows = con.execute(
                "SELECT tid, wallet, coin, dir_code, ts, notional_nanos, ABS(CAST(sz AS REAL)) FROM hl_fills "
                "WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ? ORDER BY ts, tid", (since, now)).fetchall()
        finally:
            con.close()
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
            # Не из топа по винрейту — не кит, его сделки не считаем вовсе
            # (ни за, ни против). seen не ставим: попадёт в топ — учтём.
            if tid in self.seen or not core.is_perp(coin) or (self.top is not None and wallet not in self.top):
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
            if w.count >= MIN_WHALES and w.big_u > 0:
                out.append(self._signal(coin, side, w, opp, newest))
            # Против этих китов стоят киты крупнее (по объёму) —
            # сигнал и в их сторону: входим с крупными (правило владельца).
            if opp is not None and opp.count >= MIN_WHALES and opp.big_u > 0 \
                    and self.whale_usd(opp) > self.whale_usd(w):
                out.append(self._signal(coin, -side, opp, w, newest))
        return out

    def whale_usd(self, w: core.Window | None) -> float:
        """Объём китов окна (кошельки от 1000 $), $."""
        if w is None:
            return 0.0
        return float(sum(v[0] for v in w.per.values() if v[2] > 0 and v[0] >= self.limit)) / NANOS

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
        return {"coin": coin, "side": side, "ts": ts, "whales": w.count, "wallets": first,
                "ref_px": (w.big_n / NANOS) / w.big_u, "usd": round(self.whale_usd(w), 2),
                "usd_against": round(self.whale_usd(opp), 2)}

    def whale_loss(self, coin: str, side: int, wallets: dict[str, Any], now: int) -> list[tuple[str, float]]:
        """Киты сигнала, закрывшие позицию по этой бумаге и стороне в убыток:
        закрывающие сделки (закрытие, разворот, ликвидация) после их первой
        сделки в окне сигнала и до «сейчас», итог closed_pnl < 0. Кит, у
        которого закрытия в сумме в плюс, — не в счёт. → [(кошелёк, $)]."""
        since = {str(k): int(num(v)) for k, v in wallets.items() if isinstance(k, str)}
        if not since:
            return []
        codes = core.CLOSES_FOR[1 if side > 0 else -1]
        # Разворот (код 5) — в обе стороны одним кодом: «Long > Short» закрывает
        # лонг, «Short > Long» — шорт. Разворот В нашу сторону (кит закрыл
        # встречный шорт в минус и перешёл в лонг вместе с нами) — не выход.
        flip = "%Long > Short%" if side > 0 else "%Short > Long%"
        con = self._con()
        try:
            rows = con.execute(
                f"SELECT wallet, ts, closed_pnl_nanos FROM hl_fills WHERE coin = ? AND wallet IN ({','.join('?' * len(since))}) "  # noqa: S608 — только «?»
                f"AND dir_code IN ({','.join('?' * len(codes))}) AND ts >= ? AND ts <= ? "
                f"AND (dir_code != {core.FLIP} OR dir LIKE ?)",
                (coin, *since, *codes, min(since.values()), now, flip)).fetchall()
        finally:
            con.close()
        pnl: dict[str, int] = {}
        for wl, ts, p in rows:
            if ts >= since.get(wl, now + 1):
                pnl[wl] = pnl.get(wl, 0) + int(p or 0)
        return [(wl, p / NANOS) for wl, p in sorted(pnl.items()) if p < 0]

    def whale_adds(self, coin: str, side: int, wallets: dict[str, Any], since: int, now: int) -> list[tuple[str, float]]:
        """Киты сигнала, добравшие позицию по этой бумаге и стороне после
        since (нашего входа): сумма их сделок открытия, $ — по убыванию."""
        ws = [str(k) for k in wallets if isinstance(k, str)]
        if not ws:
            return []
        con = self._con()
        try:
            rows = con.execute(
                f"SELECT wallet, SUM(notional_nanos) FROM hl_fills WHERE coin = ? AND wallet IN ({','.join('?' * len(ws))}) "  # noqa: S608 — только «?»
                "AND dir_code = ? AND ts > ? AND ts <= ? GROUP BY wallet",
                (coin, *ws, core.OPEN_LONG if side > 0 else core.OPEN_SHORT, since, now)).fetchall()
        finally:
            con.close()
        return sorted(((str(w), int(n or 0) / NANOS) for w, n in rows), key=lambda x: -x[1])

    def wallets_at(self, coin: str, side: int, ts: int) -> dict[str, int]:
        """Киты сигнала по базе: кто от 1000 $ открывал эту бумагу и сторону
        за 2 часа до сигнала, и их первая сделка в окне. Для позиции, открытой
        до того, как бот стал запоминать китов."""
        con = self._con()
        try:
            rows = con.execute(
                "SELECT wallet, MIN(ts), SUM(notional_nanos) FROM hl_fills WHERE coin = ? AND dir_code = ? "
                "AND ts >= ? AND ts <= ? GROUP BY wallet",
                (coin, core.OPEN_LONG if side > 0 else core.OPEN_SHORT, ts - WINDOW_MS, ts)).fetchall()
        finally:
            con.close()
        return {str(w): int(t) for w, t, n in rows
                if int(n or 0) >= self.limit and (self.top is None or str(w) in self.top)}

    def sweep(self, now: int) -> int:
        """Очистка: окна бумаг, по которым давно не было сделок, сами не
        пустеют (выбрасывают старое только на новой сделке) — вычистить
        старше 2 часов и убрать пустые. Сколько окон убрано."""
        before = len(self.windows)
        for w in self.windows.values():
            w.expire(now - WINDOW_MS)
        self.windows = {k: w for k, w in self.windows.items() if w.orders}
        return before - len(self.windows)


# ── бэктест: ночью и по кнопке, в этой службе (не в API) ─────────────────────

BT_HOUR_UTC, BT_MINUTE = 3, 17
# Бэктест — повтор по правилам живого бота (autotrade_replay.py): свечи OKX
# (публичное API) только под кандидатов и сделки. Раньше — сетка прежнего
# бэктеста с паузой 15 с к Hyperliquid: часы без видимого хода («завис»).
BT_PAUSE_SEC = 0.15               # между запросами к OKX (публичное API: до 20 в 2 с)
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
        text += f", сделок {int(num(p.get('trades')))}, запросов к OKX {int(num(p.get('requests')))}"
        if text != self.st.get("progress"):
            self.st["progress"] = text[:200]
            self._write()

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
        dropped = tr.sig.sweep(int(now * 1000))
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
        freed = 0
        if not busy:
            self.rotate(os.path.join(self.dir, "last_run.log"))
            freed = self.clean_cache(os.path.join(self.dir, "cache"), now)
        gc.collect()
        mb = self.rss()
        tr.st["janitor"] = {"at": int(now * 1000), "rss_mb": round(mb, 1), "windows": len(tr.sig.windows),
                            "dropped": dropped, "cache_freed_mb": round(freed / 2**20, 1)}
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
        """Кеш свечей и фандинга бэктеста: старше 30 дней — удалить; всё
        равно больше 500 МБ — удалять самые старые. Сколько байт освобождено."""
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
        self.pos_mode = ""
        self.lever_set: set[str] = set()
        self.positions: list[dict[str, Any]] = []
        self.equity = 0.0
        self.last_sync = 0.0
        self.unverified: set[str] = set()   # позиции, которые не удалось проверить по истории ордеров
        self._checked: dict[str, float] = {}
        self.skip_code = "other"
        self._skip_seen: set[tuple[str, int, str, int]] = set()

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
        # Прежняя версия исключала бумаги на неделю за 50124 (а это запрет ключу,
        # не бумаге): такие исключения снять — BTC, ETH и др. снова доступны.
        st["blocked"] = {i: b for i, b in st["blocked"].items()
                         if not (isinstance(b, dict) and "50124" in str(b.get("why", "")))}
        st["events"] = [e for e in st["events"] if isinstance(e, str)]
        op = st.get("open")
        if op is not None and not (isinstance(op, dict) and all(k in op for k in OPEN_KEYS)
                                   and op["side"] in (1, -1) and isinstance(op["inst"], str)
                                   and num(op["px"]) > 0 and num(op["tp"]) > 0 and num(op["size"]) > 0):
            # Запись позиции испорчена: забыть её — find_own узнает свою позицию по метке ордера.
            self._log(f"[AUTO-LIVE] запись позиции в state.json испорчена — сверка по счёту: {str(op)[:200]}")
            op = None
        if op is not None and not isinstance(op.get("exit", {}), dict):
            op.pop("exit")
        st["open"] = op
        if not isinstance(st.get("entering"), dict):
            st.pop("entering", None)
        st["error"] = st["error"] if isinstance(st.get("error"), str) else ""
        return st

    def save(self) -> None:
        # Во вкладку (wt_autotrade.live) — только то, что она читает.
        self.st.update(heartbeat=self.clock(), simulated=self.okx.simulated, equity=self.equity,
                       positions=[{k: p.get(k) for k in ("instId", "pos", "avgPx", "upl")} for p in self.positions],
                       top_n=len(self.sig.top) if self.sig.top is not None else None)
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
        return getattr(self, "xperp", {}).get(base) or f"{base}-USDT-SWAP"

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
        self.average_down()

    def resolve_entering(self) -> None:
        """Вход начат (ордер, возможно, ушёл), но не записан: сбой между
        рыночным ордером и записью сделки. Пока не ясно, исполнился ли он, —
        новых входов нет (иначе второй рыночный ордер). Ордер найден по метке
        и исполнен, позиция есть — она своя: принять и поставить тейк; ордер
        отменён или за 10 минут не подтвердился позицией — входа не было."""
        e = self.st.get("entering")
        if not isinstance(e, dict) or self.st.get("open"):
            self.st.pop("entering", None)
            return
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
        """Наша позиция: есть — тейк на месте? нет — закрылась (тейк или руками):
        записать сделку."""
        op = self.st.get("open")
        if not op:
            return
        # Своя позиция — та же бумага и та же сторона (в режиме long/short на
        # бумаге может быть и встречная ручная позиция владельца).
        live = [p for p in self.positions if p.get("instId") == op["inst"] and self._side_of(p) == op["side"]]
        if live:
            p = live[0]
            op.update(mark=num(p.get("markPx")), upl=num(p.get("upl")))
            if op.get("exit"):
                return  # выходим вслед за китом — тейк не ставить; закрытие повторит follow_whales
            if op.get("in_done") is False:
                self._entry_fill(op)
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

    def _entry_fill(self, op: dict[str, Any]) -> None:
        """Рыночный вход исполнился частично, а потом доисполнился: запись
        позиции (объём, средняя цена, номинал) и тейк — на весь исполненный
        объём, не на первую часть. Тейк ещё не тронут — снять и поставить
        заново (ниже в _track_open) по новой средней цене; тейк уже начал
        исполняться — не трогать, сказать в журнал."""
        o = self.okx.get_order(op["inst"], str(op.get("in_id") or ""))
        got = size_of(o.get("accFillSz"))
        if got > size_of(op.get("size")):
            got = min(got, size_of(op.get("ordered") or op.get("size")) or got)  # не больше заказанного
            ins = self.instruments().get(op["inst"], {})
            tp_o = self.okx.get_order(op["inst"], str(op["tp_id"])) if op.get("tp_id") else {}
            if op.get("tp_id") and not (tp_o.get("state") == "live" and size_of(tp_o.get("accFillSz")) <= 0):
                self.event(f"{op['inst']}: вход доисполнился до {fmt_dec(got)}, но тейк уже исполняется — "
                           "остаток без тейка (выход вслед за китом работает)")
            else:
                avg = num(o.get("avgPx"))
                if avg > 0 and abs(avg / op["px"] - 1) <= FILL_TOL:
                    op["px"] = avg
                    tp = round_step(avg * (1 + op["side"] * TP), ins.get("tickSz", "0.0001"),
                                    ROUND_CEILING if op["side"] > 0 else ROUND_FLOOR)
                    if tp > 0:
                        op["tp"], op["tp_s"] = float(tp), fmt_dec(tp)
                if op.get("tp_id"):
                    self.okx.cancel(op["inst"], str(op["tp_id"]))
                    op.pop("tp_id")  # _track_open поставит тейк заново на весь объём
                in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
                ct = num(ins.get("ctVal")) * num(ins.get("ctMult"), 1.0)
                op["size"] = float(got)
                op["notional"] = round(float(got) * ct * (1 if in_usd else op["px"]), 2)
                self.event(f"{op['inst']}: вход доисполнился — {fmt_dec(got)} по {op['px']}, тейк {op['tp_s']} "
                           "на весь объём")
        if o.get("state") in ("filled", "canceled", "mmp_canceled"):
            op["in_done"] = True

    def _record_close(self, op: dict[str, Any], h: dict[str, Any]) -> None:
        """Сделка закрыта: запись — один раз (сначала «позиции нет» в состоянии,
        потом снятие остатка тейка: сбой снятия не повторит запись)."""
        pnl = num(h.get("realizedPnl"), num(h.get("pnl")))
        out_px = num(h.get("closeAvgPx"))
        trade = {"coin": op["coin"], "inst": op["inst"], "side": op["side"], "opened": op["opened"],
                 "closed": int(num(h.get("uTime"), self.clock())) or self.clock(), "px_in": op["px"], "px_out": out_px,
                 "tp": op["tp"], "size": op["size"], "pnl": round(pnl, 4), "fee": num(h.get("fee")),
                 "funding": num(h.get("fundingFee")), "notional": op["notional"],
                 "why": "тейк" if out_px > 0 and abs(out_px / op["tp"] - 1) < 0.002
                 else "кит вышел в убыток" if op.get("exit") else "закрыта"}
        with open(os.path.join(self.dir, "trades.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps(trade, ensure_ascii=False) + "\n")
        self.st["open"] = None
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

    # выход вслед за китом
    def follow_whales(self) -> None:
        """Кит сигнала закрыл позицию в убыток — закрыть свою: снять тейк и
        рыночным ордером только на закрытие. Не вышло (сбой, отказ) — запись
        «выходим» остаётся, тейк заново не ставится, попытка — раз в минуту."""
        op = self.st.get("open")
        if op and "wallets" not in op and num(op.get("signal_ts")) > 0:
            # Позиция открыта прежней версией (без китов в записи) — найти их по базе.
            op["wallets"] = self.sig.wallets_at(op["coin"], op["side"], int(num(op["signal_ts"])))
            self.event(f"{op['inst']}: киты сигнала восстановлены по базе — {len(op['wallets'])}; "
                       "выход вслед за ними в убытке включён")
        if not op or not isinstance(op.get("wallets"), dict) or not op["wallets"]:
            return
        now = self.clock()
        if not op.get("exit"):
            lost = self.sig.whale_loss(op["coin"], op["side"], op["wallets"], now)
            if not lost:
                return
            wl, usd = lost[0]
            op["exit"] = {"at": now, "tried": 0, "why": f"кит {wl[:10]}… закрыл в убыток {usd:+.0f} $"}
            self.event(f"{op['inst']}: {op['exit']['why']} — закрываю позицию бота по рынку")
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
            self.event(f"{op['inst']}: закрыть вслед за китом не вышло — {e}; повтор через минуту")
            return
        if got is not None and got[0] >= size_of(op.get("size")):
            self._record_close(op, got[1])

    def _exit_fills(self, op: dict[str, Any]) -> tuple[Decimal, dict[str, Any]] | None:
        """Ордера выхода вслед за китом: сколько исполнено и итог для записи
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

    # разовое усреднение
    def average_down(self) -> None:
        """Позиция в минусе больше 1 % (цена против нашей средней) и кит
        сигнала добирает позицию после нашего входа (от 1000 $) — один раз
        доливаем 5 % капитала × 10× рыночным. Запись «усредняемся» с меткой
        ордера — до отправки: ответ не дошёл или служба упала — ордер найдётся
        по метке, второго не будет. Не при «Стоп», не во время выхода вслед за
        китом и не пока сам вход доисполняется."""
        op = self.st.get("open")
        if not op or op.get("exit") or op.get("in_done") is False:
            return
        if isinstance(op.get("avg"), dict):
            if not op["avg"].get("done"):
                self._avg_fill(op)
            return
        if not self.is_on() or not isinstance(op.get("wallets"), dict) or not op["wallets"]:
            return
        live = [p for p in self.positions if p.get("instId") == op["inst"] and self._side_of(p) == op["side"]]
        if not live:
            return
        mark = num(live[0].get("markPx")) or self.okx.ticker(op["inst"])
        loss = op["side"] * (mark / op["px"] - 1) if mark > 0 and num(op.get("px")) > 0 else 0.0
        if loss >= -AVG_LOSS:
            return
        now = self.clock()
        adds = [(w, u) for w, u in self.sig.whale_adds(op["coin"], op["side"], op["wallets"], int(num(op["opened"])), now)
                if u >= MIN_WHALE_USD]
        if not adds:
            return
        why = f"кит {adds[0][0][:10]}… добрал {adds[0][1]:,.0f} $ при минусе {loss * 100:.2f} %".replace(",", " ")
        ins = self.instruments().get(op["inst"], {})
        ct_val, ct_mult = dec(ins.get("ctVal")), dec(ins.get("ctMult") or "1")
        step, min_sz = dec(ins.get("lotSz")), dec(ins.get("minSz"))
        sz = Decimal(0)
        if ct_val is not None and ct_mult is not None and step is not None and min_sz is not None and self.equity > 0:
            in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
            ct_d = ct_val * ct_mult
            qty = Decimal(repr(round(self.equity * AVG_FRAC * LEVERAGE, 6))) / (ct_d if in_usd else ct_d * Decimal(repr(mark)))
            sz = (qty / step).to_integral_value(rounding=ROUND_DOWN) * step
            if sz < min_sz:
                sz = Decimal(0)
        if sz <= 0:
            op["avg"] = {"done": True, "at": now, "why": why, "skipped": "объём меньше минимального на OKX"}
            self.event(f"{op['inst']}: {why} — усреднение не сделано: объём 5 % меньше минимального на OKX")
            self.save()
            return
        cl = f"{TAG}av{now % 10**12}"
        op["avg"] = {"cl": cl, "at": now, "why": why, "ordered": float(sz), "filled": 0.0, "done": False,
                     "base_size": op["size"], "base_px": op["px"]}
        self.event(f"{op['inst']}: {why} — усредняюсь на 5 % ({fmt_dec(sz)} по рынку), один раз")
        self.save()
        try:
            self.okx.order({"instId": op["inst"], "tdMode": "cross", "side": "buy" if op["side"] > 0 else "sell",
                            "ordType": "market", "sz": fmt_dec(sz), "clOrdId": cl, **self._ps(op["side"])})
        except OkxError as e:
            if e.code.isdigit() and len(e.code) == 5:  # отказ биржи — ордера нет; второй попытки нет (разово)
                op["avg"].update(done=True, skipped=str(e).split(" [")[0][:300])
                self.event(f"{op['inst']}: усреднение не принято — {e}")
                self.save()
                return
            # Ответ не от биржи — ордер мог уйти: _avg_fill найдёт его по метке.
            self.event(f"{op['inst']}: ответ на ордер усреднения не получен — {e}; сверка по метке")
        for _ in range(10):
            if self.okx.get_order(op["inst"], cl_id=cl).get("state") in ("filled", "canceled", "mmp_canceled"):
                break
            time.sleep(0.5)
        self._avg_fill(op)

    def _avg_fill(self, op: dict[str, Any]) -> None:
        """Ордер усреднения (по метке): исполненное — в запись позиции: объём,
        средняя цена (взвешенная с прежней), номинал; тейк — +1 % от новой
        средней на весь объём (ещё не тронутый тейк снимается и ставится
        заново сразу). Ордер завершён — больше не проверять; ордера
        нет 10 минут — до биржи не дошёл."""
        av = op["avg"]
        try:
            o = self.okx.get_order(op["inst"], cl_id=str(av.get("cl")))
        except OkxError as e:
            if e.code != "51603":
                raise
            o = {}
        got = min(size_of(o.get("accFillSz")), size_of(av.get("ordered")))
        if got > size_of(av.get("filled")):
            base_size, base_px = num(av.get("base_size")), num(av.get("base_px"))
            last = self.okx.ticker(op["inst"])
            px = num(o.get("avgPx"))
            if not (px > 0 and last > 0 and abs(px / last - 1) <= FILL_TOL):
                px = last  # мусор в цене исполнения — цена тикера
            new_size = base_size + float(got)
            op["px"] = (base_px * base_size + px * float(got)) / new_size if new_size > 0 else base_px
            op["size"] = new_size
            av["filled"] = float(got)
            ins = self.instruments().get(op["inst"], {})
            in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
            ct = num(ins.get("ctVal")) * num(ins.get("ctMult"), 1.0)
            op["notional"] = round(new_size * ct * (1 if in_usd else op["px"]), 2)
            tp_o = self.okx.get_order(op["inst"], str(op["tp_id"])) if op.get("tp_id") else {}
            if op.get("tp_id") and not (tp_o.get("state") == "live" and size_of(tp_o.get("accFillSz")) <= 0):
                self.event(f"{op['inst']}: тейк уже исполняется — новый тейк от средней не ставлю")
            else:
                tp = round_step(op["px"] * (1 + op["side"] * TP), ins.get("tickSz", "0.0001"),
                                ROUND_CEILING if op["side"] > 0 else ROUND_FLOOR)
                if tp > 0:
                    op["tp"], op["tp_s"] = float(tp), fmt_dec(tp)
                if op.get("tp_id"):
                    self.okx.cancel(op["inst"], str(op["tp_id"]))
                    op.pop("tp_id")
                self._place_tp(op, size_of(op["size"]))  # сразу — на весь объём от новой средней
            self.event(f"{op['inst']}: усреднено — {fmt_dec(Decimal(str(new_size)))} по средней {op['px']:.6g}, "
                       f"тейк {op['tp_s']}")
        if o.get("state") in ("filled", "canceled", "mmp_canceled") or \
                (not o and self.clock() - num(av.get("at")) > 600_000):
            av["done"] = True
        self.save()

    # вход
    def _ps(self, side: int) -> dict[str, Any]:
        return {"posSide": "long" if side > 0 else "short"} if self.pos_mode == "long_short_mode" else {}

    def _place_tp(self, op: dict[str, Any], size: Decimal) -> None:
        ins = self.instruments().get(op["inst"], {})
        sz = round_step(size, ins.get("lotSz", "1"), ROUND_DOWN)
        # Тейк — только по ту сторону от входа, не ближе +1 % (ни нулевой цены,
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

    def try_enter(self, s: dict[str, Any]) -> str:
        """Вход по сигналу; возвращает причину пропуска ('' — вошли)."""
        now = self.clock()
        if self.st.get("open"):
            return self._skip("own_open", f"у бота уже есть своя позиция ({(self.st.get('open') or {}).get('inst')})")
        if self.st.get("entering"):
            return self._skip("entering", "прошлый вход ещё не подтверждён биржей — сверка")
        if self.unverified:
            return self._skip("unverified", f"не удалось проверить позиции ({', '.join(sorted(self.unverified))}) — свои они или ваши")
        unknown = [str(p.get("instId")) for p in self.positions if str(p.get("instId")) in self.st.get("own_unknown", {})]
        if unknown:
            return self._skip("own_unknown", f"своя позиция без записи ({', '.join(unknown)}) — проверьте её в OKX; пока она есть, бот не входит")
        # Остальные позиции на счёте — владельца (руками): на их бумаги бот не входит.
        foreign = [str(p.get("instId")) for p in self.positions]
        if now - num(self.st["entered"].get(s["coin"])) < COIN_PAUSE_MS:
            return self._skip("coin_pause", "в эту бумагу входили меньше 3 часов назад")
        usd, against, count = self.sig.balance(s["coin"], s["side"], now)
        s = {**s, "usd": round(usd, 2), "usd_against": round(against, 2)}
        if (s["coin"], s["side"]) in self.sig.windows and count < MIN_WHALES:
            return self._skip("stale", f"китов за 2 часа до «сейчас» осталось {count} — сигнал устарел")
        if against > 0 and against >= usd:
            # Против сигнала — киты крупнее: в эту сторону не идём (их сигнал —
            # отдельный кандидат, он и войдёт).
            return self._skip("bigger_against", f"против сигнала киты крупнее: {'шорты' if s['side'] > 0 else 'лонги'} "
                                                f"{against:,.0f} $ против {usd:,.0f} $ — входим только с крупными".replace(",", " "))
        lost = self.sig.whale_loss(s["coin"], s["side"], s.get("wallets") or {}, now)
        if lost:
            return self._skip("whale_exit", f"кит сигнала уже закрыл позицию в убыток ({lost[0][0][:10]}…, "
                                            f"{lost[0][1]:+.0f} $) — не входим")
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
        if s["side"] * (hl_px / s["ref_px"] - 1) > CHASE:
            return self._skip("chase", f"цена ушла от китов дальше {CHASE * 100:g} %")
        if self.equity <= 0:
            return self._skip("no_equity", "капитал счёта 0")
        frac = MARGIN_FRAC
        notional = self.equity * frac * LEVERAGE
        # Параметры бумаги из каталога — только конечные положительные: мусор
        # в каталоге не должен дать ордер неверного размера.
        ct_val, ct_mult = dec(ins.get("ctVal")), dec(ins.get("ctMult") or "1")
        step, min_sz, tick = dec(ins.get("lotSz")), dec(ins.get("minSz")), dec(ins.get("tickSz"))
        if ct_val is None or ct_mult is None or step is None or min_sz is None or tick is None:
            return self._skip("bad_params", f"{inst}: неверные параметры бумаги в каталоге OKX")
        ct_d = ct_val * ct_mult
        ct = float(ct_d)
        # Десятичными, не двоичными дробями: 10 000 / (10 000 000 × 0,00001) — ровно 100, а не 99,999…
        # Размер контракта — в монете (ctValCcy = BTC) или уже в долларах (USD/USDC/USDT).
        in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
        qty = Decimal(repr(round(notional, 6))) / (ct_d if in_usd else ct_d * Decimal(repr(last)))
        sz = (qty / step).to_integral_value(rounding=ROUND_DOWN) * step
        # Тейк должен встать на шаг цены не ближе +1 % и не дальше +1,5 %: шаг
        # слишком крупный для этой цены — тейк невозможен, без выхода не входим.
        tp_try = round_step(last * (1 + s["side"] * TP), tick, ROUND_CEILING if s["side"] > 0 else ROUND_FLOOR)
        if tp_try <= 0 or not TP * 0.999 <= s["side"] * (float(tp_try) / last - 1) <= TP + TP_SLACK:
            return self._skip("tick", f"{inst}: шаг цены {fmt_dec(tick)} слишком крупный для тейка +1 % при цене {last}")
        if sz < min_sz:
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
                                   "wallets": s.get("wallets") or {}, "usd": s.get("usd"), "usd_against": s.get("usd_against")}
            self.save()
            ord_id = self.okx.order({"instId": inst, "tdMode": "cross", "side": "buy" if s["side"] > 0 else "sell",
                                     "ordType": "market", "sz": fmt_dec(sz), "clOrdId": cl, **self._ps(s["side"])})
        except OkxError as e:
            if self.st.get("entering") and not (e.code.isdigit() and len(e.code) == 5):
                # Ответ не от биржи (ошибка шлюза, обрыв, порча ответа): ордер
                # мог уйти. Запись «входим» остаётся — сверка счёта примет
                # позицию или снимет запись; до тех пор новых входов нет.
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
        o: dict[str, Any] = {}
        for _ in range(10):
            o = self.okx.get_order(inst, ord_id)
            if o.get("state") == "filled":
                break
            time.sleep(0.5)
        got_sz = size_of(o.get("accFillSz"))
        if o.get("state") in ("canceled", "mmp_canceled") and got_sz <= 0:
            self.st.pop("entering", None)
            blocked[inst] = {"at": now, "why": f"рыночный ордер отменён биржей ({o.get('state')})", "permanent": False}
            self.event(f"{inst}: рыночный ордер отменён биржей без исполнения — входа нет")
            return self._skip("canceled", f"{inst}: ордер отменён биржей")
        if o.get("state") not in ("filled", "partially_filled") or got_sz <= 0:
            # Не ясно, исполнился ли: запись «входим» остаётся, сверка счёта
            # (resolve_entering) примет позицию по метке или снимет запись.
            self.event(f"{inst}: исполнение ордера не подтверждено ({o.get('state') or 'нет ответа'}) — сверка по счёту")
            self.save()
            return self._skip("unconfirmed", f"{inst}: исполнение не подтверждено — сверка")
        # Цена и объём исполнения — с биржи; мусор или ноль — цена тикера и наш объём.
        avg = num(o.get("avgPx"))
        # Цена исполнения рыночного ордера — рядом с тикером (±5 %); дальше —
        # порча ответа: от неё тейк встал бы «в рынок». Тогда — цена тикера.
        fill_px = avg if avg > 0 and abs(avg / last - 1) <= FILL_TOL else last
        filled_d = got_sz if got_sz <= sz else sz
        filled = float(filled_d)
        tp = round_step(fill_px * (1 + s["side"] * TP), fmt_dec(tick), ROUND_CEILING if s["side"] > 0 else ROUND_FLOOR)
        op = {"coin": s["coin"], "inst": inst, "side": s["side"], "opened": now, "px": fill_px, "size": filled,
              "notional": round(filled * ct * (1 if in_usd else fill_px), 2), "tp": float(tp), "tp_s": fmt_dec(tp), "whales": s["whales"],
              "ref_px": s["ref_px"] / scale, "signal_ts": s["ts"], "in_id": ord_id,
              "margin_pct": round(frac * 100, 2), "wallets": s.get("wallets") or {},
              "usd": s.get("usd"), "usd_against": s.get("usd_against"),
              # Рыночный исполнен частично — может доисполниться: сверка
              # увеличит запись и тейк (_entry_fill).
              "in_done": o.get("state") == "filled", "ordered": float(sz)}
        self.st["open"] = op
        self.st["entered"][s["coin"]] = now
        self.st.pop("entering", None)
        self.event(f"вход {inst} {'лонг' if s['side'] > 0 else 'шорт'} {fmt_dec(sz)} по {fill_px} "
                   f"(≈{op['notional']:.0f} $, маржа {frac * 100:g} %, китов {s['whales']}), тейк {op['tp_s']}")
        self._place_tp(op, filled_d)
        self.save()
        return ""

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
              "in_id": str(order.get("ordId") or ""), "adopted": True,
              "wallets": info.get("wallets") if isinstance(info.get("wallets"), dict) else {}}
        self.st["open"] = op
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
        if self.st.get("open"):
            return
        checked = self._checked
        held = {str(p.get("instId") or "") for p in self.positions}
        self.unverified &= held
        for p in self.positions:
            inst = str(p.get("instId") or "")
            if not inst or time.time() - checked.get(inst, 0) < 3600:
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
            mine = [o for o in hist
                    if str(o.get("clOrdId", "")).startswith(f"{TAG}in") and o.get("state") in ("filled", "partially_filled")
                    and (1 if o.get("side") == "buy" else -1) == side
                    and self.clock() - num(o.get("cTime")) < 7 * 86_400_000
                    and str(o.get("ordId") or "") not in self.st["closed_ids"]]
            if mine:
                info = self.st.get("entering") if (self.st.get("entering") or {}).get("inst") == inst else None
                self.adopt(p, mine[0], info)
                self.st.pop("entering", None)
                return

    def recover(self) -> None:
        """После перезапуска: вход был начат, но не записан. Ордер исполнен и
        позиция есть — принять её как свою и поставить тейк; ордера нет —
        входа не было."""
        if not self.st.get("entering"):
            return
        self.sync()  # в нём resolve_entering: своя позиция — принять, нет — снять запись
        self.save()

    def step(self) -> None:
        sigs = self.sig.poll(self.clock())
        if time.time() - self.last_sync >= SYNC_SEC or sigs:
            self.sync()
        if not self.is_on():
            self.save()
            return
        for s in self.candidates(sigs):
            self.skip_code = "other"
            why = self.try_enter(s)
            if not why:
                break
            self.st["last_skip"] = {"coin": s["coin"], "side": s["side"], "why": why, "at": self.clock(),
                                    "code": self.skip_code}
            self.count_skip(s, self.skip_code)
        self.save()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Автокопирование китов — реальная торговля на OKX")
    ap.add_argument("--db", default=os.environ.get("WHALE_HL_DB", "hyperliquid.db"), help="база бота (только чтение)")
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
            json.dump({"error": msg, "heartbeat": now_ms(), "open": None, "entered": {}, "events": []}, f, ensure_ascii=False)
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
    me = [os.path.abspath(__file__), core.__file__]  # код обновился — перезапуск (бэктест — отдельным процессом)
    stamp = [os.path.getmtime(p) for p in me]
    tr.event(f"старт ({'демо-счёт' if okx.simulated else 'РЕАЛЬНЫЙ счёт'})")
    try:
        tr.recover()
    except (OkxError, OSError) as e:  # не вышло сейчас — find_own в сверке счёта догонит
        tr.event(f"сверка после перезапуска: {e}")
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
        # Код обновился (автодеплой) — выйти: systemd поднимет с новым. Идёт
        # пересчёт — дождаться его (иначе systemd убьёт его вместе со службой).
        if bts.proc is None and [os.path.getmtime(p) for p in me] != stamp:
            tr.event("код обновился — перезапуск")
            return 0
        time.sleep(POLL_SEC)


if __name__ == "__main__":
    sys.exit(main())
