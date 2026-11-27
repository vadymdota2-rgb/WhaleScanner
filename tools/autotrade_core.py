# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Общее для торговли (autotrade_live.py) и бэктеста-повтора
(autotrade_replay.py): база бота только на чтение, коды сделок, окно китов,
имя бумаги на OKX, публичный запрос к OKX, предел памяти.

Раньше это жило в бэктесте-сетке (autotrade_backtest.py) вместе с ~1500
строками, которые служба больше не запускает; сетка удалена, общее — здесь.
"""
from __future__ import annotations

import json
import math
import os
import re
import sqlite3
import urllib.parse
import urllib.request
from collections import deque
from typing import Any

OKX_API = "https://www.okx.com"
# Имена на OKX, которые отличаются от Hyperliquid (сверено по цене в ноябре
# 2026). Совпадение имени ничего не доказывает: xyz:STX — акции Seagate, а STX
# на OKX — токен Stacks; поэтому вход всё равно сверяет цену (±2 %).
OKX_ALIASES = {"GOLD": "XAU", "SILVER": "XAG", "COPPER": "XCU", "PLATINUM": "XPT", "PALLADIUM": "XPD",
               "NATGAS": "NG", "BRENTOIL": "BZ", "SP500": "US500", "XYZ100": "US100", "ANTH": "ANTHROPIC",
               "OAI": "OPENAI", "SMSN": "SAMSUNG", "SKHX": "SKHYNIX", "TCNT": "TENCENT"}
NANOS = 1_000_000_000
DAY_MS = 86_400_000
HOUR_MS = 3_600_000

# dir_code в hl_fills (hyperliquid_core.cpp, dirCode).
OPEN_LONG, OPEN_SHORT, CLOSE_LONG, CLOSE_SHORT, FLIP, LIQ_LONG, LIQ_SHORT = 1, 2, 3, 4, 5, 6, 7
CLOSES_FOR = {1: (CLOSE_LONG, FLIP, LIQ_LONG), -1: (CLOSE_SHORT, FLIP, LIQ_SHORT)}


def open_ro(path: str) -> sqlite3.Connection:
    """База бота — только на чтение: запись в неё невозможна даже по ошибке."""
    if not os.path.isfile(path):
        raise OSError(f"нет базы бота: {path}")
    con = sqlite3.connect("file:" + os.path.abspath(path) + "?mode=ro", uri=True)
    con.execute("PRAGMA query_only=1")
    return con


def is_perp(coin: str) -> bool:
    """Фьючерс (бессрочный): основная площадка (BTC, ETH, …) и HIP-3
    (xyz:TSLA, …). Спот у Hyperliquid — «@107» или «PURR/USDC» — не берём."""
    return bool(coin) and not coin.startswith("@") and "/" not in coin


def okx_candidate(coin: str) -> tuple[str, float]:
    """Имя бумаги на OKX по имени на Hyperliquid и масштаб цены (сколько
    единиц OKX в одной единице Hyperliquid): kPEPE — 1000 PEPE."""
    base = coin.split(":", 1)[1] if ":" in coin else coin
    if ":" not in coin and len(base) > 1 and base[0] == "k" and base[1:].isupper():
        return base[1:], 1000.0
    return (OKX_ALIASES.get(base, base) if ":" in coin else base), 1.0


# Стоп — 1,5 % от цены входа (по бэктесту с входом по рынку 27.11.2026: стоп
# 1,5 % — итог на 17 $ лучше 3 % и просадка 38 $ против 52 $; было 3 %,
# до того — за уровнем 4ч, не дальше 1,5 %; уровни убраны 27.11.2026).
STOP_PCT = 0.015


# MA30 на 15-минутном графике (правило владельца 27.11.2026): средняя 30
# закрытий 15-минутных свечей (идущая — по текущей цене). Лонг — только когда
# цена выше MA30, шорт — ниже.
MA_BARS = 30
MA_BAR_MS = 15 * 60_000


def ma_ok(ma: float, px: float, side: int) -> bool | None:
    """Цена по ту сторону MA30, что и сделка? None — MA нет или цена ровно на
    ней (до погрешности округления): MA направления не даёт — как без неё."""
    if not (math.isfinite(ma) and math.isfinite(px)) or ma <= 0 or px <= 0 or abs(px / ma - 1) < 1e-9:
        return None
    return px > ma if side > 0 else px < ma


# Рейтинг китов по нашим сделкам (правило владельца 27.11.2026): сделка бота
# засчитывается всем китам её сигнала (плюс — всем плюс). Винрейт кита — от
# RATING_MIN_TRADES сделок; выше — приоритетнее; выше PRIORITY_WINRATE —
# приоритетный кит: с ним входим и в одиночку, без MA30 и «против крупнее».
RATING_MIN_TRADES = 5
PRIORITY_WINRATE = 0.9


class Rating:
    """Наш винрейт с каждым китом: кошелёк → [плюсовых, всего]."""

    def __init__(self) -> None:
        self.per: dict[str, list[int]] = {}

    def add(self, wallets: object, win: bool) -> None:
        for w in wallets if isinstance(wallets, (list, tuple, dict, set)) else ():
            if isinstance(w, str):
                v = self.per.setdefault(w, [0, 0])
                v[0] += int(win)
                v[1] += 1

    def rate(self, wallet: str) -> float | None:
        v = self.per.get(wallet)
        return v[0] / v[1] if v is not None and v[1] >= RATING_MIN_TRADES else None

    def best(self, wallets: object) -> float:
        """Лучший винрейт среди китов сигнала; оценённых нет — −1."""
        rates = [r for w in (wallets if isinstance(wallets, (list, tuple, dict, set)) else ())
                 if isinstance(w, str) and (r := self.rate(w)) is not None]
        return max(rates, default=-1.0)

    def priority(self) -> set[str]:
        return {w for w in self.per if (r := self.rate(w)) is not None and r > PRIORITY_WINRATE}


def fixed_stop(px: float, side: int, pct: float = STOP_PCT) -> float:
    """Стоп входа: pct от цены входа px (лонг — ниже, шорт — выше); цена —
    мусор — 0 (вызывающий такое не открывает)."""
    if not math.isfinite(px) or px <= 0:
        return 0.0
    return px * (1 - side * pct)


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]", "_", name)


def okx_public(path: str, params: dict[str, Any]) -> object:
    """GET публичного API OKX (ключ не нужен) → data; ошибка OKX — None."""
    url = OKX_API + path + "?" + urllib.parse.urlencode(params)
    # Без своего User-Agent OKX отвечает 403 стандартному urllib.
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (compatible; backtest)"})
    with urllib.request.urlopen(req, timeout=20) as r:
        got = json.loads(r.read(16 << 20))
    return got.get("data") if isinstance(got, dict) and str(got.get("code")) == "0" else None


class Window:
    """Окно китов одной бумаги и стороны: сделки за последние 2 часа и суммы
    по кошелькам — по ходу (добавили — прибавили, вышла из окна — вычли), а
    не пересчётом на каждую сделку. Объём — в нано-долларах целыми: сумма и
    разность точны, сравнение с порогом не «плывёт» от округления.

    count — кошельков от порога (китов); whale_n — их нано $ (объём китов);
    big_n / big_u — нано $ и штуки китов, у кого штуки известны, — для
    средней цены китов."""
    __slots__ = ("big_k", "big_n", "big_u", "count", "limit", "orders", "per", "whale_n")

    def __init__(self, limit: int) -> None:
        self.limit = limit
        self.orders: deque[tuple[int, str, int, float]] = deque()
        self.per: dict[str, list[Any]] = {}   # кошелёк → [нано $, штук, сделок, сделок со штуками]
        self.count = 0
        self.whale_n = 0
        self.big_n = 0
        self.big_u = 0.0
        self.big_k = 0

    def _change(self, wallet: str, dn: int, du: float, dk: int, dku: int) -> None:
        v = self.per.get(wallet)
        n0, u0, k0, ku0 = v if v is not None else (0, 0.0, 0, 0)
        k1, ku1 = k0 + dk, ku0 + dku
        n1 = n0 + dn if k1 else 0
        u1 = u0 + du if ku1 else 0.0
        was, now = k0 > 0 and n0 >= self.limit, k1 > 0 and n1 >= self.limit
        self.count += now - was
        self.whale_n += (n1 if now else 0) - (n0 if was else 0)
        if k0 and ku0 and n0 >= self.limit:
            self.big_n -= n0
            self.big_u -= u0
            self.big_k -= 1
        if k1 and ku1 and n1 >= self.limit:
            self.big_n += n1
            self.big_u += u1
            self.big_k += 1
        if not self.big_k:  # китов-со-штуками нет — сумма штук ровно 0 (без накопленной погрешности)
            self.big_n, self.big_u = 0, 0.0
        if k1:
            self.per[wallet] = [n1, u1, k1, ku1]
        else:
            self.per.pop(wallet, None)

    def add(self, ts: int, wallet: str, nanos: int, units: float) -> None:
        item = (ts, wallet, nanos, units)
        if self.orders and ts < self.orders[-1][0]:
            # Опоздавшая сделка (бот записал её позже) — на своё место по
            # времени: выброс старше окна идёт слева и требует порядка.
            i = len(self.orders)
            while i > 0 and self.orders[i - 1][0] > ts:
                i -= 1
            self.orders.insert(i, item)
        else:
            self.orders.append(item)
        self._change(wallet, nanos, units, 1, int(units > 0))

    def expire(self, before: int) -> None:
        while self.orders and self.orders[0][0] < before:
            _t, w, n, u = self.orders.popleft()
            self._change(w, -n, -u, -1, -int(u > 0))


def limit_memory(mb: int) -> None:
    """Жёсткий предел памяти процесса (RLIMIT_AS): дальше — MemoryError и
    понятный выход, а не съеденная память сервера, где живут бот и API."""
    if mb <= 0:
        return
    import resource  # noqa: PLC0415 — только Unix; нужен лишь при пределе памяти
    cap = mb * 1024 * 1024
    _soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    if hard != resource.RLIM_INFINITY:
        cap = min(cap, hard)
    resource.setrlimit(resource.RLIMIT_AS, (cap, hard))


def out_of_memory(e: BaseException, capped: bool) -> bool:
    """Упёрлись в предел памяти? Python говорит MemoryError, но SQLite на
    нехватке памяти — по-разному в разных сборках: sqlite3.Error «out of
    memory» или SystemError (функция вернула ошибку без исключения — так
    падает модуль sqlite3, когда malloc отказал). SystemError считаем
    нехваткой только под пределом памяти: без него это настоящая ошибка."""
    if isinstance(e, MemoryError):
        return True
    if isinstance(e, sqlite3.Error) and "memory" in str(e).lower():
        return True
    return capped and isinstance(e, SystemError)
