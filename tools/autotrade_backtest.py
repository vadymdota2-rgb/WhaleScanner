#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Бэктест «автокопирования китов» на фьючерсах Hyperliquid — этап 1 ТЗ.

Вопрос один: заработала бы копия сделок китов на рынках акций (xyz:, io:,
para:) после комиссий, фандинга и задержки — и не подгонка ли это. Скрипт
ничего не торгует и бота не трогает:

- база бота (hyperliquid.db) открывается только на чтение
  (``file:…?mode=ro``); в каталог базы и рядом с ней скрипт не пишет —
  отчёт и кеш свечей отказываются туда ложиться;
- в сеть — только за свечами и историей фандинга (публичный info API
  Hyperliquid), один раз на бумагу и интервал, с паузой между запросами;
  всё кешируется во временном каталоге и копится между запусками
  (``--offline`` — только кеш, без сети).

Свечи. Hyperliquid отдаёт лишь последние 5000 свечей каждого интервала:
минутных — на 3,5 дня, 5-минутных — на 17, 15-минутных — на 52, часовых —
на 208. Для каждой сделки берётся самый мелкий интервал, покрывающий её
целиком; доля сделок по интервалам — в отчёте. Разбивка по задержке D честна
только на минутных свечах — отчёт показывает её отдельно для них.

Модель исполнения (консервативная):
- вход — через D секунд после сделки кита, по цене внутри свечи (линейно от
  открытия к закрытию) плюс проскальзывание против нас;
- в свече входа учитывается только движение против нас (стоп), не в нашу
  пользу (тейк);
- стоп и тейк в одной свече — считается стоп; стоп — в начале свечи, тейк —
  в конце; любой выход — с проскальзыванием против нас;
- комиссия тейкера — с обеих сторон; фандинг — по часам удержания
  (лонг платит положительную ставку);
- одна позиция на бумагу; не чаще одного входа на бумагу за N минут.

Подбор и защита от подгонки: последние 25 % периода (по времени сигналов)
в подборе не участвуют. Лучшие 10 наборов выбираются на первых 75 % и
проверяются на отложенных. Вердикт — по лучшему набору подбора, проверенному
на отложенных: итог > 0 после комиссий, profit factor ≥ 1,3, сделок ≥ 100,
просадка ≤ 15 % условного депозита.

    python3 tools/autotrade_backtest.py --db ~/WhaleScanner/hyperliquid.db
    python3 tools/autotrade_backtest.py --db … --offline --out /tmp/bt
"""
from __future__ import annotations

import argparse
import bisect
import csv
import itertools
import json
import math
import os
import re
import sqlite3
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from array import array
from collections import defaultdict
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field, replace
from datetime import datetime, timedelta, timezone

HL_INFO = "https://api.hyperliquid.xyz/info"
OKX_API = "https://www.okx.com"
# Имена на OKX, которые отличаются от Hyperliquid (сверено по цене в ноябре
# 2026). Совпадение имени ничего не доказывает: xyz:STX — акции Seagate, а
# STX на OKX — токен Stacks; xyz:CVX — Chevron, а CVX на OKX — токен Convex.
# Поэтому любая пара проходит ещё и сверку цены (okx_map).
OKX_ALIASES = {"GOLD": "XAU", "SILVER": "XAG", "COPPER": "XCU", "PLATINUM": "XPT", "PALLADIUM": "XPD",
               "NATGAS": "NG", "BRENTOIL": "BZ", "SP500": "US500", "XYZ100": "US100", "ANTH": "ANTHROPIC",
               "OAI": "OPENAI", "SMSN": "SAMSUNG", "SKHX": "SKHYNIX", "TCNT": "TENCENT"}
OKX_PRICE_TOL = 0.02   # цены одной бумаги на двух биржах расходятся меньше чем на 2 %
OKX_BAR, OKX_BAR_SEC = "5m", 300
STOCK_DEXES = ("xyz", "io", "para")
# Интервалы от мелкого к крупному; Hyperliquid хранит 5000 последних каждого.
INTERVALS: tuple[tuple[str, int], ...] = (("1m", 60), ("5m", 300), ("15m", 900), ("1h", 3600))
KEEP_CANDLES = 5000
NANOS = 1_000_000_000
DAY_MS = 86_400_000
HOUR_MS = 3_600_000

# dir_code в hl_fills (hyperliquid_core.cpp, dirCode).
OPEN_LONG, OPEN_SHORT, CLOSE_LONG, CLOSE_SHORT, FLIP, LIQ_LONG, LIQ_SHORT = 1, 2, 3, 4, 5, 6, 7
CLOSES_FOR = {1: (CLOSE_LONG, FLIP, LIQ_LONG), -1: (CLOSE_SHORT, FLIP, LIQ_SHORT)}

Fetch = Callable[[dict], object]


# ── данные из базы бота (только чтение) ──────────────────────────────────────

@dataclass
class Signal:
    wallet: str
    coin: str
    side: int          # +1 лонг, −1 шорт
    ts: int            # мс, первое исполнение ордера кита
    notional: float    # $ всего ордера
    account: float     # $ счёта кита на момент сделки (0 — неизвестно)
    leverage: int      # плечо кита (0 — неизвестно)
    pnl30: float = 0.0             # закрытый PnL кита за 30 дней до сделки
    whale_close: int | None = None  # когда кит начал закрывать (мс)
    px: float = 0.0                 # средняя цена ордера кита
    size: float = 0.0               # штук в ордере
    # Сговор китов: за окно до этого ордера включительно — сколько разных
    # кошельков открыли ту же сторону на той же бумаге, их общий объём и
    # средняя цена входа (по объёму). Один кит — wallets = 1.
    wallets: int = 1
    cluster_notional: float = 0.0
    ref_px: float = 0.0

    def __post_init__(self) -> None:
        # Одиночный ордер — сам себе сговор (пока attach_clusters не посчитал).
        self.cluster_notional = self.cluster_notional or self.notional
        self.ref_px = self.ref_px or self.px


def open_ro(path: str) -> sqlite3.Connection:
    """База бота — только на чтение: запись в неё невозможна даже по ошибке."""
    if not os.path.isfile(path):
        raise SystemExit(f"нет базы: {path}")
    uri = "file:" + os.path.abspath(path) + "?mode=ro"
    con = sqlite3.connect(uri, uri=True)
    con.execute("PRAGMA query_only=1")
    return con


def is_stock(coin: str, dexes: Sequence[str]) -> bool:
    return ":" in coin and coin.split(":", 1)[0] in dexes


def is_perp(coin: str) -> bool:
    """Фьючерс (бессрочный): основная площадка (BTC, ETH, …) и HIP-3
    (xyz:TSLA, …). Спот у Hyperliquid — «@107» или «PURR/USDC» — не берём."""
    return bool(coin) and not coin.startswith("@") and "/" not in coin


def load_signals(con: sqlite3.Connection, dexes: Sequence[str] | None, since_ms: int = 0,
                 cluster_ms: int = 24 * 3_600_000, min_whale: float = 0.0) -> list[Signal]:
    """Открытия лонга и шорта китов на фьючерсах (закрытия — не сигнал).
    dexes — только эти площадки HIP-3 (акции); None — все фьючерсы: крипта,
    акции, металлы. Ордер кита исполняется кусками (много строк с одним oid)
    — сигнал один на ордер: объём — сумма кусков, цена — средняя, время —
    первого куска. К каждому — сговор за cluster_ms до него (Signal.wallets)."""
    rows = con.execute(
        "SELECT wallet, coin, dir_code, oid, tid, ts, notional_nanos, sz, account_value_nanos, leverage "
        "FROM hl_fills WHERE dir_code IN (1, 2) AND ts >= ? ORDER BY ts, tid",
        (since_ms,)).fetchall()
    orders: dict[tuple, Signal] = {}
    for wallet, coin, code, oid, tid, ts, notional, sz, account, lev in rows:
        if not is_perp(coin) or (dexes is not None and not is_stock(coin, dexes)):
            continue
        try:
            n = abs(float(sz))
        except (TypeError, ValueError):
            n = 0.0
        key = (wallet, coin, oid) if oid else (wallet, coin, "tid", tid)
        sig = orders.get(key)
        if sig is None:
            orders[key] = Signal(wallet, coin, 1 if code == OPEN_LONG else -1, ts, notional / NANOS,
                                 account / NANOS, lev, size=n)
        else:
            sig.notional += notional / NANOS
            sig.size += n
    signals = sorted(orders.values(), key=lambda s: (s.ts, s.wallet, s.coin))
    for sig in signals:
        sig.px = sig.notional / sig.size if sig.size > 0 else 0.0
    attach_history(con, signals)
    attach_clusters(signals, cluster_ms, min_whale)
    return signals


def attach_clusters(signals: list[Signal], window_ms: int, min_whale: float = 0.0) -> None:
    """Сговор: на каждом ордере — сколько разных китов за окно до него
    (включительно) открыли ту же сторону на той же бумаге, их общий объём и
    средняя цена по объёму. Кит считается, если его позиция в эту сторону
    за окно (все его ордера) — не меньше min_whale $. Только прошлое — без
    заглядывания вперёд."""
    recent: dict[tuple[str, int], list[Signal]] = defaultdict(list)
    for sig in signals:
        bucket = recent[(sig.coin, sig.side)]
        bucket.append(sig)
        while bucket and bucket[0].ts < sig.ts - window_ms:
            bucket.pop(0)
        per_wallet: dict[str, float] = defaultdict(float)
        for b in bucket:
            per_wallet[b.wallet] += b.notional
        sig.wallets = sum(1 for v in per_wallet.values() if v >= min_whale)
        sig.cluster_notional = sum(b.notional for b in bucket)
        # Средняя цена входа всех — объём в $ на число штук (как средняя
        # цена позиции), а не среднее цен с весами.
        priced = [b for b in bucket if b.size > 0]
        units = sum(b.size for b in priced)
        sig.ref_px = sum(b.notional for b in priced) / units if units > 0 else sig.px


def attach_history(con: sqlite3.Connection, signals: list[Signal]) -> None:
    """Закрытый PnL кита за 30 дней строго до сделки (без заглядывания
    вперёд) и момент, когда кит начал закрывать позицию."""
    wallets = sorted({s.wallet for s in signals})
    pnl: dict[str, tuple[list[int], list[float]]] = {}
    closes: dict[tuple[str, str, int], list[int]] = defaultdict(list)
    for chunk in (wallets[i:i + 500] for i in range(0, len(wallets), 500)):
        marks = ",".join("?" * len(chunk))
        acc: dict[str, list[tuple[int, int]]] = defaultdict(list)
        for w, ts, p in con.execute(
                f"SELECT wallet, ts, closed_pnl_nanos FROM hl_fills WHERE closed_pnl_nanos != 0 "  # noqa: S608 — только «?»
                f"AND wallet IN ({marks}) ORDER BY ts", chunk):
            acc[w].append((ts, p))
        for w, items in acc.items():
            times = [t for t, _ in items]
            sums = list(itertools.accumulate((p / NANOS for _, p in items), initial=0.0))
            pnl[w] = (times, sums)
        for w, coin, ts, code in con.execute(
                f"SELECT wallet, coin, ts, dir_code FROM hl_fills WHERE dir_code IN (3, 4, 5, 6, 7) "  # noqa: S608
                f"AND wallet IN ({marks}) ORDER BY ts", chunk):
            for side, codes in CLOSES_FOR.items():
                if code in codes:
                    closes[(w, coin, side)].append(ts)
    for s in signals:
        if s.wallet in pnl:
            times, sums = pnl[s.wallet]
            lo = bisect.bisect_left(times, s.ts - 30 * DAY_MS)
            hi = bisect.bisect_left(times, s.ts)
            s.pnl30 = sums[hi] - sums[lo]
        lst = closes.get((s.wallet, s.coin, s.side))
        if lst:
            i = bisect.bisect_right(lst, s.ts)
            s.whale_close = lst[i] if i < len(lst) else None


# ── свечи и фандинг (сеть — только здесь) ────────────────────────────────────

def http_fetch(body: dict) -> object:
    """Запрос к публичному API: Hyperliquid — POST info; {"okx": путь,
    "params": {…}} — GET публичного API OKX (ключ не нужен), ответ — data."""
    if "okx" in body:
        url = OKX_API + str(body["okx"]) + "?" + urllib.parse.urlencode(body.get("params") or {})
        # Без своего User-Agent OKX отвечает 403 стандартному urllib.
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (compatible; backtest)"})  # noqa: S310 — адрес постоянный, https
        with urllib.request.urlopen(req, timeout=20) as r:  # noqa: S310 — адрес постоянный, https
            got = json.loads(r.read(16 << 20))
        return got.get("data") if isinstance(got, dict) and str(got.get("code")) == "0" else None
    req = urllib.request.Request(HL_INFO, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:  # noqa: S310 — адрес постоянный, https
        return json.loads(r.read(4 << 20))


def safe_name(coin: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]", "_", coin)


@dataclass
class Market:
    """Свечи одной бумаги по интервалам: время открытия (мс) и o, h, l, c."""
    by_interval: dict[str, list[tuple[int, float, float, float, float]]] = field(default_factory=dict)


class MarketData:
    def __init__(self, cache: str, offline: bool, fetch: Fetch | None = None, pause: float = 1.0,
                 now_ms: int | None = None) -> None:
        self.cache = cache
        self.offline = offline
        self.fetch = fetch or http_fetch
        self.pause = pause
        self.now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
        self.requests = 0
        self.errors: list[str] = []
        os.makedirs(cache, exist_ok=True)

    def _call(self, body: dict) -> list:
        if self.offline:
            return []
        if self.requests and self.pause:
            time.sleep(self.pause)
        self.requests += 1
        try:
            got = self.fetch(body)
        except Exception as e:  # noqa: BLE001 — сеть: любой сбой — пропуск, а не падение
            what = body.get("okx") or f"{body.get('type')} {body.get('req', body).get('coin', '')}"
            self.errors.append(f"{what}: {type(e).__name__}")
            return []
        return got if isinstance(got, list) else []

    def _call_any(self, body: dict) -> object:
        """Как _call, но ответ — любой JSON (allMids — словарь)."""
        if self.offline:
            return None
        if self.requests and self.pause:
            time.sleep(self.pause)
        self.requests += 1
        try:
            return self.fetch(body)
        except Exception as e:  # noqa: BLE001 — сеть: любой сбой — пропуск, а не падение
            self.errors.append(f"{body.get('type') or body.get('okx')}: {type(e).__name__}")
            return None

    def okx_candles(self, inst: str, start_ms: int, end_ms: int) -> Market:
        """5-минутные свечи OKX (история — на годы назад, по 100 за запрос,
        от новых к старым), с кешем как у Hyperliquid."""
        step = OKX_BAR_SEC * 1000
        name = f"okx_candles_{safe_name(inst)}_{OKX_BAR}.json"
        have = {int(k): v for k, v in self._load(name).items()}
        end_ms = min(end_ms, self.now_ms)
        covered = have and min(have) <= start_ms and max(have) + step >= end_ms - step
        if not covered and not self.offline:
            after = end_ms + step
            for _ in range(2000):
                rows = self._call({"okx": "/api/v5/market/history-candles",
                                   "params": {"instId": inst, "bar": OKX_BAR, "after": after, "limit": 100}})
                got = 0
                for r in rows:
                    try:
                        t = int(r[0])
                        have[t] = [float(r[1]), float(r[2]), float(r[3]), float(r[4])]
                        got += 1
                        after = min(after, t)
                    except (IndexError, TypeError, ValueError):
                        continue
                if not got or after <= start_ms:
                    break
            if have:
                self._save(name, {str(k): v for k, v in have.items()})
        return Market({OKX_BAR: sorted((t, *v) for t, v in have.items() if all(x > 0 for x in v))})

    def okx_funding(self, inst: str, start_ms: int, end_ms: int) -> tuple[dict[int, float], str]:
        """Фандинг OKX по факту (realizedRate) на моменты списания."""
        name = f"okx_funding_{safe_name(inst)}.json"
        have = {int(k): float(v) for k, v in self._load(name).items()}
        if not self.offline and not (have and min(have) <= start_ms and max(have) >= end_ms - 8 * HOUR_MS):
            after = min(end_ms, self.now_ms) + 1
            for _ in range(200):
                rows = self._call({"okx": "/api/v5/public/funding-rate-history",
                                   "params": {"instId": inst, "after": after, "limit": 100}})
                got = 0
                for r in rows:
                    try:
                        t = int(r["fundingTime"])
                        have[t] = float(r.get("realizedRate") or r["fundingRate"])
                        got += 1
                        after = min(after, t)
                    except (KeyError, TypeError, ValueError):
                        continue
                if not got or after <= start_ms:
                    break
            if have:
                self._save(name, {str(k): v for k, v in have.items()})
        return have, ("OKX" if have else "нет данных — 0")

    def _load(self, name: str) -> dict:
        p = os.path.join(self.cache, name)
        try:
            with open(p, encoding="utf-8") as f:
                data = json.load(f)
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def _save(self, name: str, data: dict) -> None:
        p = os.path.join(self.cache, name)
        with open(p + ".tmp", "w", encoding="utf-8") as f:
            json.dump(data, f)
        os.replace(p + ".tmp", p)

    def candles(self, coin: str, start_ms: int, end_ms: int) -> Market:
        """Свечи от мелких к крупным; как только интервал покрыл весь нужный
        срок — крупнее не запрашиваем (меньше запросов к API)."""
        m = Market()
        end_ms = min(end_ms, self.now_ms)
        for interval, sec in INTERVALS:
            name = f"candles_{safe_name(coin)}_{interval}.json"
            have = {int(k): v for k, v in self._load(name).items()}
            oldest = self.now_ms - KEEP_CANDLES * sec * 1000
            want_from = max(start_ms - sec * 1000, oldest)
            covered = have and min(have) <= want_from and max(have) + sec * 1000 >= min(end_ms, self.now_ms - sec * 1000)
            if want_from < end_ms and not covered:
                cur = want_from
                for _ in range(40):  # до 40 страниц на интервал
                    got = self._call({"type": "candleSnapshot",
                                      "req": {"coin": coin, "interval": interval, "startTime": cur, "endTime": end_ms}})
                    rows = [c for c in got if isinstance(c, dict) and isinstance(c.get("t"), int)]
                    for c in rows:
                        try:
                            have[c["t"]] = [float(c["o"]), float(c["h"]), float(c["l"]), float(c["c"])]
                        except (KeyError, TypeError, ValueError):
                            continue
                    if not rows:
                        break
                    nxt = max(c["t"] for c in rows) + sec * 1000
                    if nxt <= cur or nxt >= end_ms:
                        break
                    cur = nxt
                if have:
                    self._save(name, {str(k): v for k, v in have.items()})
            got_c = sorted((t, *v) for t, v in have.items() if all(x > 0 for x in v))
            m.by_interval[interval] = got_c
            if got_c and got_c[0][0] <= start_ms and got_c[-1][0] + sec * 1000 >= end_ms:
                break
        return m

    def funding(self, coin: str, start_ms: int, end_ms: int, from_db: dict[int, float]) -> tuple[dict[int, float], str]:
        """Ставка фандинга по часам (мс начала часа → доля). Сначала база
        бота, нет там — история у API (кеш), нет и там — ноль, с пометкой."""
        if from_db:
            return from_db, "база"
        name = f"funding_{safe_name(coin)}.json"
        have = {int(k): float(v) for k, v in self._load(name).items()}
        if not self.offline and not (have and min(have) <= start_ms and max(have) >= end_ms - 2 * HOUR_MS):
            cur = start_ms
            for _ in range(40):
                got = self._call({"type": "fundingHistory", "coin": coin, "startTime": cur, "endTime": end_ms})
                rows = [r for r in got if isinstance(r, dict) and isinstance(r.get("time"), int)]
                for r in rows:
                    try:
                        have[r["time"] // HOUR_MS * HOUR_MS] = float(r["fundingRate"])
                    except (KeyError, TypeError, ValueError):
                        continue
                if not rows:
                    break
                nxt = max(r["time"] for r in rows) + 1
                if nxt <= cur or nxt >= end_ms:
                    break
                cur = nxt
            if have:
                self._save(name, {str(k): v for k, v in have.items()})
        return have, ("API" if have else "нет данных — 0")


def okx_candidate(coin: str) -> tuple[str, float]:
    """Имя бумаги на OKX по имени на Hyperliquid и масштаб цены (сколько
    единиц OKX в одной единице Hyperliquid): kPEPE — 1000 PEPE."""
    base = coin.split(":", 1)[1] if ":" in coin else coin
    if ":" not in coin and len(base) > 1 and base[0] == "k" and base[1:].isupper():
        return base[1:], 1000.0
    return (OKX_ALIASES.get(base, base) if ":" in coin else base), 1.0


def okx_map(md: MarketData, coins: Sequence[str]) -> dict[str, dict]:
    """Какие бумаги китов можно торговать на OKX. Имя (с заменами) — только
    кандидат; пара принимается, если цены сейчас сходятся (±2 % с учётом
    масштаба). Иначе — «нет на OKX» или «другой актив с тем же именем».
    Результат — в кеш на сутки (offline — из кеша)."""
    name = "okx_map.json"
    cached = md._load(name)
    if cached.get("day") == md.now_ms // DAY_MS or md.offline:
        got = cached.get("map") or {}
        if all(c in got for c in coins) or md.offline:
            return {c: got.get(c, {"status": "unverified"}) for c in coins}
    insts = {r.get("instId") for r in md._call({"okx": "/api/v5/public/instruments", "params": {"instType": "SWAP"}})
             if isinstance(r, dict) and r.get("state") == "live"}
    last: dict[str, float] = {}
    for r in md._call({"okx": "/api/v5/market/tickers", "params": {"instType": "SWAP"}}):
        try:
            last[str(r["instId"])] = float(r["last"])
        except (KeyError, TypeError, ValueError):
            continue
    mids: dict[str, float] = {}
    for dex in sorted({c.split(":", 1)[0] if ":" in c else "" for c in coins}):
        got = md._call_any({"type": "allMids", "dex": dex})
        if isinstance(got, dict):
            for k, v in got.items():
                try:
                    mids[str(k)] = float(v)
                except (TypeError, ValueError):
                    continue
    out: dict[str, dict] = {}
    for c in coins:
        base, scale = okx_candidate(c)
        inst = f"{base}-USDT-SWAP"
        if not insts:  # каталог OKX не получен — неизвестно, а не «нет»
            out[c] = {"status": "unverified", "inst": inst}
        elif inst not in insts:
            out[c] = {"status": "missing"}
        elif not (mids.get(c, 0) > 0 and last.get(inst, 0) > 0):
            out[c] = {"status": "unverified", "inst": inst}
        else:
            ratio = last[inst] * scale / mids[c]
            ok = abs(ratio - 1) <= OKX_PRICE_TOL
            out[c] = {"status": "ok" if ok else "mismatch", "inst": inst, "scale": scale, "ratio": round(ratio, 5)}
    if insts and not md.offline:
        md._save(name, {"day": md.now_ms // DAY_MS, "map": {**(cached.get("map") or {}), **out}})
    return out


def funding_from_db(con: sqlite3.Connection, coins: Sequence[str]) -> dict[str, dict[int, float]]:
    out: dict[str, dict[int, float]] = defaultdict(dict)
    for coin in coins:
        for hour_ts, rate in con.execute("SELECT hour_ts, rate_nanos FROM hl_funding_rate WHERE coin = ?", (coin,)):
            out[coin][hour_ts * 1000] = rate / NANOS
    return out


# ── путь цены после входа ────────────────────────────────────────────────────

@dataclass
class Path:
    """Всё, что нужно для выхода по любым тейку и стопу, — без повторного
    прохода по свечам: накопленный максимум и минимум (монотонны — поиск
    касания уровня двоичный)."""
    interval: str
    sec: int
    entry_t: int
    entry_px: float
    starts: array          # начало каждой свечи пути (мс); [0] — момент входа
    cummax: array
    negcummin: array       # −накопленный минимум: неубывающий
    times_exit: dict[int, tuple[int, float]]  # выход по времени T мин → (момент, цена)
    whale_exit: tuple[int, float] | None
    end_t: int             # до какого момента путь известен
    funding: list[tuple[int, float]]
    open_end: bool = False  # срока нет: не дошли до тейка — позиция открыта на конец данных


def price_at(c: list, sec: int, t: int) -> float | None:
    """Цена в момент t: внутри свечи — линейно от открытия к закрытию; в
    дыре между свечами — закрытие последней."""
    i = bisect.bisect_right(c, (t, math.inf)) - 1
    if i < 0:
        return None
    t0, o, _h, _l, cl = c[i]
    if t >= t0 + sec * 1000:
        return cl
    return o + (cl - o) * (t - t0) / (sec * 1000)


def build_path(sig: Signal, delay: int, m: Market, hold_ms: int, holds: Sequence[int], slip: float,
               funding: dict[int, float]) -> Path | None:
    entry_t = sig.ts + delay * 1000
    for interval, sec in INTERVALS:
        c = m.by_interval.get(interval) or []
        # hold_ms = 0 — срока нет («пересиживаем всегда»): путь — до конца
        # данных этого интервала (у Hyperliquid — до «сейчас»).
        horizon = entry_t + hold_ms if hold_ms else (c[-1][0] + sec * 1000 if c else 0)
        if not c or c[0][0] > entry_t or c[-1][0] + sec * 1000 < horizon or horizon <= entry_t:
            continue
        px = price_at(c, sec, entry_t)
        if px is None:
            continue
        side = sig.side
        entry_px = px * (1 + side * slip)
        i0 = bisect.bisect_right(c, (entry_t, math.inf)) - 1
        starts, hi, lo = array("q"), array("d"), array("d")
        for k in range(i0, len(c)):
            t0, _o, h, low, _cl = c[k]
            if t0 >= horizon:
                break
            if k == i0:  # свеча входа: только движение против нас
                t0 = entry_t
                h, low = (entry_px, low) if side > 0 else (h, entry_px)
            starts.append(t0)
            hi.append(max(h, hi[-1]) if hi else h)
            lo.append(min(low, lo[-1]) if lo else low)
        negmin = array("d", (-x for x in lo))
        times_exit = {}
        for t_min in holds:
            t = entry_t + t_min * 60_000
            p = price_at(c, sec, t)
            if p is not None:
                times_exit[t_min] = (t, p)
        whale_exit = None
        if sig.whale_close is not None:
            t = sig.whale_close + delay * 1000
            if entry_t < t < horizon:
                p = price_at(c, sec, t)
                if p is not None:
                    whale_exit = (t, p)
        cap = price_at(c, sec, horizon)
        times_exit[-1] = (horizon, cap if cap is not None else c[-1][4])
        fund = sorted((h, r) for h, r in funding.items() if entry_t < h <= horizon)
        return Path(interval, sec, entry_t, entry_px, starts, hi, negmin, times_exit, whale_exit, horizon, fund,
                    open_end=not hold_ms)
    return None


@dataclass(frozen=True)
class Exits:
    tp: float      # доля, 0 — выкл
    sl: float      # доля, 0 — выкл
    hold: int      # минут, 0 — выкл (тогда — предел удержания)
    whale: bool    # выходить, когда кит закрыл


def exit_trade(p: Path, side: int, ex: Exits, slip: float, fee: float, liq: float = 0.0,
               liq_loss: float = 1.0) -> tuple[int, float, str, float]:
    """Момент выхода, доход (доля от размера позиции, после комиссий и
    фандинга), причина и худший ход против нас до выхода (доля, MAE). Стоп —
    в начале свечи касания, тейк — в конце: при обоих в одной свече
    побеждает стоп. liq — ход цены против нас, на котором биржа ликвидирует
    (доля; 0 — не считать): тогда потеряно liq_loss (доля от размера: маржа
    позиции или доля счёта при кросс-марже) и комиссия входа; стоп ближе
    ликвидации — сработает раньше неё. Срока нет (open_end) и тейк не
    достигнут — «открыта»: доход — по рынку на конец данных, без комиссии
    выхода (позиция не закрыта)."""
    events: list[tuple[int, int, float, str, int]] = []  # (момент, приоритет, цена, причина, свеча)
    sec_ms = p.sec * 1000
    last = len(p.starts) - 1

    def adverse(move: float) -> int:
        level = p.entry_px * (1 - side * move)
        return bisect.bisect_left(p.negcummin, -level) if side > 0 else bisect.bisect_left(p.cummax, level)

    def candle(t: int) -> int:
        return max(0, min(last, bisect.bisect_right(p.starts, t) - 1))

    if ex.sl:
        i = adverse(ex.sl)
        if i <= last:
            events.append((p.starts[i], 0, p.entry_px * (1 - side * ex.sl), "стоп", i))
    if liq > 0:
        i = adverse(liq)
        if i <= last:
            events.append((p.starts[i], 1, p.entry_px * (1 - side * liq), "ликвидация", i))
    if ex.tp:
        level = p.entry_px * (1 + side * ex.tp)
        i = (bisect.bisect_left(p.cummax, level) if side > 0 else bisect.bisect_left(p.negcummin, -level))
        if i <= last:
            # В свече входа тейка не бывает (там учтено только движение против
            # нас), поэтому i ≥ 1 и свеча начинается с границы интервала.
            events.append((p.starts[i] + sec_ms, 2, level, "тейк", i))
    if ex.hold and ex.hold in p.times_exit:
        t, px = p.times_exit[ex.hold]
        events.append((t, 3, px, "время", candle(t)))
    if ex.whale and p.whale_exit is not None:
        t, px = p.whale_exit
        events.append((t, 3, px, "кит закрыл", candle(t)))
    t, px = p.times_exit[-1]
    events.append((t, 4, px, "открыта" if p.open_end else "предел удержания", last))
    t, _prio, px, why, idx = min(events)
    worst = -p.negcummin[idx] if side > 0 else p.cummax[idx]
    mae = max(0.0, side * (1 - worst / p.entry_px))
    if why == "ликвидация":
        return t, -liq_loss - fee, why, mae
    out = px if why == "открыта" else px * (1 - side * slip)
    gross = side * (out / p.entry_px - 1)
    fund = side * sum(r for h, r in p.funding if h <= t)
    return t, gross - (fee if why == "открыта" else 2 * fee) - fund, why, mae


# ── итоги ────────────────────────────────────────────────────────────────────

@dataclass
class Trade:
    sig: Signal
    entry_t: int
    exit_t: int
    pnl_pct: float
    pnl_usd: float
    why: str
    interval: str
    mae_usd: float = 0.0   # худший ход против нас до выхода, $ (просадка внутри сделки)

    @property
    def open(self) -> bool:
        return self.why == "открыта"


@dataclass
class Stats:
    trades: int = 0
    wins: int = 0
    avg_win: float = 0.0
    avg_loss: float = 0.0
    pf: float = 0.0
    net: float = 0.0
    max_dd: float = 0.0
    worst_streak: int = 0
    worst_streak_usd: float = 0.0
    open_trades: int = 0       # не дошли до тейка — открыты на конец данных
    unrealized: float = 0.0    # их результат по рынку (входит в net)
    max_mae: float = 0.0       # самая глубокая просадка внутри одной сделки, $
    longest_h: float = 0.0     # самая долгая сделка, ч (открытая — до конца данных)

    def row(self) -> dict:
        return {"сделок": self.trades, "прибыльных_%": round(100 * self.wins / self.trades, 1) if self.trades else 0,
                "ср_прибыль_$": round(self.avg_win, 2), "ср_убыток_$": round(self.avg_loss, 2),
                "profit_factor": round(self.pf, 2) if math.isfinite(self.pf) else "inf",
                "итог_$": round(self.net, 2), "просадка_$": round(self.max_dd, 2),
                "худшая_серия": self.worst_streak, "худшая_серия_$": round(self.worst_streak_usd, 2),
                "открытых": self.open_trades, "открытые_$": round(self.unrealized, 2),
                "просадка_в_сделке_$": round(self.max_mae, 2), "дольше_всех_ч": round(self.longest_h, 1)}


def stats_json(st: Stats) -> dict:
    """Те же итоги для приложения: ключи латиницей, PF без бесконечности
    (убытков не было — null)."""
    return {"trades": st.trades, "win_pct": round(100 * st.wins / st.trades, 1) if st.trades else 0.0,
            "avg_win": round(st.avg_win, 2), "avg_loss": round(st.avg_loss, 2),
            "pf": round(st.pf, 3) if math.isfinite(st.pf) else None, "net": round(st.net, 2),
            "max_dd": round(st.max_dd, 2), "worst_streak": st.worst_streak,
            "worst_streak_usd": round(st.worst_streak_usd, 2), "open": st.open_trades,
            "unrealized": round(st.unrealized, 2), "max_mae": round(st.max_mae, 2), "longest_h": round(st.longest_h, 1)}


def stats(trades: Sequence[Trade]) -> Stats:
    s = Stats(trades=len(trades))
    if not trades:
        return s
    wins = [t.pnl_usd for t in trades if t.pnl_usd > 0]
    losses = [t.pnl_usd for t in trades if t.pnl_usd <= 0]
    s.wins = len(wins)
    s.avg_win = sum(wins) / len(wins) if wins else 0.0
    s.avg_loss = sum(losses) / len(losses) if losses else 0.0
    gl = -sum(losses)
    s.pf = sum(wins) / gl if gl > 0 else (math.inf if wins else 0.0)
    s.net = sum(t.pnl_usd for t in trades)
    s.open_trades = sum(t.open for t in trades)
    s.unrealized = sum(t.pnl_usd for t in trades if t.open)
    s.max_mae = max(t.mae_usd for t in trades)
    s.longest_h = max(t.exit_t - t.entry_t for t in trades) / HOUR_MS
    eq = peak = 0.0
    run = run_usd = 0
    for t in sorted(trades, key=lambda x: (x.exit_t, x.entry_t)):
        # Просадка счёта — и внутри сделки (пересиживаем: позиция могла уйти
        # против нас глубоко, прежде чем дошла до тейка), а не только по
        # закрытым.
        s.max_dd = max(s.max_dd, peak - (eq - t.mae_usd))
        eq += t.pnl_usd
        peak = max(peak, eq)
        s.max_dd = max(s.max_dd, peak - eq)
        if t.pnl_usd <= 0:
            run += 1
            run_usd += t.pnl_usd
            if run > s.worst_streak or (run == s.worst_streak and run_usd < s.worst_streak_usd):
                s.worst_streak, s.worst_streak_usd = run, run_usd
        else:
            run = run_usd = 0
    return s


@dataclass(frozen=True)
class Filters:
    min_notional: float    # общий объём сговора за окно (один кит — его ордер и прежние)
    min_account: float
    max_leverage: int
    need_pnl30: bool
    cooldown_min: int
    min_wallets: int = 1   # сколько разных китов открыли ту же сторону за окно
    max_chase: float = 0.0  # насколько цена входа может быть хуже средней цены китов (доля; 0 — без предела)

    def ok(self, s: Signal) -> bool:
        if s.wallets < self.min_wallets or s.cluster_notional < self.min_notional:
            return False
        if self.min_account and s.account < self.min_account:  # неизвестный счёт (0) — не проходит
            return False
        if self.max_leverage and not 0 < s.leverage <= self.max_leverage:
            return False
        return not (self.need_pnl30 and s.pnl30 <= 0)


@dataclass(frozen=True)
class Sizing:
    """Размер и риск: маржа — доля депозита, позиция = маржа × плечо; не
    больше max_open позиций одновременно (по всем бумагам).

    Ликвидация. Изолированная маржа: убыток съел маржу позиции до
    поддерживающей — ход против нас 1/плечо − maint, потеряна маржа. Кросс-
    маржа: позицию держит весь счёт (на позицию — его доля, если позиций
    может быть несколько) — ход против нас доля счёта / позиция − maint,
    потеряна эта доля счёта."""
    deposit: float
    margin_frac: float
    leverage: float
    max_open: int
    maint: float
    cross: bool = False

    @property
    def notional(self) -> float:
        return self.deposit * self.margin_frac * self.leverage

    @property
    def backing(self) -> float:
        """Сколько денег держит одну позицию (доля от её размера)."""
        if self.cross:
            return self.deposit / max(1, self.max_open) / self.notional
        return 1 / self.leverage

    @property
    def liq_move(self) -> float:
        return max(0.0, self.backing - self.maint)


@dataclass(frozen=True)
class Params:
    delay: int
    filters: Filters
    exits: Exits

    def json(self) -> dict:
        f, e = self.filters, self.exits
        return {"delay": self.delay, "min_notional": f.min_notional, "min_account": f.min_account,
                "max_leverage": f.max_leverage, "pnl30": f.need_pnl30, "cooldown": f.cooldown_min,
                "wallets": f.min_wallets, "chase": round(f.max_chase * 100, 3),
                "tp": round(e.tp * 100, 3), "sl": round(e.sl * 100, 3), "hold": e.hold, "whale": e.whale}

    def row(self) -> dict:
        f, e = self.filters, self.exits
        return {"D_сек": self.delay, "китов_от": f.min_wallets, "объём_от_$": f.min_notional,
                "догонять_до_%": round(f.max_chase * 100, 3) or "выкл", "счёт_от_$": f.min_account,
                "плечо_до": f.max_leverage or "выкл", "PnL30>0": "да" if f.need_pnl30 else "выкл",
                "пауза_мин": f.cooldown_min, "тейк_%": round(e.tp * 100, 3) or "выкл",
                "стоп_%": round(e.sl * 100, 3) or "выкл", "по_времени_мин": e.hold or "выкл",
                "кит_закрыл": "да" if e.whale else "выкл"}


def simulate(signals: Sequence[Signal], paths: dict[int, Path], prm: Params, slip: float, fee: float,
             sz: Sizing) -> list[Trade]:
    """Сделки по набору параметров: фильтр, пауза на бумагу, одна позиция на
    бумагу, не больше sz.max_open позиций сразу (сигнал при занятых местах
    пропускается)."""
    trades: list[Trade] = []
    last_entry: dict[str, int] = {}
    busy_until: dict[str, int] = {}
    open_until: list[int] = []
    cool = prm.filters.cooldown_min * 60_000
    chase = prm.filters.max_chase
    for idx, s in enumerate(signals):
        p = paths.get(idx)
        if p is None or not prm.filters.ok(s):
            continue
        if p.entry_t < busy_until.get(s.coin, 0) or p.entry_t - last_entry.get(s.coin, -cool - 1) < cool:
            continue
        # Цена ушла от китов: ниже их средней для лонга (выше — для шорта) —
        # входим всегда; в другую сторону дальше chase — поздно, пропуск.
        if chase and s.ref_px > 0 and s.side * (p.entry_px / s.ref_px - 1) > chase:
            continue
        open_until = [u for u in open_until if u > p.entry_t]
        if sz.max_open and len(open_until) >= sz.max_open:
            continue
        # Депозит кончился (закрытые к этому моменту съели его) — торговать
        # нечем: дальше сделок нет. Без этого модель «торговала» бы в долг.
        if sz.deposit + sum(t.pnl_usd for t in trades if t.exit_t <= p.entry_t) <= 0:
            break
        t, pct, why, mae = exit_trade(p, s.side, prm.exits, slip, fee, sz.liq_move, sz.backing)
        last_entry[s.coin] = p.entry_t
        busy_until[s.coin] = t
        open_until.append(t)
        trades.append(Trade(s, p.entry_t, t, pct, pct * sz.notional, why, p.interval, mae * sz.notional))
    return trades


# ── время суток: рынок США ───────────────────────────────────────────────────

def us_open(ms: int) -> bool:
    """Основная сессия NYSE/Nasdaq: пн–пт 9:30–16:00 по Нью-Йорку (праздники
    не учитываются). Летнее время — по правилам США с 2007 года."""
    utc = datetime.fromtimestamp(ms / 1000, tz=timezone.utc)
    y = utc.year

    def nth_sunday(month: int, n: int) -> datetime:
        d = datetime(y, month, 1, tzinfo=timezone.utc)
        d += timedelta(days=(6 - d.weekday()) % 7 + 7 * (n - 1))
        return d

    dst = nth_sunday(3, 2) + timedelta(hours=7) <= utc < nth_sunday(11, 1) + timedelta(hours=6)
    ny = utc - timedelta(hours=4 if dst else 5)
    minutes = ny.hour * 60 + ny.minute
    return ny.weekday() < 5 and 570 <= minutes < 960


# ── прогон ───────────────────────────────────────────────────────────────────

def pct_list(s: str) -> list[float]:
    return [float(x) / 100 for x in s.split(",") if x.strip()]


def num_list(s: str) -> list[float]:
    return [float(x) for x in s.split(",") if x.strip()]


def int_list(s: str) -> list[int]:
    return [int(float(x)) for x in s.split(",") if x.strip()]


def inside(child: str, parent: str) -> bool:
    child, parent = os.path.realpath(child), os.path.realpath(parent)
    return os.path.commonpath([child, parent]) == parent


def run(a: argparse.Namespace, fetch: Fetch | None = None, now_ms: int | None = None) -> dict:
    bot_dir = os.path.dirname(os.path.realpath(a.db))
    for what, path in (("отчёт", a.out), ("кеш свечей", a.cache)):
        if inside(path, bot_dir):
            raise SystemExit(f"{what} ({path}) — в каталоге бота ({bot_dir}): туда скрипт не пишет")
    os.makedirs(a.out, exist_ok=True)

    con = open_ro(a.db)
    since = 0 if not a.since_days else (now_ms or int(time.time() * 1000)) - a.since_days * DAY_MS
    signals = load_signals(con, None if a.markets == "all" else a.dexes, since, a.cluster_hours * 3_600_000,
                           a.min_whale)
    # Свечи и пути — только для ордеров, которые могут пройти правило китов:
    # остальные не станут сделкой ни при каком наборе сетки.
    raw_signals = len(signals)
    k_min, n_min = min(int_list(a.cluster)), min(num_list(a.min_notional))
    signals = [s for s in signals if s.wallets >= k_min and s.cluster_notional >= n_min]
    if a.count_only:
        con.close()
        text = count_report(signals, raw_signals, k_min, a)
        sys.stdout.write(text)
        return {"text": text, "signals": len(signals)}
    md = MarketData(a.cache, a.offline, fetch, a.pause, now_ms)
    delays, holds = int_list(a.delays), int_list(a.hold)
    # Срока нет (--max-hold 0, правило владельца — «пересиживаем всегда»):
    # путь — до конца данных; позиция, не дошедшая до тейка, — «открыта».
    hold_ms = max([*holds, a.max_hold]) * 60_000 if a.max_hold else 0
    db_funding = funding_from_db(con, sorted({s.coin for s in signals}))
    con.close()
    if a.fee is None:
        a.fee = 0.05 if a.venue == "okx" else 0.09
    # Есть ли бумаги китов на OKX и под каким именем — считаем всегда (кроме
    # --offline без кеша): это ответ на вопрос «можно ли торговать на OKX».
    okx = okx_map(md, sorted({s.coin for s in signals})) if (a.venue == "okx" or not a.offline) else {}
    if a.venue == "okx":
        # Торгуем на OKX: только бумаги, сверенные по цене; цены китов — в
        # единицах OKX (kPEPE → PEPE: делим на 1000).
        signals = [s for s in signals if okx.get(s.coin, {}).get("status") == "ok"]
        for s in signals:
            scale = float(okx[s.coin]["scale"])
            s.px, s.ref_px = s.px / scale, s.ref_px / scale

    by_coin: dict[str, list[int]] = defaultdict(list)
    for i, s in enumerate(signals):
        by_coin[s.coin].append(i)
    funding_src: dict[str, str] = {}
    paths: dict[int, dict[int, Path]] = {d: {} for d in delays}
    markets: dict[str, tuple[Market, dict[int, float]]] = {}
    no_data = 0
    for coin, idxs in sorted(by_coin.items()):
        start = signals[idxs[0]].ts
        end = signals[idxs[-1]].ts + max(delays) * 1000 + hold_ms if hold_ms else md.now_ms
        if a.venue == "okx":
            inst = str(okx[coin]["inst"])
            market = md.okx_candles(inst, start, end)
            fund, funding_src[coin] = md.okx_funding(inst, start, end)
        else:
            market = md.candles(coin, start, end)
            fund, funding_src[coin] = md.funding(coin, start, end, db_funding.get(coin, {}))
        markets[coin] = (market, fund)
        for i in idxs:
            got = False
            for d in delays:
                p = build_path(signals[i], d, market, hold_ms, holds, a.slippage / 100, fund)
                if p is not None:
                    paths[d][i] = p
                    got = True
            no_data += not got

    if not signals:
        raise SystemExit(f"сигналов нет: из {raw_signals} открытий китов правило «от {k_min} китов в одну сторону» "
                         "не прошло ни одно")
    t_lo, t_hi = signals[0].ts, signals[-1].ts
    cut = t_lo + (t_hi - t_lo) * 3 // 4
    train = [i for i, s in enumerate(signals) if s.ts < cut]
    hold_out = [i for i, s in enumerate(signals) if s.ts >= cut]

    filters = [Filters(x, a.min_account, a.max_leverage, bool(p), a.cooldown, k, c)
               for x in num_list(a.min_notional) for p in int_list(a.pnl30)
               for k in int_list(a.cluster) for c in pct_list(a.chase)]
    exits = [Exits(tp, sl, h, w) for tp in pct_list(a.tp) for sl in pct_list(a.sl) for h in holds
             for w in (bool(x) for x in int_list(a.whale_exit))]
    fee, slip = a.fee / 100, a.slippage / 100
    if not 0 < a.leverage <= a.max_market_leverage:
        raise SystemExit(f"плечо {a.leverage} — вне 0…{a.max_market_leverage} (предел рынка)")
    sizing = Sizing(a.deposit, a.margin_pct / 100, a.leverage, a.max_open, 1 / (2 * a.max_market_leverage),
                    cross=a.margin == "cross")

    def subset(idx: list[int], d: int) -> tuple[list[Signal], dict[int, Path]]:
        return [signals[i] for i in idx], {k: paths[d][i] for k, i in enumerate(idx) if i in paths[d]}

    grid: list[tuple[Params, Stats]] = []
    for d in delays:
        sigs, ps = subset(train, d)
        for f in filters:
            for e in exits:
                prm = Params(d, f, e)
                grid.append((prm, stats(simulate(sigs, ps, prm, slip, fee, sizing))))
    ranked = sorted((g for g in grid if g[1].trades >= a.min_train_trades), key=lambda g: (g[1].net, g[1].pf), reverse=True)
    top = ranked[:10]

    def on(idx: list[int], prm: Params) -> list[Trade]:
        sigs, ps = subset(idx, prm.delay)
        return simulate(sigs, ps, prm, slip, fee, sizing)

    def passes(st: Stats) -> bool:
        return st.net > 0 and st.pf >= 1.3 and st.trades >= 100 and st.max_dd <= 0.15 * a.deposit

    def against(idx: list[int], prm: Params) -> list[Trade]:
        """То же самое, но в обратную сторону от китов. Тейк без стопа
        выигрывает часто и на случайной цене (редкий застрявший убыток
        перекрывает много тейков) — преимущество китов есть, только если
        повторять за ними лучше, чем делать наоборот."""
        flipped = [replace(signals[i], side=-signals[i].side) for i in idx]
        ps: dict[int, Path] = {}
        for k, sig in enumerate(flipped):
            market, fund = markets[sig.coin]
            p = build_path(sig, prm.delay, market, hold_ms, holds, slip, fund)
            if p is not None:
                ps[k] = p
        return simulate(flipped, ps, prm, slip, fee, sizing)

    by_chase: list[dict] = []
    pace: list[dict] = []
    checked = [(prm, st, stats(on(hold_out, prm))) for prm, st in top]
    best = checked[0] if checked else None
    fade = stats(against(hold_out, best[0])) if best else Stats()
    # Критерий ТЗ и сверх него: за китами лучше, чем против них.
    verdict = bool(best and passes(best[2]) and best[2].net > fade.net)

    # ── отчёт ──
    lines: list[str] = []
    w = lines.append
    fmt = lambda ms: datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")  # noqa: E731
    w("Бэктест автокопирования китов — фьючерсы Hyperliquid (этап 1): "
      + ("все — крипта, акции, металлы" if a.markets == "all" else "акции (" + ", ".join(a.dexes) + ")"))
    w(f"Торговля — на {'OKX' if a.venue == 'okx' else 'Hyperliquid'}; сигналы — сделки китов на Hyperliquid.")
    if okx:
        okx_ok = sorted(c for c, m in okx.items() if m.get("status") == "ok")
        okx_bad = sorted(c for c, m in okx.items() if m.get("status") == "mismatch")
        okx_no = sorted(c for c, m in okx.items() if m.get("status") in ("missing", "unverified"))
        w(f"OKX: есть {len(okx_ok)} из {len(okx)} бумаг китов"
          + (f"; другой актив с тем же именем (цена не сходится): {', '.join(okx_bad)}" if okx_bad else "")
          + (f"; нет на OKX: {', '.join(okx_no[:40])}{' …' if len(okx_no) > 40 else ''}" if okx_no else ""))
        renamed = sorted(f"{c}→{m['inst'].removesuffix('-USDT-SWAP')}" for c, m in okx.items()
                         if m.get("status") == "ok" and m["inst"].removesuffix("-USDT-SWAP") != c.split(":")[-1])
        if renamed:
            w("На OKX под другим именем: " + ", ".join(renamed))
    w(f"Период сигналов: {fmt(t_lo)} — {fmt(t_hi)}; подбор — до {fmt(cut)}, отложенные — после.")
    w(f"Ордеров китов: {raw_signals}; из них проходят правило китов (от {k_min} в одну сторону): {len(signals)}; "
      f"без свечей: {no_data}.")
    res = defaultdict(int)
    for i in range(len(signals)):
        p = paths[delays[0]].get(i)
        res[p.interval if p else "нет"] += 1
    w("Свечи (по сигналам, D=" + str(delays[0]) + "): " + ", ".join(f"{k} — {v}" for k, v in sorted(res.items())))
    w("Фандинг: " + ", ".join(f"{c}: {s}" for c, s in sorted(funding_src.items())))
    w(f"Сигнал — открытие лонга или шорта кита (закрытия не сигнал); сговор — разные киты в ту же сторону "
      f"за {a.cluster_hours:g} ч, у каждого позиция от {a.min_whale:g} $; цена хуже средней китов дальше "
      f"предела — вход пропускается.")
    tps = [x for x in pct_list(a.tp) if x]
    if tps:
        w("Тейк: " + ", ".join(f"{x * 100:g} % движения цены = {x * 100 * a.leverage:g} % на маржу" for x in tps)
          + f" (плечо {a.leverage:g}×).")
    w(f"Маржа {'кросс' if sizing.cross else 'изолированная'}. "
      f"Депозит {a.deposit:.0f} $, маржа на сделку {a.margin_pct:g} % ({a.deposit * sizing.margin_frac:.0f} $), плечо "
      f"{a.leverage:g}× — позиция {sizing.notional:.0f} $; одновременно позиций до {a.max_open or 'без предела'}; "
      f"ликвидация при ходе против позиции {sizing.liq_move * 100:.1f} % (поддерживающая маржа "
      f"{sizing.maint * 100:.1f} % — половина начальной при пределе рынка {a.max_market_leverage:g}×).")
    w(f"Комиссия тейкера {a.fee} % с каждой стороны, проскальзывание {a.slippage} %; запросов к API: {md.requests}"
      + (f", сбоев: {len(md.errors)}" if md.errors else ""))
    w("")
    if not best:
        w(f"Ни один набор не набрал {a.min_train_trades} сделок на периоде подбора — судить не о чем.")
    else:
        w(f"ВЕРДИКТ: {'МОЖНО ДАЛЬШЕ (бумажная торговля)' if verdict else 'НЕТ — дальше не идём'}")
        w("Критерий на отложенных данных: итог > 0, profit factor ≥ 1,3, сделок ≥ 100, просадка ≤ "
          f"{0.15 * a.deposit:.0f} $ (15 % депозита). Лучший набор подбора на отложенных: "
          + ", ".join(f"{k} {v}" for k, v in best[2].row().items()))
        w(f"Из лучших 10 критерий на отложенных проходят: {sum(passes(c[2]) for c in checked)}.")
        if a.max_hold == 0:
            ho = best[2]
            w(f"Пересиживаем (срока нет): на отложенных не дошли до тейка и открыты {ho.open_trades} "
              f"({ho.unrealized:.2f} $ по рынку); самая долгая сделка — {ho.longest_h:.1f} ч; худшая просадка "
              f"внутри сделки — {ho.max_mae:.2f} $ ({ho.max_mae / a.deposit * 100:.1f} % депозита). Пока позиция "
              f"открыта, новых сделок нет (одна позиция за раз).")
        w(f"Против китов (тот же набор, обратная сторона) на отложенных: сделок {fade.trades}, прибыльных "
          f"{fade.row()['прибыльных_%']} %, итог {fade.net:.2f} $ — за китами "
          + ("лучше" if best[2].net > fade.net else "НЕ лучше: преимущества китов нет, только форма стратегии")
          + ". Тейк без стопа часто выигрывает и на случайной цене: смотрите на редкие большие убытки.")
        w("")
        w("Лучшие 10 наборов (подбор → отложенные):")
        for n, (prm, st, ho) in enumerate(checked, 1):
            w(f" {n:2}. " + ", ".join(f"{k} {v}" for k, v in prm.row().items()))
            w("     подбор: " + ", ".join(f"{k} {v}" for k, v in st.row().items()))
            w("     отлож.: " + ", ".join(f"{k} {v}" for k, v in ho.row().items()))
        prm = best[0]
        all_trades = on(list(range(len(signals))), prm)
        w("")
        w("Лучший набор на всём периоде: " + ", ".join(f"{k} {v}" for k, v in stats(all_trades).row().items()))

        def breakdown(title: str, key: Callable[[Trade], str], limit: int = 0) -> None:
            groups: dict[str, list[Trade]] = defaultdict(list)
            for t in all_trades:
                groups[key(t)].append(t)
            items = sorted(groups.items(), key=lambda kv: -len(kv[1]))
            w("")
            w(title)
            for k, ts in items[:limit or None]:
                st = stats(ts)
                row = st.row()
                w(f"  {k}: сделок {st.trades}, прибыльных {row['прибыльных_%']} %, итог {st.net:.2f} $, "
                  f"PF {row['profit_factor']}")

        breakdown("По бумагам:", lambda t: t.sig.coin)
        breakdown("По китам (15 самых частых):", lambda t: t.sig.wallet, 15)
        breakdown("По времени суток:", lambda t: "рынок США открыт" if us_open(t.entry_t) else "рынок США закрыт")
        breakdown("По причине выхода:", lambda t: t.why)
        w("")
        days = max(1.0, (t_hi - t_lo) / DAY_MS)
        w(f"Сколько сделок в день (лучший набор, весь период {days:.0f} дн., возможностей входа "
          f"{len(episodes(signals, a.cooldown))}):")
        for mo in (1, 3, 10):
            sz_mo = replace(sizing, max_open=mo)
            sigs_all = list(range(len(signals)))
            tr = simulate([signals[i] for i in sigs_all], {k: paths[prm.delay][i] for k, i in enumerate(sigs_all)
                                                         if i in paths[prm.delay]}, prm, slip, fee, sz_mo)
            st_mo = stats(tr)
            tp_h = sorted((t.exit_t - t.entry_t) / HOUR_MS for t in tr if t.why == "тейк")
            med = tp_h[len(tp_h) // 2] if tp_h else 0.0
            eq, bust = a.deposit, 0
            for t in sorted(tr, key=lambda x: x.exit_t):
                eq += t.pnl_usd
                if eq <= 0:
                    bust = t.exit_t
                    break
            w(f"  позиций сразу {mo} (маржа {mo * a.margin_pct:g} % депозита): сделок {len(tr)} — "
              f"{len(tr) / days:.1f} в день; до тейка обычно {med:.1f} ч; открыты на конец {st_mo.open_trades}; "
              f"итог {st_mo.net:.2f} $; просадка {st_mo.max_dd:.2f} $"
              + (f"; ДЕПОЗИТ КОНЧИЛСЯ {fmt(bust)}" if bust else ""))
            pace.append({"max_open": mo, "trades": len(tr), "per_day": round(len(tr) / days, 2),
                         "median_tp_h": round(med, 2), "open": st_mo.open_trades, "net": round(st_mo.net, 2),
                         "max_dd": round(st_mo.max_dd, 2), "bust": bust})
        w("")
        w("По пределу догона (лучший набор подбора для каждого предела; подбор → отложенные):")
        for chase in pct_list(a.chase):
            cand = [g for g in ranked if g[0].filters.max_chase == chase]
            if not cand:
                w(f"  {chase * 100:g} %: нет набора с {a.min_train_trades}+ сделками")
                continue
            ho = stats(on(hold_out, cand[0][0]))
            w(f"  {chase * 100:g} %: подбор {cand[0][1].net:.2f} $ ({cand[0][1].trades} сделок), "
              f"отложенные {ho.net:.2f} $ ({ho.trades} сделок, PF {ho.row()['profit_factor']})")
            by_chase.append({"chase": round(chase * 100, 3), "train": stats_json(cand[0][1]), "holdout": stats_json(ho)})
        w("")
        w("По задержке D (лучший набор подбора для каждой D; на всём периоде и только на минутных свечах):")
        for d in delays:
            cand = [g for g in ranked if g[0].delay == d]
            if not cand:
                w(f"  D={d} с: нет набора с {a.min_train_trades}+ сделками")
                continue
            tr = on(list(range(len(signals))), cand[0][0])
            m1 = [t for t in tr if t.interval == "1m"]
            w(f"  D={d} с: итог {stats(tr).net:.2f} $ ({len(tr)} сделок); на минутных — {stats(m1).net:.2f} $ ({len(m1)})")

        with open(os.path.join(a.out, "trades.csv"), "w", newline="", encoding="utf-8") as f:
            wr = csv.writer(f)
            wr.writerow(["бумага", "кит", "сторона", "сделка_кита_UTC", "вход_UTC", "выход_UTC", "объём_кита_$",
                         "счёт_кита_$", "плечо_кита", "PnL30_кита_$", "доход_%", "доход_$", "выход", "свечи",
                         "рынок_США", "отложенные"])
            for t in all_trades:
                s = t.sig
                wr.writerow([s.coin, s.wallet, "лонг" if s.side > 0 else "шорт", fmt(s.ts), fmt(t.entry_t), fmt(t.exit_t),
                             round(s.notional, 2), round(s.account, 2), s.leverage, round(s.pnl30, 2),
                             round(t.pnl_pct * 100, 4), round(t.pnl_usd, 2), t.why, t.interval,
                             "открыт" if us_open(t.entry_t) else "закрыт", "да" if s.ts >= cut else "нет"])
    with open(os.path.join(a.out, "grid.csv"), "w", newline="", encoding="utf-8") as f:
        rows = [{**prm.row(), **st.row()} for prm, st in grid]
        if rows:
            wr2 = csv.DictWriter(f, fieldnames=list(rows[0]))
            wr2.writeheader()
            wr2.writerows(rows)
    with open(os.path.join(a.out, "top10.csv"), "w", newline="", encoding="utf-8") as f:
        rows = [{**prm.row(), **{f"подбор_{k}": v for k, v in st.row().items()},
                 **{f"отлож_{k}": v for k, v in ho.row().items()}} for prm, st, ho in checked]
        if rows:
            wr3 = csv.DictWriter(f, fieldnames=list(rows[0]))
            wr3.writeheader()
            wr3.writerows(rows)
    text = "\n".join(lines) + "\n"
    with open(os.path.join(a.out, "report.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    summary = build_summary(a, sizing, signals, cut, res, funding_src, md, checked, verdict,
                            on(list(range(len(signals))), best[0]) if best else [], passes, fade if best else None)
    summary["okx"] = {"venue": a.venue, "map": okx}
    summary["by_chase"] = by_chase
    summary["pace"] = pace
    summary["entries_per_day"] = round(len(episodes(signals, a.cooldown)) / max(1.0, (t_hi - t_lo) / DAY_MS), 2)
    with open(os.path.join(a.out, "summary.json.tmp"), "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False)
    os.replace(os.path.join(a.out, "summary.json.tmp"), os.path.join(a.out, "summary.json"))
    return {"summary": summary, "text": text, "verdict": verdict, "signals": len(signals), "grid": grid, "top": checked,
            "requests": md.requests}


def episodes(signals: Sequence[Signal], cooldown_min: int) -> list[Signal]:
    """Возможности входа: первый ордер, прошедший правило китов, на бумаге;
    следующие на той же бумаге раньше паузы — тот же случай, не новый."""
    last: dict[str, int] = {}
    out = []
    for s in signals:
        if s.ts - last.get(s.coin, -10**18) >= cooldown_min * 60_000:
            out.append(s)
            last[s.coin] = s.ts
    return out


def count_report(signals: Sequence[Signal], raw: int, k_min: int, a: argparse.Namespace) -> str:
    """--count-only: сколько возможностей входа по правилу китов в день — без
    сети и без свечей (только чтение базы), за секунды. Это верхняя граница
    сделок: при одной позиции за раз, пока она не дошла до тейка, новые не
    открываются."""
    ep = episodes(signals, a.cooldown)
    by_day: dict[str, int] = defaultdict(int)
    by_coin: dict[str, int] = defaultdict(int)
    by_hour: dict[int, int] = defaultdict(int)
    for s in ep:
        d = datetime.fromtimestamp(s.ts / 1000, tz=timezone.utc)
        by_day[d.strftime("%Y-%m-%d")] += 1
        by_coin[s.coin] += 1
        by_hour[d.hour] += 1
    days = max(1, len(by_day) if not ep else (ep[-1].ts - ep[0].ts) // DAY_MS + 1)
    lines = [f"Ордеров китов: {raw}; проходят правило (от {k_min} китов в одну сторону за {a.cluster_hours:g} ч, "
             f"у каждого от {a.min_whale:g} $): {len(signals)}; возможностей входа (пауза {a.cooldown} мин на "
             f"бумагу): {len(ep)} за {days} дн. — в среднем {len(ep) / days:.1f} в день.",
             "По дням (UTC): " + ", ".join(f"{d} — {n}" for d, n in sorted(by_day.items())[-30:]),
             "По часам (UTC): " + ", ".join(f"{h:02d} — {by_hour[h]}" for h in range(24)),
             "Бумаги: " + ", ".join(f"{c} — {n}" for c, n in sorted(by_coin.items(), key=lambda kv: -kv[1])[:30]),
             "Это верхняя граница: сделка — только если цена не ушла от средней китов дальше предела, и пока "
             "открытая позиция не дошла до тейка, новые не открываются (одна за раз)."]
    return "\n".join(lines) + "\n"


def build_summary(a: argparse.Namespace, sizing: Sizing, signals: Sequence[Signal], cut: int, res: dict,
                  funding_src: dict[str, str], md: MarketData, checked: list, verdict: bool,
                  trades: list[Trade], passes: Callable[[Stats], bool], fade_st: Stats | None = None) -> dict:
    """Итог для закрытой вкладки приложения (API читает summary.json): то же,
    что в отчёте, машиночитаемо. Адреса китов — сокращённо."""
    def short(w: str) -> str:
        return w[:6] + "…" + w[-4:] if len(w) > 12 else w

    def group(key: Callable[[Trade], str], limit: int = 0) -> list[dict]:
        g: dict[str, list[Trade]] = defaultdict(list)
        for t in trades:
            g[key(t)].append(t)
        items = sorted(g.items(), key=lambda kv: -len(kv[1]))[:limit or None]
        return [{"key": k, **stats_json(stats(v))} for k, v in items]

    eq, curve = 0.0, []
    for t in sorted(trades, key=lambda x: (x.exit_t, x.entry_t)):
        eq += t.pnl_usd
        curve.append([t.exit_t, round(eq, 2)])
    if len(curve) > 500:  # для графика хватит 500 точек; последняя — всегда
        curve = [*curve[::len(curve) // 500 + 1], curve[-1]]
    return {
        "generated_at": int(time.time()),
        "verdict": verdict,
        "criteria": {"net_gt": 0, "pf_ge": 1.3, "trades_ge": 100, "max_dd_le": round(0.15 * a.deposit, 2),
                     "beats_fade": True},
        "period": {"from": signals[0].ts if signals else 0, "to": signals[-1].ts if signals else 0, "cut": cut},
        "sizing": {"deposit": a.deposit, "margin_pct": a.margin_pct, "leverage": a.leverage, "max_open": a.max_open,
                   "cross": sizing.cross, "markets": a.markets, "cluster_hours": a.cluster_hours,
                   "min_whale": a.min_whale,
                   "notional": sizing.notional, "liq_move_pct": round(sizing.liq_move * 100, 3),
                   "fee_pct": a.fee, "slippage_pct": a.slippage},
        "data": {"signals": len(signals), "candles": dict(res), "requests": md.requests, "errors": len(md.errors),
                 "funding": funding_src},
        "top": [{"params": prm.json(), "train": stats_json(st), "holdout": stats_json(ho), "passes": passes(ho)}
                for prm, st, ho in checked],
        "full": stats_json(stats(trades)) if trades else None,
        "fade": stats_json(fade_st) if fade_st is not None else None,
        "equity": curve,
        "by_coin": group(lambda t: t.sig.coin),
        "by_whale": group(lambda t: short(t.sig.wallet), 15),
        "by_session": group(lambda t: "open" if us_open(t.entry_t) else "closed"),
        "by_exit": group(lambda t: t.why),
        "trades": [{"coin": t.sig.coin, "side": t.sig.side, "whale": short(t.sig.wallet), "entry": t.entry_t,
                    "exit": t.exit_t, "pct": round(t.pnl_pct * 100, 3), "usd": round(t.pnl_usd, 2), "why": t.why,
                    "holdout": t.sig.ts >= cut} for t in sorted(trades, key=lambda x: x.entry_t)[-100:]],
    }


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description="Бэктест автокопирования китов по акциям Hyperliquid (только чтение)")
    ap.add_argument("--db", default=os.environ.get("WHALE_HL_DB_FILE", "hyperliquid.db"), help="база бота (только чтение)")
    ap.add_argument("--out", default="", help="каталог отчёта (по умолчанию — новый временный)")
    ap.add_argument("--cache", default=os.path.join(tempfile.gettempdir(), "whale-autotrade-cache"),
                    help="кеш свечей и фандинга (копится между запусками)")
    ap.add_argument("--offline", action="store_true", help="без сети: только кеш")
    ap.add_argument("--count-only", action="store_true",
                    help="только посчитать возможности входа по правилу китов по дням (без сети и свечей)")
    ap.add_argument("--pause", type=float, default=1.0, help="пауза между запросами к API, с")
    ap.add_argument("--markets", choices=("all", "stocks"), default="all",
                    help="all — все фьючерсы (крипта, акции, металлы); stocks — только площадки --dexes")
    ap.add_argument("--dexes", type=lambda s: tuple(x for x in s.split(",") if x), default=STOCK_DEXES)
    ap.add_argument("--cluster", default="2,3",
                    help="китов в одну сторону за окно, от (сетка; правило владельца — минимум 2)")
    ap.add_argument("--min-whale", type=float, default=1000,
                    help="кит считается, если его позиция в эту сторону за окно — от X $ (правило владельца — 1000)")
    ap.add_argument("--cluster-hours", type=float, default=2, help="окно сговора, ч (правило владельца — 2)")
    # Предел догона: тейк — 1 % от нашего входа; цена, уже ушедшая от средней
    # китов на треть цели (0,3 %), — вход «по хаям». 0,1 % режет почти всё
    # (за D секунд цена почти всегда чуть сдвигается), 0,5 % — уже догонялки.
    # Владелец выбрал 0,3 %; сетку можно задать (отчёт: «По пределу догона»).
    ap.add_argument("--chase", default="0.3",
                    help="на сколько %% цена входа может быть хуже средней цены китов (сетка; 0 — без предела)")
    ap.add_argument("--margin", choices=("cross", "isolated"), default="cross", help="кросс- или изолированная маржа")
    ap.add_argument("--since-days", type=int, default=0, help="только последние N дней (0 — все)")
    ap.add_argument("--delays", default="5,30,120,300", help="задержка входа D, с (сетка)")
    ap.add_argument("--min-notional", default="0", help="общий объём китов за окно от X $ (сетка; 0 — без предела)")
    ap.add_argument("--min-account", type=float, default=0, help="счёт кита от Y $ (0 — выкл)")
    ap.add_argument("--max-leverage", type=int, default=0, help="плечо кита до Z (0 — выкл)")
    ap.add_argument("--pnl30", default="1,0", help="требовать PnL кита за 30 дней > 0: 1 — да, 0 — нет (сетка)")
    ap.add_argument("--cooldown", type=int, default=60, help="не чаще одного входа на бумагу за N минут")
    ap.add_argument("--tp", default="1",
                    help="тейк, %% движения цены (правило владельца — 1 %%: при 10× это 10 %% на маржу; 0 — выкл; сетка)")
    ap.add_argument("--whale-exit", default="0", help="выходить, когда кит закрыл: 1 — да, 0 — нет (сетка)")
    ap.add_argument("--sl", default="0", help="стоп, %% (0 — выкл — пересиживаем; сетка)")
    ap.add_argument("--hold", default="0", help="выход по времени, мин (0 — выкл; сетка)")
    ap.add_argument("--max-hold", type=int, default=0,
                    help="предел удержания, мин; 0 — нет (правило владельца — пересиживаем всегда: не дошла до тейка — "
                         "позиция открыта на конец данных, результат по рынку)")
    ap.add_argument("--slippage", type=float, default=0.05, help="проскальзывание, %% на каждую сторону")
    ap.add_argument("--venue", choices=("hl", "okx"), default="hl",
                    help="где торговать: hl — Hyperliquid; okx — OKX (только бумаги, сверенные по цене; свечи и фандинг OKX)")
    ap.add_argument("--fee", type=float, default=None,
                    help="комиссия тейкера, %% на каждую сторону (по умолчанию: Hyperliquid 0,09 — HIP-3 до двойной "
                         "базовой 0,045; OKX 0,05)")
    ap.add_argument("--deposit", type=float, default=10000, help="условный депозит, $")
    ap.add_argument("--margin-pct", type=float, default=10, help="маржа на сделку, %% депозита")
    ap.add_argument("--leverage", type=float, default=10, help="плечо")
    ap.add_argument("--max-open", type=int, default=1, help="позиций одновременно, по всем бумагам (0 — без предела)")
    ap.add_argument("--max-market-leverage", type=float, default=10,
                    help="предел плеча рынка: поддерживающая маржа — половина начальной при нём")
    ap.add_argument("--min-train-trades", type=int, default=30, help="наборы с меньшим числом сделок в подборе не ранжируются")
    return ap


def main(argv: Sequence[str] | None = None) -> int:
    a = parser().parse_args(argv)
    if not a.out:
        a.out = tempfile.mkdtemp(prefix="autotrade-backtest-")
    r = run(a)
    sys.stdout.write(r["text"])
    print(f"\nФайлы: {a.out}/report.txt, trades.csv, grid.csv, top10.csv")
    return 0


if __name__ == "__main__":
    sys.exit(main())
