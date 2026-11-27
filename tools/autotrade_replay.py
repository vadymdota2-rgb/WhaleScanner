#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Бэктест-повтор по актуальным правилам живого бота: история сделок китов из
базы бота (только чтение) прогоняется через ТОТ ЖЕ код сигналов, что у
службы торговли (autotrade_live.Signals: окна китов, момент — сделка кита,
«входим с крупными», убыток и добор кита), и через те же правила входа и
выхода:

- от 2 китов в одну сторону за 2 часа, у каждого от 1000 $; на момент входа —
  тоже от 2; против — киты крупнее или поровну — не входим, крупных от 2 —
  входим с ними; кит сигнала уже вышел в убыток — не входим;
- MA30 15-минутного графика: лонг — цена выше MA30, шорт — ниже (после
  уровней — они важнее);
- уровни графика 4ч: лонг — только ниже 40 % диапазона цены за 24 свечи 4ч
  (4 дня; у поддержки), шорт — выше 60 % (у сопротивления), середина — нет;
- рейтинг китов — по сделкам повтора, закрытым к моменту решения: выше наш
  винрейт с китами сигнала — раньше; кит выше 90 % (от 5 сделок) — с ним и в
  одиночку, без «против крупнее», уровней и MA30;
- бумага есть на OKX (USDT-SWAP), цена сходится с китами (±2 %), не хуже их
  средней больше чем на 0,3 %; в бумагу — не чаще раза в 3 часа; своих
  позиций — до 3, на разных бумагах (капитал и рейтинг — по сделкам,
  закрытым к моменту входа);
- маржа 5 % капитала × 10×, объём — по контракту и шагу OKX (меньше
  минимума — пропуск); вход — по рынку (тейкер, проскальзывание), но не хуже
  средней цены китов ±0,3 % (как IOC с границей у живого бота); вариант —
  лимитом по цене решения (мейкер, 5 минут);
- выход: лимитный тейк от цены входа (lv.TP); по рынку — стоп (core.STOP_PCT
  от цены входа; вариант — за уровнем) или кит сигнала закрыл в убыток;
  усреднения нет.

Цены — 5-минутные свечи OKX (публичное API, без ключа; история — годы, по
100 свечей за запрос), только под кандидатов и сделки, с кешем блоками по
100 свечей. Ход — в progress.json (служба показывает его во вкладке).
Итог — summary.json (его рисует вкладка) и report.txt.

Допущения: цена решения — открытие 5-минутной свечи; внутри свечи при
одновременном тейке и стопе (или выходе за китом) — стоп/выход (хуже);
фандинг не учитывается.

    python3 tools/autotrade_replay.py --db ~/WhaleScanner/hyperliquid.db --since-days 30
"""
from __future__ import annotations

import argparse
import heapq
import json
import math
import os
import sqlite3
import sys
import tempfile
import threading
import time
from collections import defaultdict
from collections.abc import Callable, Iterator, Sequence
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from decimal import ROUND_CEILING, ROUND_FLOOR, Decimal
from typing import Any

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autotrade_core as core
import autotrade_live as lv

BAR_MS = 300_000                  # 5-минутные свечи OKX
LIMIT_WAIT_MS = 5 * 60_000        # вариант «вход лимитом»: ордер ждёт исполнения 5 минут
SYNC_MS = 15_000                  # живой бот сверяет счёт раз в 15 с
Fetch = Callable[[dict[str, Any]], object]


def okx_fetch(body: dict[str, Any]) -> object:
    return core.okx_public(str(body["okx"]), body.get("params") or {})


def candle_ok(r: object) -> bool:
    return isinstance(r, list) and len(r) == 5 and all(
        isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x) for x in r) and min(r[1:]) > 0


class Limiter:
    """Запросов к OKX — не чаще одного в interval секунд на все потоки
    (публичные свечи OKX: 20 запросов за 2 с с адреса)."""

    def __init__(self, interval: float) -> None:
        self.interval = interval
        self.lock = threading.Lock()
        self.next = 0.0

    def wait(self) -> None:
        with self.lock:
            now = time.monotonic()
            at = max(now, self.next)
            self.next = at + self.interval
        if at > now:
            time.sleep(at - now)


class Prices:
    """5-минутные свечи OKX по блокам из 100 (один запрос на блок), кеш на
    диске по бумагам; без сети (--offline) — только кеш. В кеш навсегда —
    только ответ OKX за уже закончившийся блок: сбой сети или незаконченный
    блок — лишь на этот прогон (иначе пустой блок от сбоя остался бы навсегда).

    С пулом (pool) — блок запрашивается вместе с соседними (предыдущий — для
    MA30 и уровней, два следующих — повтор идёт вперёд) параллельно; частота
    — общий Limiter; сбой — до 3 попыток."""

    def __init__(self, cache: str, fetch: Fetch, pause: float, offline: bool, now_ms: int,
                 bar: str = "5m", bar_ms: int = BAR_MS, pool: ThreadPoolExecutor | None = None,
                 limiter: Limiter | None = None) -> None:
        self.cache, self.fetch, self.pause, self.offline, self.now_ms = cache, fetch, pause, offline, now_ms
        self.pool, self.limiter = pool, limiter or Limiter(pause)
        self.pending: dict[tuple[str, int], Future[list[Any] | None]] = {}
        self.bar, self.bar_ms, self.block_ms = bar, bar_ms, 100 * bar_ms
        self.blocks: dict[str, dict[int, list[list[float]]]] = {}
        self.fresh: dict[tuple[str, int], list[list[float]]] = {}
        self.dirty: set[str] = set()
        self.index: dict[tuple[str, int], dict[int, list[float]]] = {}   # свеча по времени начала — без перебора блока
        self.requests = 0
        self.errors = 0
        os.makedirs(cache, exist_ok=True)

    def _file(self, inst: str) -> str:
        return os.path.join(self.cache, f"replay_{core.safe_name(inst)}_{self.bar}.json")

    def _load(self, inst: str) -> dict[int, list[list[float]]]:
        if inst not in self.blocks:
            got: dict[int, list[list[float]]] = {}
            try:
                with open(self._file(inst), encoding="utf-8") as f:
                    raw = json.load(f)
                if isinstance(raw, dict):
                    for k, v in raw.items():
                        if isinstance(v, list) and str(k).isdigit():
                            got[int(k)] = [c for c in v if candle_ok(c)]  # испорченный кеш — без мусора
            except (OSError, ValueError):
                pass
            self.blocks[inst] = got
        return self.blocks[inst]

    def save(self) -> None:
        for inst in self.dirty:
            p = self._file(inst)
            with open(p + ".tmp", "w", encoding="utf-8") as f:
                json.dump({str(k): v for k, v in self.blocks[inst].items()}, f)
            os.replace(p + ".tmp", p)
        self.dirty.clear()

    def block(self, inst: str, b: int) -> list[list[float]]:
        """Свечи блока b: [[время, open, high, low, close], …] по времени."""
        have = self._load(inst)
        end = (b + 1) * self.block_ms
        if b in have:
            return have[b]
        if (inst, b) in self.fresh:
            return self.fresh[(inst, b)]
        if self.offline:
            return []
        fut = self.pending.pop((inst, b), None)
        if fut is None:
            self.requests += 1
            rows = self._get(inst, b)
        else:
            rows = fut.result()
        self._prefetch(inst, b)
        if not isinstance(rows, list):
            self.errors += 1
        out: list[list[float]] = []
        for r in rows if isinstance(rows, list) else []:
            try:
                c = [int(r[0]), float(r[1]), float(r[2]), float(r[3]), float(r[4])]
            except (IndexError, TypeError, ValueError):
                continue
            if b * self.block_ms <= c[0] < end and candle_ok(c):
                out.append(c)
        out.sort()
        if isinstance(rows, list) and end <= self.now_ms:
            have[b] = out
            self.dirty.add(inst)
        else:
            self.fresh[(inst, b)] = out
        return out

    def _get(self, inst: str, b: int) -> list[Any] | None:
        """Запрос блока к OKX (и из потока пула): сбой или не список — ещё
        попытка (до 3, пауза 1 и 2 с); не вышло — None."""
        for attempt in range(3):
            if attempt:
                time.sleep(attempt)
            self.limiter.wait()
            try:
                rows = self.fetch({"okx": "/api/v5/market/history-candles",
                                   "params": {"instId": inst, "bar": self.bar, "after": (b + 1) * self.block_ms,
                                              "limit": 100}})
            except Exception:  # noqa: BLE001 — сеть: сбой — ещё попытка, потом блок без свечей
                rows = None
            if isinstance(rows, list):
                return rows
        return None

    def _prefetch(self, inst: str, b: int) -> None:
        """Соседние блоки — заранее, параллельно (только с пулом)."""
        if self.pool is None:
            return
        have = self.blocks.get(inst, {})
        for nb in (b - 1, b + 1, b + 2):
            if nb < 0 or nb * self.block_ms >= self.now_ms or nb in have or (inst, nb) in self.fresh \
                    or (inst, nb) in self.pending:
                continue
            self.requests += 1
            self.pending[(inst, nb)] = self.pool.submit(self._get, inst, nb)

    def candle(self, inst: str, t: int) -> list[float] | None:
        """Свеча, в которую попадает момент t."""
        b = t // self.block_ms
        idx = self.index.get((inst, b))
        if idx is None:
            idx = self.index[(inst, b)] = {int(c[0]): c for c in self.block(inst, b)}
        return idx.get(t // self.bar_ms * self.bar_ms)

    def candles(self, inst: str, start: int, end: int) -> Iterator[list[float]]:
        """Свечи с момента start до end — по блокам, по мере надобности."""
        b = start // self.block_ms
        while b * self.block_ms < end:
            for c in self.block(inst, b):
                if c[0] + self.bar_ms > start and c[0] < end:
                    yield c
            b += 1


def instruments(fetch: Fetch, offline: bool, cache: str) -> dict[str, dict[str, Any]]:
    """Каталог USDT-SWAP OKX (публичный): контракт, шаг, минимум, шаг цены."""
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, "replay_instruments.json")
    got: object = None
    if not offline:
        try:
            got = fetch({"okx": "/api/v5/public/instruments", "params": {"instType": "SWAP"}})
        except Exception:  # noqa: BLE001 — нет сети — кеш
            got = None
    if isinstance(got, list) and got:
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            json.dump(got, f)
        os.replace(path + ".tmp", path)
    else:
        try:
            with open(path, encoding="utf-8") as f:
                got = json.load(f)
        except (OSError, ValueError):
            got = []
    return {str(r["instId"]): r for r in got if isinstance(r, dict) and str(r.get("instId", "")).endswith("-USDT-SWAP")
            and r.get("state", "live") == "live"} if isinstance(got, list) else {}


def coin_per_contract(ins: dict[str, Any], px: float) -> float:
    """Монет в одном контракте: размер в монете — как есть, в долларах — по цене."""
    ct = lv.num(ins.get("ctVal")) * lv.num(ins.get("ctMult"), 1.0)
    return ct / px if str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT") and px > 0 else ct




@dataclass
class Trade:
    coin: str
    side: int
    entry_t: int
    exit_t: int
    entry_px: float     # цена первого входа (с проскальзыванием)
    tp: float           # тейк
    exit_px: float      # цена выхода; открытая — цена на конец данных
    notional: float     # $ по цене входа
    pnl: float          # $ с комиссиями
    why: str            # тейк / стоп / кит вышел в убыток / открыта
    mae: float          # худший ход против нас внутри сделки, $
    wallets: tuple[str, ...] = ()   # киты сигнала — для рейтинга

    @property
    def open(self) -> bool:
        return self.why == "открыта"


@dataclass(frozen=True)
class Rules:
    """Тейк, предел стопа и фильтры повтора: по умолчанию — как у живого
    бота; варианты (VARIANTS) — по одному изменению, на тех же сделках китов."""
    name: str = "текущие правила"
    tp: float = lv.TP
    stop_max: float = core.STOP_PCT   # стоп, доля от входа (за уровнем — предел; бесконечность — только уровень)
    at_level: bool = False            # стоп за уровнем 4ч (не дальше stop_max), а не ровно stop_max от входа
    chase: float = lv.CHASE           # не хуже средней цены китов больше чем на эту долю
    levels: bool = True               # фильтр уровней 4ч (стоп за уровнем — всегда)
    ma: bool = True                   # фильтр MA30 15м
    max_open: int = lv.MAX_OPEN       # своих позиций одновременно
    pause_ms: int = lv.COIN_PAUSE_MS  # в бумагу — не чаще
    limit: bool = False               # вход лимитом по цене решения (мейкер, 5 мин) — вариант; по умолчанию — по рынку, как живой
    flip: bool = False                # против китов: сторона сделки — обратная сигналу, выхода вслед за китом нет


# Сравнение (--variants): что дало бы одно изменение правил на тех же китах —
# чтобы менять тейк, стоп и фильтры по данным, а не наугад.
VARIANTS = (Rules("тейк +0,5 %", tp=0.005), Rules("тейк +1 %", tp=0.01), Rules("тейк +1,5 %", tp=0.015),
            Rules("стоп 1,5 %", stop_max=0.015), Rules("стоп 5 %", stop_max=0.05),
            Rules("стоп за уровнем, не дальше 3 %", at_level=True),
            Rules("стоп только за уровнем", at_level=True, stop_max=math.inf),
            Rules("без уровней 4ч", levels=False), Rules("без MA30", ma=False), Rules("1 позиция", max_open=1),
            Rules("пауза бумаги 30 мин", pause_ms=30 * 60_000), Rules("вход лимитом (мейкер, 5 мин)", limit=True),
            Rules("вход не хуже цены китов", chase=0.0), Rules("против китов", flip=True))


# Разбор сигналов (--variants): куда идёт цена после сигнала — в сторону
# китов, %, через 15 мин, 1 ч, 4 ч и 24 ч; по группам (число китов, объём,
# «против — крупнее», уровни, MA30, ушла ли цена от китов). Повтор сигнала
# той же бумаги и стороны чаще раза в 15 минут — не в счёт (одна волна китов).
HORIZONS = ((15 * 60_000, "r15m"), (3_600_000, "r1h"), (4 * 3_600_000, "r4h"), (86_400_000, "r24h"))
STUDY_GAP_MS = 15 * 60_000


class Study:
    """Итоги разбора по группам: число сигналов, средний ход цены и доля
    сигналов, после которых цена пошла в сторону китов."""

    def __init__(self) -> None:
        self.last: dict[tuple[str, int], int] = {}
        self.groups: dict[str, list[list[float]]] = {}   # группа → [сигналов], [сумма %], [в сторону китов], [с ценой]

    def add(self, keys: Sequence[str], rets: Sequence[float | None]) -> None:
        for k in keys:
            g = self.groups.setdefault(k, [[0.0], [0.0] * len(HORIZONS), [0.0] * len(HORIZONS), [0.0] * len(HORIZONS)])
            g[0][0] += 1
            for i, r in enumerate(rets):
                if r is not None:
                    g[1][i] += r
                    g[2][i] += r > 0
                    g[3][i] += 1

    def rows(self) -> list[dict[str, Any]]:
        out = []
        for k, (n, s, up, c) in self.groups.items():
            row: dict[str, Any] = {"key": k, "n": int(n[0])}
            for i, (_h, name) in enumerate(HORIZONS):
                row[name] = round(s[i] / c[i], 3) if c[i] else None
            row["up1h"] = round(100 * up[1] / c[1], 1) if c[1] else None
            row["up4h"] = round(100 * up[2] / c[2], 1) if c[2] else None
            out.append(row)
        return out


class Replay:
    """Повтор: строки сделок открытия по времени → Signals живого бота →
    правила входа; открытая позиция ведётся по свечам до выхода."""

    def __init__(self, a: argparse.Namespace, prices: Prices, instr: dict[str, dict[str, Any]],
                 progress: Callable[[dict[str, Any]], None], hours: Prices | None = None,
                 rules: Rules | None = None, shared: Replay | None = None) -> None:
        self.a, self.px, self.instr, self.progress = a, prices, instr, progress
        self.rules = rules or Rules()
        # Свечи 4ч — для уровней (24 свечи, 4 дня); кеш и сеть — те же, что у 5-минутных.
        self.hours = hours or Prices(prices.cache, prices.fetch, prices.pause, prices.offline, prices.now_ms,
                                     bar="4H", bar_ms=core.LEVEL_BAR_MS)
        # Варианты идут одним проходом (replay_all): окна китов и запросы к базе
        # о выходах китов — общие с основным повтором (shared), своё — капитал,
        # позиции, рейтинг и пропуски.
        self.sig: lv.Signals = shared.sig if shared is not None else lv.Signals(a.db, keep=True)
        # Расчёты на момент решения (выход китов, объёмы за и против, уровни,
        # MA30) — одни на все варианты: ключ — сигнал и момент; чистится на
        # каждой строке (replay_all).
        self.memo: dict[Any, Any] = shared.memo if shared is not None else {}
        self.rows_cache: dict[Any, list[tuple[str, int, int]]] = shared.rows_cache if shared is not None else {}
        self.priority: set[str] = set()   # свои приоритетные киты (рейтинг этого варианта)
        self.study: Study | None = None    # разбор сигналов — только у основного и только с --variants
        self.equity = float(a.deposit)
        self.trades: list[Trade] = []
        self.entered: dict[str, int] = {}
        self.busy: list[tuple[int, str]] = []          # свои позиции: (время выхода, бумага); открытая — 10^18
        self.closing: list[tuple[int, int]] = []       # куча (время выхода, № сделки): ещё не в капитале и рейтинге
        self.skips: defaultdict[str, int] = defaultdict(int)
        self.rating = core.Rating()   # наш винрейт с китами — по закрытым сделкам повтора

    # правила входа — как Trader.try_enter, без биржи
    def decide(self, s: dict[str, Any], now: int) -> str:
        self.settle(now)
        self.busy = [b for b in self.busy if b[0] > now]
        if len(self.busy) >= self.rules.max_open:
            return "own_open"
        if now - self.entered.get(s["coin"], -10**15) < self.rules.pause_ms:
            return "coin_pause"
        # Бумаги нет на OKX — сразу (живой бот проверяет это позже, итог тот же,
        # а повтор не тратит запрос к базе на каждый такой сигнал).
        base, scale = core.okx_candidate(s["coin"])
        inst = f"{base}-USDT-SWAP"
        ins = self.instr.get(inst)
        if not ins:
            return "no_inst"
        if any(b[1] == inst for b in self.busy):
            return "own_same"
        usd, against, count = self.cached(("bal", s["coin"], s["side"], now), lambda: self.sig.balance(s["coin"], s["side"], now))
        prio = self.sig.priority_in(s["coin"], s["side"], s.get("wallets"), now, self.priority)   # приоритетный — без этих правил
        if against > 0 and against >= usd and not prio:
            return "bigger_against"
        if count < lv.MIN_WHALES and not prio:
            return "stale"
        if self.whale_loss(s, now):
            return "whale_exit"
        c = self.px.candle(inst, now)
        if c is None:
            return "no_price"
        last = c[1]  # цена на момент решения — открытие свечи
        hl_px = last * scale
        if abs(hl_px / s["ref_px"] - 1) > lv.MATCH_TOL:
            return "other_asset"
        side = -s["side"] if self.rules.flip else s["side"]   # сторона сделки
        if side * (hl_px / s["ref_px"] - 1) > self.rules.chase:
            return "chase"
        lo, hi = self.cached(("lvl", inst, now), lambda: self.range_levels(inst, now))
        if self.rules.levels and not prio and core.level_ok(lo, hi, last, side) is False:
            return "level"
        if self.rules.ma and not prio and core.ma_ok(self.cached(("ma", inst, now), lambda: self.ma30_15m(inst, now, last)),
                                                     last, side) is False:
            return "ma30"
        if self.equity <= 0:
            return "no_equity"
        sz = lv.contract_size(ins, self.equity * lv.MARGIN_FRAC * lv.LEVERAGE, last)  # как у живого бота
        if sz <= 0:
            return "min_size"
        tick = lv.dec(ins.get("tickSz"))
        if tick is None:
            return "bad_params"
        tp = self.rules.tp
        tp_try = lv.round_step(last * (1 + side * tp), tick, ROUND_CEILING if side > 0 else ROUND_FLOOR)
        if tp_try <= 0 or not tp * 0.999 <= side * (float(tp_try) / last - 1) <= tp + lv.TP_SLACK:
            return "tick"
        stop = core.entry_stop(lo, hi, last, side, self.rules.stop_max) if self.rules.at_level \
            else core.fixed_stop(last, side, self.rules.stop_max)
        if not self.open_trade(s, inst, ins, sz, last, now, stop, side):
            return "limit_miss"
        return ""

    def open_trade(self, s: dict[str, Any], inst: str, ins: dict[str, Any], sz: Decimal, last: float, now: int,
                   stop: float, side: int) -> bool:
        """Сделка (сторона side) от входа до выхода по свечам; False — лимит
        входа не исполнился."""
        a = self.a
        slip = a.slippage / 100
        rnd = ROUND_CEILING if side > 0 else ROUND_FLOOR
        start, entry, fee_in = now, last * (1 + side * slip), a.fee_taker
        # По рынку — как живой (IOC с границей): не хуже средней китов ± chase,
        # проскальзывание за границу не идёт.
        bound = s["ref_px"] / core.okx_candidate(s["coin"])[1] * (1 + side * self.rules.chase)
        if side * (entry - bound) > 0:
            entry = bound
        if self.rules.limit:
            # Лимит по цене решения: исполнится, если за 5 минут цена (свечи ПОСЛЕ
            # решения) дошла до него; мейкер, без проскальзывания.
            got = next((c for c in self.px.candles(inst, now, now + LIMIT_WAIT_MS + 1)
                        if c[0] > now and side * (c[3 if side > 0 else 2] - last) <= 0), None)
            if got is None:
                return False
            start, entry, fee_in = int(got[0]), last, a.fee_maker
        tick = lv.dec(ins.get("tickSz")) or Decimal("0.0001")
        qty = float(sz) * coin_per_contract(ins, entry)      # монет в позиции
        avg_px = entry
        fees = qty * entry * fee_in / 100
        tp = float(lv.round_step(avg_px * (1 + side * self.rules.tp), tick, rnd))
        # Выход вслед за китом в убытке — только с китами (против китов — нет).
        exit_at = None if self.rules.flip else self.loss_exit_time(s["coin"], side, s.get("wallets") or {}, now)
        if exit_at is not None and exit_at + SYNC_MS <= start:
            return False   # кит закрылся в убытке, пока лимит ждал, — бот снял лимит (как живой)
        mae = 0.0
        why, t_out, px_out = "открыта", self.px.now_ms, avg_px
        for tf, o, h, lo, cl in self.px.candles(inst, start, self.px.now_ms):
            t = int(tf)
            end = t + BAR_MS
            # Кит вышел в убыток — бот замечает на сверке (до 15 с) и закрывает по рынку.
            if exit_at is not None and exit_at + SYNC_MS < end:
                why, t_out, px_out = "кит вышел в убыток", max(t, exit_at + SYNC_MS), cl * (1 - side * slip)
                fees += qty * px_out * a.fee_taker / 100
                break
            worst = lo if side > 0 else h
            # Стоп входа (3 % от входа; вариант — за уровнем) — по рынку (бот видит
            # на сверке; цена — стоп или хуже, если свеча открылась уже за ним).
            # Раньше тейка в той же свече — хуже для нас.
            if stop > 0 and side * (worst - stop) <= 0:
                why, t_out = "стоп", t
                px_out = (o if side * (o - stop) <= 0 else stop) * (1 - side * slip)
                mae = max(mae, qty * side * (avg_px - px_out))
                fees += qty * px_out * a.fee_taker / 100
                break
            mae = max(mae, qty * side * (avg_px - worst))
            best = h if side > 0 else lo
            if side * (best - tp) >= 0:
                why, t_out, px_out = "тейк", t, tp
                fees += qty * tp * a.fee_maker / 100
                break
            t_out, px_out = end, cl
        pnl = qty * side * (px_out - avg_px) - fees
        self.entered[s["coin"]] = start
        # Позиция занимает место до выхода; итог — в капитал и рейтинг только
        # к моменту выхода (settle): позиций до трёх, закрываются не по порядку.
        if why != "открыта":
            heapq.heappush(self.closing, (t_out, len(self.trades)))
        self.busy.append((t_out if why != "открыта" else 10**18, inst))
        self.trades.append(Trade(s["coin"], side, start, t_out, entry, tp, px_out, qty * avg_px, pnl, why, mae,
                                 tuple(sorted(w for w in s.get("wallets") or {} if isinstance(w, str)))))
        return True

    def study_signal(self, s: dict[str, Any], now: int) -> None:
        """Сигнал — в разбор: ход цены после него в сторону китов и группы."""
        st = self.study
        k = (s["coin"], s["side"])
        if st is None or now - st.last.get(k, -10**15) < STUDY_GAP_MS:
            return
        base, scale = core.okx_candidate(s["coin"])
        inst = f"{base}-USDT-SWAP"
        c = self.px.candle(inst, now) if inst in self.instr else None
        if c is None or abs(c[1] * scale / s["ref_px"] - 1) > lv.MATCH_TOL:
            return   # бумаги нет на OKX или это другой актив
        st.last[k] = now
        last, side = c[1], s["side"]
        rets: list[float | None] = []
        for h, _name in HORIZONS:
            c2 = self.px.candle(inst, now + h) if now + h < self.px.now_ms else None
            rets.append(side * (c2[1] / last - 1) * 100 if c2 is not None else None)
        usd, against, _count = self.cached(("bal", s["coin"], side, now), lambda: self.sig.balance(s["coin"], side, now))
        lo, hi = self.cached(("lvl", inst, now), lambda: self.range_levels(inst, now))
        lvl = core.level_ok(lo, hi, last, side)
        ma = core.ma_ok(self.cached(("ma", inst, now), lambda: self.ma30_15m(inst, now, last)), last, side)
        n = int(s["whales"])
        keys = ["все сигналы", f"китов {n}" if n < 4 else "китов 4 и больше",
                "объём китов до 10 тыс $" if s["usd"] < 10_000 else "объём китов 10–50 тыс $" if s["usd"] < 50_000
                else "объём китов от 50 тыс $",
                "против — киты крупнее" if 0 < usd <= against else "против — меньше или никого",
                "цена ушла от китов > 0,3 %" if side * (c[1] * scale / s["ref_px"] - 1) > lv.CHASE else "цена у цены китов",
                {True: "уровни: у своего", False: "уровни: у чужого", None: "уровней нет"}[lvl]]
        if ma is not None:
            keys.append("MA30: за сторону китов" if ma else "MA30: против стороны китов")
        st.add(keys, rets)

    def whale_loss(self, s: dict[str, Any], now: int) -> bool:
        """Кит сигнала уже закрыл в убыток (Signals.whale_loss) — один запрос к
        базе на сигнал и момент на все варианты."""
        key = ("loss", s["coin"], s["side"], tuple(sorted((s.get("wallets") or {}).items())), now)
        return bool(self.cached(key, lambda: bool(self.sig.whale_loss(s["coin"], s["side"], s.get("wallets") or {}, now))))

    def cached(self, key: Any, make: Callable[[], Any]) -> Any:
        """Общий для вариантов расчёт на момент решения: первый вариант
        считает, остальные берут готовое."""
        if key not in self.memo:
            self.memo[key] = make()
        return self.memo[key]

    def loss_exit_time(self, coin: str, side: int, wallets: dict[str, Any], since: int) -> int | None:
        """Когда кто-то из китов сигнала впервые оказался в убытке по закрытиям
        (как Signals.whale_loss, но по времени): сумма closed_pnl закрытий с
        его первой сделки в окне < 0 — после нашего входа."""
        run: dict[str, int] = defaultdict(int)
        for w, ts, p in self.whale_rows(coin, side, wallets):
            run[w] += p
            if ts > since and run[w] < 0:
                return ts
        return None

    def ma30_15m(self, inst: str, now: int, last: float) -> float:
        """MA30 15-минутного графика на момент now — как у живого бота: 29
        закрытых 15-минутных свечей (закрытие — у последней 5-минутки свечи) и
        идущая — по цене last. Свечи нет — 0 (MA нет)."""
        cur = now // core.MA_BAR_MS * core.MA_BAR_MS
        closes = []
        for k in range(1, core.MA_BARS):
            c = self.px.candle(inst, cur - (k - 1) * core.MA_BAR_MS - BAR_MS)
            if c is None:
                return 0.0
            closes.append(c[4])
        return (sum(closes) + last) / core.MA_BARS

    def range_levels(self, inst: str, now: int) -> tuple[float, float]:
        """Уровни графика 4ч на момент now — как у живого бота (24 свечи 4ч OKX
        с идущей): 23 закрытые свечи 4ч и идущая — по её закрытым 5-минуткам
        (идущая 5-минутка заглянула бы в будущее). Свечей нет — (0, 0)."""
        bar = now // core.LEVEL_BAR_MS * core.LEVEL_BAR_MS
        got = [c for c in self.hours.candles(inst, bar - core.LEVEL_MS + core.LEVEL_BAR_MS, bar) if c[0] + core.LEVEL_BAR_MS <= bar]
        got += [c for c in self.px.candles(inst, bar, now) if c[0] + BAR_MS <= now]
        return (min(c[3] for c in got), max(c[2] for c in got)) if got else (0.0, 0.0)

    def whale_rows(self, coin: str, side: int, wallets: dict[str, Any]) -> list[tuple[str, int, int]]:
        """Закрытия китов сигнала по бумаге (закрытие, разворот против нас,
        ликвидация) с их первой сделки в окне: (кошелёк, время, closed_pnl)."""
        since = {str(k): int(lv.num(v)) for k, v in wallets.items()}
        if not since:
            return []
        key = (coin, side, tuple(sorted(since.items())))
        got = self.rows_cache.get(key)
        if got is not None:
            return got   # тот же сигнал у другого варианта — без запроса к базе
        marks = ",".join("?" * len(since))
        codes = core.CLOSES_FOR[1 if side > 0 else -1]
        q = (f"SELECT wallet, ts, closed_pnl_nanos FROM hl_fills WHERE coin = ? AND wallet IN ({marks}) "  # noqa: S608 — только «?»
             f"AND dir_code IN ({','.join('?' * len(codes))}) AND ts >= ? "
             f"AND (dir_code != {core.FLIP} OR dir LIKE ?) ORDER BY ts, tid")
        args = (coin, *since, *codes, min(since.values()), "%Long > Short%" if side > 0 else "%Short > Long%")
        con = core.open_ro(self.a.db)
        try:
            rows = con.execute(q, args).fetchall()
        finally:
            con.close()
        out = [(str(w), int(t), int(v or 0)) for w, t, v in rows if int(t) >= since.get(str(w), 10**18)]
        self.rows_cache[key] = out
        return out

    def requests(self) -> int:
        return self.px.requests + self.hours.requests

    def settle(self, now: int) -> None:
        """Сделки, закрытые к моменту now: итог — в капитал (размер следующих
        входов — от него), киты — в рейтинг (как у живого бота по
        trades.jsonl). Позиций до трёх — закрываются не по порядку входа."""
        while self.closing and self.closing[0][0] <= now:
            _t, i = heapq.heappop(self.closing)
            t = self.trades[i]
            self.equity += t.pnl
            self.rating.add(t.wallets, t.pnl > 0)
            self.priority = self.rating.priority()

    def run(self, since: int, until: int) -> None:
        replay_all([self], since, until)


def replay_all(rps: Sequence[Replay], since: int, until: int) -> None:
    """Один проход по сделкам китов на все варианты (первый — основной): окна
    и сигналы — общие (Signals первого), решение — у каждого своё. Раньше
    каждый вариант читал всю историю заново — 13 проходов, часы."""
    main = rps[0]
    sig = main.sig
    con = core.open_ro(main.a.db)
    try:
        total = con.execute("SELECT COUNT(*) FROM hl_fills WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ?",
                            (since, until)).fetchone()[0]
        cur = con.execute(
            "SELECT tid, wallet, coin, dir_code, ts, notional_nanos, ABS(CAST(sz AS REAL)) FROM hl_fills "
            "WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ? ORDER BY ts, tid", (since, until))
        done = 0
        last_save = time.time()
        last_note = 0.0
        while rows := cur.fetchmany(5000):
            for row in rows:
                done += 1
                for rp in rps:
                    rp.settle(int(row[4]))
                # Одиночный сигнал приоритетного кита — если он приоритетный хоть у
                # одного варианта; остальные его пропускают (solo — не свой).
                sig.priority = set().union(*(rp.priority for rp in rps))
                # Строка — по одной: одновременных сигналов (порядок по нашему
                # винрейту с китами, как у живого бота в одном опросе) почти нет.
                for s in reversed(sig.feed([row])):  # как candidates: встречный «крупный» — первым
                    now = s["ts"] + main.a.delay * 1000
                    main.study_signal(s, now)
                    for rp in rps:
                        if s.get("solo") and s["solo"] not in rp.priority:
                            continue
                        why = rp.decide(s, now)
                        rp.skips[why or "entered"] += 1
                main.memo.clear()   # расчёты — на момент решения: дальше не нужны
            sig.seen.clear()  # повтор идёт по строкам один раз — повторов нет, память не копим
            if time.time() - last_note > 2:
                last_note = time.time()
                main.progress({"stage": "повтор", "done": done, "total": total, "trades": len(main.trades),
                               "requests": main.requests()})
                sig.sweep(int(rows[-1][4]))
            if time.time() - last_save > 60:   # кеш свечей — на диск раз в минуту (не каждые 2 с)
                last_save = time.time()
                main.px.save()
                main.hours.save()
    finally:
        con.close()
        sig.close()
    for rp in rps:
        rp.settle(10**17)   # закрытые к концу данных — в капитал (открытые — нет)
    main.px.save()
    main.hours.save()
    main.progress({"stage": "готово", "done": total, "total": total, "trades": len(main.trades),
                   "requests": main.requests()})


# ── итоги ────────────────────────────────────────────────────────────────────

def stats(trades: Sequence[Trade]) -> dict[str, Any]:
    """Итоги для вкладки: просадка — с провалами внутри сделок (позиция могла
    уйти против нас глубоко, прежде чем дошла до тейка или выхода)."""
    pnl = [t.pnl for t in trades]
    wins, losses = [x for x in pnl if x > 0], [x for x in pnl if x <= 0]
    eq = peak = dd = 0.0
    run = worst = 0
    run_usd = worst_usd = 0.0
    for t in sorted(trades, key=lambda x: (x.exit_t, x.entry_t)):
        dd = max(dd, peak - (eq - t.mae))
        eq += t.pnl
        peak = max(peak, eq)
        dd = max(dd, peak - eq)
        if t.pnl <= 0:
            run, run_usd = run + 1, run_usd + t.pnl
            if run > worst or (run == worst and run_usd < worst_usd):
                worst, worst_usd = run, run_usd
        else:
            run, run_usd = 0, 0.0
    gl = -sum(losses)
    return {"trades": len(pnl), "win_pct": round(100 * len(wins) / len(pnl), 1) if pnl else 0.0,
            "avg_win": round(sum(wins) / len(wins), 2) if wins else 0.0,
            "avg_loss": round(sum(losses) / len(losses), 2) if losses else 0.0,
            "pf": round(sum(wins) / gl, 3) if gl > 0 else None, "net": round(sum(pnl), 2), "max_dd": round(dd, 2),
            "worst_streak": worst, "worst_streak_usd": round(worst_usd, 2),
            "open": sum(t.open for t in trades), "unrealized": round(sum(t.pnl for t in trades if t.open), 2),
            "max_mae": round(max((t.mae for t in trades), default=0.0), 2),
            "longest_h": round(max(((t.exit_t - t.entry_t) / core.HOUR_MS for t in trades), default=0.0), 1)}


def us_open(ms: int) -> bool:
    """Рынок США открыт (9:30–16:00 по Нью-Йорку, пн–пт; летнее время — со
    второго воскресенья марта до первого воскресенья ноября)."""
    t = datetime.fromtimestamp(ms / 1000, tz=timezone.utc)

    def nth_sunday(month: int, n: int) -> datetime:
        d = datetime(t.year, month, 1, tzinfo=timezone.utc)
        d += timedelta(days=(6 - d.weekday()) % 7)
        return d + timedelta(weeks=n - 1)

    summer = nth_sunday(3, 2) + timedelta(hours=7) <= t < nth_sunday(11, 1) + timedelta(hours=6)
    ny = t - timedelta(hours=4 if summer else 5)
    return ny.weekday() < 5 and (9 * 60 + 30) <= ny.hour * 60 + ny.minute < 16 * 60


def summary(a: argparse.Namespace, rp: Replay, since: int, now: int) -> dict[str, Any]:
    trades = rp.trades
    cut = since + (now - since) * 3 // 4
    first, last = [t for t in trades if t.entry_t < cut], [t for t in trades if t.entry_t >= cut]
    st_all, st_last = stats(trades), stats(last)
    crit = {"pf_ge": 1.3, "trades_ge": 100, "max_dd_le": round(0.15 * a.deposit, 2)}  # и итог > 0
    verdict = st_all["net"] > 0 and st_last["net"] > 0 and (st_last["pf"] is None or st_last["pf"] >= crit["pf_ge"]) \
        and st_last["trades"] >= crit["trades_ge"] and st_last["max_dd"] <= crit["max_dd_le"]

    def group(key: Callable[[Trade], str]) -> list[dict[str, Any]]:
        g: dict[str, list[Trade]] = defaultdict(list)
        for t in trades:
            g[key(t)].append(t)
        return [{"key": k, **stats(v)} for k, v in sorted(g.items(), key=lambda kv: -len(kv[1]))]

    eq, curve = 0.0, []
    for t in sorted(trades, key=lambda x: (x.exit_t, x.entry_t)):
        eq += t.pnl
        curve.append([t.exit_t, round(eq, 2)])
    if len(curve) > 500:  # для графика хватит 500 точек; последняя — всегда
        curve = [*curve[::len(curve) // 500 + 1], curve[-1]]

    def row(t: Trade) -> dict[str, Any]:
        return {"coin": t.coin, "side": t.side, "entry": t.entry_t, "exit": t.exit_t,
                "px_in": t.entry_px, "px_out": t.exit_px, "tp": t.tp, "usd": round(t.pnl, 2),
                "pct": round(t.pnl / t.notional * 100, 3) if t.notional else 0.0, "why": t.why,
                "mae": round(t.mae, 2),
                "held_h": round((t.exit_t - t.entry_t) / core.HOUR_MS, 1)}

    return {
        "generated_at": int(time.time()),
        "verdict": verdict,
        "criteria": crit,
        "period": {"from": since, "to": now},
        "sizing": {"deposit": a.deposit, "deposit_end": round(rp.equity, 2), "margin_pct": lv.MARGIN_FRAC * 100,
                   "leverage": lv.LEVERAGE, "fee_taker_pct": a.fee_taker, "fee_maker_pct": a.fee_maker,
                   "slippage_pct": a.slippage},
        "data": {"signals": sum(rp.skips.values()), "requests": rp.requests(), "errors": rp.px.errors + rp.hours.errors},
        "full": st_all, "first": stats(first), "last": st_last,
        "equity": curve,
        "by_coin": group(lambda t: t.coin),
        "by_session": group(lambda t: "open" if us_open(t.entry_t) else "closed"),
        "by_exit": group(lambda t: t.why),
        "skips": dict(sorted(rp.skips.items(), key=lambda kv: -kv[1])),
        "per_day": round(len(trades) / max(1.0, (now - since) / core.DAY_MS), 2),
        "trades": [row(t) for t in sorted(trades, key=lambda x: x.entry_t)[-100:]],
        # Открытая позиция модели (не дошла до выхода к концу данных — это «сейчас»).
        "active": [{**row(t), "to_tp_pct": round(t.side * (t.tp / t.exit_px - 1) * 100, 3) if t.exit_px else None}
                   for t in trades if t.open],
    }


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description="Бэктест-повтор по актуальным правилам живого бота (только чтение базы)")
    ap.add_argument("--db", default=os.environ.get("WHALE_HL_DB_FILE", "hyperliquid.db"), help="база бота (только чтение)")
    ap.add_argument("--out", default="", help="каталог отчёта")
    ap.add_argument("--cache", default=os.path.join(tempfile.gettempdir(), "whale-autotrade-cache"), help="кеш свечей")
    ap.add_argument("--offline", action="store_true", help="без сети: только кеш")
    ap.add_argument("--pause", type=float, default=0.15, help="пауза между запросами к OKX, с")
    ap.add_argument("--max-mem-mb", type=int, default=1200, help="предел памяти, МБ (0 — без предела)")
    ap.add_argument("--since-days", type=int, default=30, help="сколько последних дней повторить")
    ap.add_argument("--deposit", type=float, default=1000, help="капитал на начало, $")
    ap.add_argument("--delay", type=int, default=5, help="задержка решения после сделки кита, с (опрос базы)")
    ap.add_argument("--slippage", type=float, default=0.05, help="проскальзывание рыночного ордера, %%")
    ap.add_argument("--fee-taker", type=float, default=0.05, help="комиссия тейкера OKX, %%")
    ap.add_argument("--fee-maker", type=float, default=0.02, help="комиссия мейкера OKX (тейк лимитом), %%")
    ap.add_argument("--variants", action="store_true", help="сравнить варианты правил (VARIANTS) на тех же китах")
    return ap


def variant_row(r: Rules, st: dict[str, Any], days: float) -> dict[str, Any]:
    """Строка сравнения: настройки варианта, его итог и сделок в месяц (30 дней)."""
    return {"name": r.name, "tp_pct": round(r.tp * 100, 3), "per_month": round(st["trades"] / max(days, 1.0) * 30, 1),
            "stop_max_pct": round(r.stop_max * 100, 3) if math.isfinite(r.stop_max) else None,
            "levels": r.levels, "ma": r.ma,
            **{k: st[k] for k in ("trades", "win_pct", "net", "pf", "max_dd", "open")}}


def run(a: argparse.Namespace, fetch: Fetch | None = None, now_ms: int | None = None) -> dict[str, Any]:
    now = now_ms if now_ms is not None else int(time.time() * 1000)
    os.makedirs(a.out, exist_ok=True)

    def progress(p: dict[str, Any]) -> None:
        tmp = os.path.join(a.out, "progress.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({**p, "at": int(time.time())}, f, ensure_ascii=False)
        os.replace(tmp, os.path.join(a.out, "progress.json"))

    progress({"stage": "каталог OKX", "done": 0, "total": 0, "trades": 0, "requests": 0})
    f = fetch or okx_fetch
    # Свечи — до 4 запросов сразу, общая частота — не чаще раза в --pause.
    pool = ThreadPoolExecutor(max_workers=4, thread_name_prefix="okx")
    limiter = Limiter(a.pause)
    prices = Prices(a.cache, f, a.pause, a.offline, now, pool=pool, limiter=limiter)
    hours = Prices(a.cache, f, a.pause, a.offline, now, bar="4H", bar_ms=core.LEVEL_BAR_MS, pool=pool,
                   limiter=limiter)  # уровни графика 4ч
    instr = instruments(f, a.offline, a.cache)
    rp = Replay(a, prices, instr, progress, hours)
    since = now - a.since_days * core.DAY_MS
    # Варианты — тем же проходом, что основной (свечи, окна китов и запросы к базе — общие).
    alts = [Replay(a, prices, instr, progress, hours, rules=r, shared=rp) for r in VARIANTS] \
        if getattr(a, "variants", False) else []
    rp.study = Study() if alts else None
    try:
        replay_all([rp, *alts], since, now)
    finally:
        pool.shutdown(wait=False, cancel_futures=True)
    s = summary(a, rp, since, now)
    s["variants"] = []
    s["edge"] = rp.study.rows() if rp.study is not None else []
    if alts:
        days = (now - since) / core.DAY_MS
        s["variants"] = [variant_row(x.rules, stats(x.trades), days) for x in (rp, *alts)]

    def fmt(ms: float) -> str:
        return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    def line(st: dict[str, Any]) -> str:
        return (f"сделок {st['trades']}, в плюс {st['win_pct']} %, итог {st['net']:+.2f} $, PF {st['pf']}, "
                f"просадка {st['max_dd']:.2f} $, открыто {st['open']}")

    text = "\n".join([
        "Бэктест-повтор по актуальным правилам живого бота",
        f"Период: {fmt(since)} — {fmt(now)}; капитал {a.deposit:.2f} $ → {rp.equity:.2f} $ (закрытые сделки); "
        f"маржа {lv.MARGIN_FRAC * 100:g} % × {lv.LEVERAGE}×, тейк +{lv.TP * 100:g} %, "
        f"стоп {core.STOP_PCT * 100:g} % от входа, "
        "выход вслед за китом в убытке.",
        "Весь период: " + line(s["full"]),
        "Первые 3/4: " + line(s["first"]),
        "Последняя 1/4: " + line(s["last"]),
        "Сигналы: " + ", ".join(f"{k} — {v}" for k, v in s["skips"].items()),
        f"Запросов к OKX: {rp.requests()} (часовых — {hours.requests})"
        + (f", сбоев: {prices.errors + hours.errors}" if prices.errors + hours.errors else ""),
        *(["", "Варианты (те же киты, одно изменение правил):"] if s["variants"] else []),
        *(f"  {v['name']}: " + line(v) + f", в месяц ≈ {v['per_month']:g}" for v in s["variants"]),
        *(["", "Куда идёт цена после сигнала (в сторону китов, %: 15 мин / 1 ч / 4 ч / 24 ч; доля в сторону китов за 1 ч):"]
          if s["edge"] else []),
        *(f"  {e['key']} ({e['n']}): {e['r15m']} / {e['r1h']} / {e['r4h']} / {e['r24h']}; {e['up1h']} %" for e in s["edge"]),
        "",
        "Сделки:",
        *(f"  {fmt(t.entry_t)} {t.coin} {'лонг' if t.side > 0 else 'шорт'} {t.entry_px:.6g} → {t.exit_px:.6g} "
          f"{t.why} {t.pnl:+.2f} $" for t in rp.trades),
    ]) + "\n"
    with open(os.path.join(a.out, "report.txt"), "w", encoding="utf-8") as fh:
        fh.write(text)
    tmp = os.path.join(a.out, "summary.json.tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(s, fh, ensure_ascii=False)
    os.replace(tmp, os.path.join(a.out, "summary.json"))
    return {"summary": s, "text": text, "trades": rp.trades, "skips": dict(rp.skips)}


def main(argv: Sequence[str] | None = None) -> int:
    a = parser().parse_args(argv)
    if not a.out:
        a.out = tempfile.mkdtemp(prefix="autotrade-replay-")
    core.limit_memory(a.max_mem_mb)
    try:
        r = run(a)
    except (MemoryError, SystemError, sqlite3.Error) as e:
        if not core.out_of_memory(e, a.max_mem_mb > 0):
            raise
        sys.stderr.write(f"бэктест: не хватило предела памяти {a.max_mem_mb} МБ — остановлен сам, сервер не тронут "
                         f"({type(e).__name__}). Уменьшите --since-days.\n")
        return 3
    sys.stdout.write(r["text"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
