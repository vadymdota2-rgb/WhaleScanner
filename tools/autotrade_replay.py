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
- бумага есть на OKX (USDT-SWAP), цена сходится с китами (±2 %), не хуже их
  средней больше чем на 0,3 %; в бумагу — не чаще раза в 3 часа; позиция —
  одна;
- маржа 5 % капитала × 10×, объём — по контракту и шагу OKX (меньше
  минимума — пропуск), рыночный вход (проскальзывание);
- выход: лимитный тейк +1 % от средней; кит сигнала закрыл в убыток — по
  рынку; разовое усреднение: минус больше 1 % и кит добрал от 1000 $ после
  нашего входа — ещё 5 % капитала, тейк +1 % от новой средней.

Цены — 5-минутные свечи OKX (публичное API, без ключа; история — годы, по
100 свечей за запрос), только под кандидатов и сделки, с кешем блоками по
100 свечей. Ход — в progress.json (служба показывает его во вкладке).
Итог — summary.json (его рисует вкладка) и report.txt.

Сравнение вариантов: тот же поток сигналов ведут сразу несколько счетов —
правила живого бота и варианты выхода (VARIANTS: «вместе с китами» без
тейка, тейк или раньше — вместе с китом, без усреднения, фильтр потоков на
биржи: сильный завод — не лонг, сильный вывод — не шорт). Вход у всех — по
правилам живого бота (у вариантов с фильтром — ещё и он); у каждого счёта свой капитал, своя позиция и паузы.
Вкладка показывает их рядом — правила меняются по цифрам, а не на глаз.

Допущения: цена решения — открытие 5-минутной свечи; внутри свечи при
одновременном тейке и выходе за китом — выход (хуже); фандинг не учитывается.

    python3 tools/autotrade_replay.py --db ~/WhaleScanner/hyperliquid.db --since-days 30
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sqlite3
import sys
import tempfile
import time
from collections import defaultdict
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from decimal import ROUND_CEILING, ROUND_DOWN, ROUND_FLOOR, Decimal
from typing import Any

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autotrade_core as core
import autotrade_live as lv

BAR_MS = 300_000                  # 5-минутные свечи OKX
BLOCK_MS = 100 * BAR_MS           # блок кеша — 100 свечей (один запрос)
SYNC_MS = 15_000                  # живой бот сверяет счёт раз в 15 с
Fetch = Callable[[dict[str, Any]], object]


def okx_fetch(body: dict[str, Any]) -> object:
    return core.okx_public(str(body["okx"]), body.get("params") or {})


def candle_ok(r: object) -> bool:
    return isinstance(r, list) and len(r) == 5 and all(
        isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x) for x in r) and min(r[1:]) > 0


class Prices:
    """5-минутные свечи OKX по блокам из 100 (один запрос на блок), кеш на
    диске по бумагам; без сети (--offline) — только кеш. В кеш навсегда —
    только ответ OKX за уже закончившийся блок: сбой сети или незаконченный
    блок — лишь на этот прогон (иначе пустой блок от сбоя остался бы навсегда)."""

    def __init__(self, cache: str, fetch: Fetch, pause: float, offline: bool, now_ms: int) -> None:
        self.cache, self.fetch, self.pause, self.offline, self.now_ms = cache, fetch, pause, offline, now_ms
        self.blocks: dict[str, dict[int, list[list[float]]]] = {}
        self.fresh: dict[tuple[str, int], list[list[float]]] = {}
        self.dirty: set[str] = set()
        self.requests = 0
        self.errors = 0
        os.makedirs(cache, exist_ok=True)

    def _file(self, inst: str) -> str:
        return os.path.join(self.cache, f"replay_{core.safe_name(inst)}_5m.json")

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
        end = (b + 1) * BLOCK_MS
        if b in have:
            return have[b]
        if (inst, b) in self.fresh:
            return self.fresh[(inst, b)]
        if self.offline:
            return []
        if self.requests and self.pause:
            time.sleep(self.pause)
        self.requests += 1
        try:
            rows = self.fetch({"okx": "/api/v5/market/history-candles",
                               "params": {"instId": inst, "bar": "5m", "after": end, "limit": 100}})
        except Exception:  # noqa: BLE001 — сеть: сбой — блок без свечей, а не падение
            rows = None
        if not isinstance(rows, list):
            self.errors += 1
        out: list[list[float]] = []
        for r in rows if isinstance(rows, list) else []:
            try:
                c = [int(r[0]), float(r[1]), float(r[2]), float(r[3]), float(r[4])]
            except (IndexError, TypeError, ValueError):
                continue
            if b * BLOCK_MS <= c[0] < end and candle_ok(c):
                out.append(c)
        out.sort()
        if isinstance(rows, list) and end <= self.now_ms:
            have[b] = out
            self.dirty.add(inst)
        else:
            self.fresh[(inst, b)] = out
        return out

    def candle(self, inst: str, t: int) -> list[float] | None:
        """Свеча, в которую попадает момент t."""
        start = t // BAR_MS * BAR_MS
        return next((c for c in self.block(inst, t // BLOCK_MS) if c[0] == start), None)

    def candles(self, inst: str, start: int, end: int) -> Iterator[list[float]]:
        """Свечи с момента start до end — по блокам, по мере надобности."""
        b = start // BLOCK_MS
        while b * BLOCK_MS < end:
            for c in self.block(inst, b):
                if c[0] + BAR_MS > start and c[0] < end:
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


def contracts(ins: dict[str, Any], notional: float, px: float) -> Decimal | None:
    """Объём в контрактах как у живого бота: вниз по шагу; меньше минимума — None."""
    ct_val, ct_mult = lv.dec(ins.get("ctVal")), lv.dec(ins.get("ctMult") or "1")
    step, min_sz = lv.dec(ins.get("lotSz")), lv.dec(ins.get("minSz"))
    if ct_val is None or ct_mult is None or step is None or min_sz is None or notional <= 0 or px <= 0:
        return None
    ct_d = ct_val * ct_mult
    in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
    qty = Decimal(repr(round(notional, 6))) / (ct_d if in_usd else ct_d * Decimal(repr(px)))
    sz = (qty / step).to_integral_value(rounding=ROUND_DOWN) * step
    return sz if sz >= min_sz and sz > 0 else None


@dataclass
class Trade:
    coin: str
    side: int
    whales: int
    entry_t: int
    exit_t: int
    entry_px: float     # цена первого входа (с проскальзыванием)
    tp: float           # тейк на конец (после усреднения — от новой средней)
    exit_px: float      # цена выхода; открытая — цена на конец данных
    notional: float     # $ по средней цене (после усреднения — весь объём)
    pnl: float          # $ с комиссиями
    why: str            # тейк / кит вышел в убыток / открыта
    mae: float          # худший ход против нас внутри сделки, $
    averaged: bool

    @property
    def open(self) -> bool:
        return self.why == "открыта"


@dataclass(frozen=True)
class Rules:
    """Вариант правил выхода. Вход у всех вариантов — по правилам живого
    бота; первый вариант (live) — сами эти правила целиком."""
    key: str
    name: str
    tp: bool = True             # лимитный тейк +1 % от средней
    with_whales: bool = False   # выход, когда кит сигнала закрыл от 1000 $ (в плюс или в минус); иначе — только в убытке
    average: bool = True        # разовое усреднение
    flow_z: float | None = None  # фильтр потоков на биржи: завод сильнее flow_z σ — не лонг, вывод — не шорт


VARIANTS = (
    Rules("live", "как сейчас: тейк +1 %, выход за китом в убытке, усреднение"),
    Rules("with_whales", "вместе с китами: тейка нет, выходим, когда кит сигнала закрыл от 1000 $",
          tp=False, with_whales=True),
    Rules("tp_or_whales", "тейк +1 % или раньше — вместе с китом", with_whales=True),
    Rules("no_average", "как сейчас, без усреднения", average=False),
    Rules("flows", "как сейчас + потоки на биржи: сильный завод (от 1σ) — не лонг, сильный вывод — не шорт", flow_z=1.0),
    Rules("flows_half", "как сейчас + потоки на биржи (от 0,5σ)", flow_z=0.5),
)


class Book:
    """Счёт одного варианта: свой капитал, сделки, паузы и пропуски."""

    def __init__(self, rules: Rules, deposit: float) -> None:
        self.rules = rules
        self.equity = deposit
        self.trades: list[Trade] = []
        self.entered: dict[str, int] = {}
        self.busy_until = 0
        self.skips: defaultdict[str, int] = defaultdict(int)


class Replay:
    """Повтор: строки сделок открытия по времени → Signals живого бота →
    правила входа на каждом счёте-варианте; открытая позиция ведётся по
    свечам до выхода."""

    def __init__(self, a: argparse.Namespace, prices: Prices, instr: dict[str, dict[str, Any]],
                 progress: Callable[[dict[str, Any]], None]) -> None:
        self.a, self.px, self.instr, self.progress = a, prices, instr, progress
        self.sig = lv.Signals(a.db)
        self.books = [Book(r, float(a.deposit)) for r in VARIANTS]
        # Заводы на биржи и выводы (BTC и токены BSC) — для вариантов с фильтром потоков.
        here = os.path.dirname(os.path.abspath(a.db))
        self.flows = core.Flows(a.btc_db or os.path.join(here, "btc.db"), a.bscex_db or os.path.join(here, "bsc_ex.db"),
                                a.main_db or os.path.join(here, "whale_bot.db"))
        self.flow_seen: set[str] = set()

    # правила входа — как Trader.try_enter, без биржи
    def decide(self, b: Book, s: dict[str, Any], now: int) -> str:
        if now < b.busy_until:
            return "own_open"
        if now - b.entered.get(s["coin"], -10**15) < lv.COIN_PAUSE_MS:
            return "coin_pause"
        # Бумаги нет на OKX — сразу (живой бот проверяет это позже, итог тот же,
        # а повтор не тратит запрос к базе на каждый такой сигнал).
        base, scale = core.okx_candidate(s["coin"])
        inst = f"{base}-USDT-SWAP"
        ins = self.instr.get(inst)
        if not ins:
            return "no_inst"
        usd, against, count = self.sig.balance(s["coin"], s["side"], now)
        if against > 0 and against >= usd:
            return "bigger_against"
        if count < lv.MIN_WHALES:
            return "stale"
        if self.sig.whale_loss(s["coin"], s["side"], s.get("wallets") or {}, now):
            return "whale_exit"
        c = self.px.candle(inst, now)
        if c is None:
            return "no_price"
        last = c[1]  # цена на момент решения — открытие свечи
        hl_px = last * scale
        if abs(hl_px / s["ref_px"] - 1) > lv.MATCH_TOL:
            return "other_asset"
        if s["side"] * (hl_px / s["ref_px"] - 1) > lv.CHASE:
            return "chase"
        if b.equity <= 0:
            return "no_equity"
        sz = contracts(ins, b.equity * lv.MARGIN_FRAC * lv.LEVERAGE, last)
        if sz is None:
            return "min_size"
        tick = lv.dec(ins.get("tickSz"))
        if tick is None:
            return "bad_params"
        tp_try = lv.round_step(last * (1 + s["side"] * lv.TP), tick, ROUND_CEILING if s["side"] > 0 else ROUND_FLOOR)
        if tp_try <= 0 or not lv.TP * 0.999 <= s["side"] * (float(tp_try) / last - 1) <= lv.TP + lv.TP_SLACK:
            return "tick"
        if b.rules.flow_z is not None:
            z = self.flows.z(s["coin"], now)
            if z is not None:
                self.flow_seen.add(s["coin"])
                # Плюс — заводят на биржи (к продаже): против лонга; минус — выводят: против шорта.
                if s["side"] * z > b.rules.flow_z:
                    return "flow_against"
        self.open_trade(b, s, inst, ins, sz, last, now)
        return ""

    def open_trade(self, b: Book, s: dict[str, Any], inst: str, ins: dict[str, Any], sz: Decimal, last: float,
                   now: int) -> None:
        a, side, r = self.a, s["side"], b.rules
        slip = a.slippage / 100
        rnd = ROUND_CEILING if side > 0 else ROUND_FLOOR
        entry = last * (1 + side * slip)
        tick = lv.dec(ins.get("tickSz")) or Decimal("0.0001")
        qty = float(sz) * coin_per_contract(ins, entry)      # монет в позиции
        avg_px = entry
        fees = qty * entry * a.fee_taker / 100
        tp = float(lv.round_step(avg_px * (1 + side * lv.TP), tick, rnd))
        wallets = s.get("wallets") or {}
        exit_at = self.exit_time(s["coin"], side, wallets, now, r.with_whales)
        add_at = self.add_time(s["coin"], side, wallets, now) if r.average else None
        averaged = False
        mae = 0.0
        why, t_out, px_out = "открыта", self.px.now_ms, avg_px
        for tf, o, h, lo, cl in self.px.candles(inst, now, self.px.now_ms):
            t = int(tf)
            end = t + BAR_MS
            # Кит вышел (в убытке; у «вместе с китами» — любым закрытием) — бот
            # замечает на сверке (до 15 с) и закрывает по рынку.
            if exit_at is not None and exit_at + SYNC_MS < end:
                why = "вместе с китом" if r.with_whales else "кит вышел в убыток"
                t_out, px_out = max(t, exit_at + SYNC_MS), cl * (1 - side * slip)
                fees += qty * px_out * a.fee_taker / 100
                break
            worst = lo if side > 0 else h
            mae = max(mae, qty * side * (avg_px - worst))
            # Разовое усреднение: кит уже добрал, минус больше 1 % — по цене порога
            # (или хуже, если свеча открылась уже за ним).
            if not averaged and add_at is not None and add_at < end:
                edge = avg_px * (1 - side * lv.AVG_LOSS)
                if side * (worst - edge) < 0:
                    fill = (o if side * (o - edge) < 0 else edge) * (1 + side * slip)
                    add_sz = contracts(ins, max(0.0, b.equity) * lv.AVG_FRAC * lv.LEVERAGE, fill)
                    averaged = True
                    if add_sz is not None:
                        add_q = float(add_sz) * coin_per_contract(ins, fill)
                        avg_px = (avg_px * qty + fill * add_q) / (qty + add_q)
                        qty += add_q
                        fees += add_q * fill * a.fee_taker / 100
                        tp = float(lv.round_step(avg_px * (1 + side * lv.TP), tick, rnd))
            best = h if side > 0 else lo
            if r.tp and side * (best - tp) >= 0:
                why, t_out, px_out = "тейк", t, tp
                fees += qty * tp * a.fee_maker / 100
                break
            t_out, px_out = end, cl
        pnl = qty * side * (px_out - avg_px) - fees
        if why != "открыта":
            b.equity += pnl
        b.entered[s["coin"]] = now
        b.busy_until = t_out if why != "открыта" else 10**18
        b.trades.append(Trade(s["coin"], side, int(s.get("whales") or 0), now, t_out, entry, tp, px_out,
                                 qty * avg_px, pnl, why, mae, averaged))

    def exit_time(self, coin: str, side: int, wallets: dict[str, Any], since: int, any_close: bool) -> int | None:
        """Когда выходить вслед за китом сигнала (после нашего входа): кит в
        убытке по закрытиям (как Signals.whale_loss, но по времени) — сумма
        closed_pnl закрытий с его первой сделки в окне < 0; any_close («вместе
        с китами») — ещё и когда его закрытия после нашего входа набрали от
        1000 $, в плюс или в минус."""
        pnl: dict[str, int] = defaultdict(int)
        vol: dict[str, int] = defaultdict(int)
        for w, ts, p, n in self.whale_rows(coin, side, wallets, closes=True):
            pnl[w] += p
            if ts > since:
                vol[w] += n
                if pnl[w] < 0 or (any_close and vol[w] >= lv.MIN_WHALE_USD * core.NANOS):
                    return ts
        return None

    def add_time(self, coin: str, side: int, wallets: dict[str, Any], since: int) -> int | None:
        """Когда кит сигнала добрал позицию после нашего входа на 1000 $ и больше."""
        run: dict[str, int] = defaultdict(int)
        for w, ts, n, _ in self.whale_rows(coin, side, wallets, closes=False):
            if ts > since:
                run[w] += n
                if run[w] >= lv.MIN_WHALE_USD * core.NANOS:
                    return ts
        return None

    def whale_rows(self, coin: str, side: int, wallets: dict[str, Any], closes: bool) -> list[tuple[str, int, int, int]]:
        """Сделки китов сигнала по бумаге с их первой сделки в окне: закрытия
        (closed_pnl, объём) или добавления (объём, 0) — по времени."""
        since = {str(k): int(lv.num(v)) for k, v in wallets.items()}
        if not since:
            return []
        marks = ",".join("?" * len(since))
        if closes:
            codes = core.CLOSES_FOR[1 if side > 0 else -1]
            q = (f"SELECT wallet, ts, closed_pnl_nanos, notional_nanos FROM hl_fills WHERE coin = ? AND wallet IN ({marks}) "  # noqa: S608 — только «?»
                 f"AND dir_code IN ({','.join('?' * len(codes))}) AND ts >= ? "
                 f"AND (dir_code != {core.FLIP} OR dir LIKE ?) ORDER BY ts, tid")
            args: tuple[Any, ...] = (coin, *since, *codes, min(since.values()),
                                     "%Long > Short%" if side > 0 else "%Short > Long%")
        else:
            q = (f"SELECT wallet, ts, notional_nanos, 0 FROM hl_fills WHERE coin = ? AND wallet IN ({marks}) "  # noqa: S608
                 "AND dir_code = ? AND ts >= ? ORDER BY ts, tid")
            args = (coin, *since, core.OPEN_LONG if side > 0 else core.OPEN_SHORT, min(since.values()))
        con = core.open_ro(self.a.db)
        try:
            rows = con.execute(q, args).fetchall()
        finally:
            con.close()
        return [(str(w), int(t), int(v or 0), int(n or 0)) for w, t, v, n in rows if int(t) >= since.get(str(w), 10**18)]

    def run(self, since: int, until: int) -> None:
        con = core.open_ro(self.a.db)
        try:
            total = con.execute("SELECT COUNT(*) FROM hl_fills WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ?",
                                (since, until)).fetchone()[0]
            cur = con.execute(
                f"SELECT tid, wallet, coin, dir_code, ts, notional_nanos, ABS(CAST(sz AS REAL)), {lv.MARGIN_SQL} FROM hl_fills "  # noqa: S608 — константа
                "WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ? ORDER BY ts, tid", (since, until))
            done = 0
            last_note = 0.0
            while rows := cur.fetchmany(5000):
                for row in rows:
                    done += 1
                    cands = list(reversed(self.sig.feed([row])))  # как candidates: встречный «крупный» — первым
                    for b in self.books:
                        for s in cands:
                            why = self.decide(b, s, s["ts"] + self.a.delay * 1000)
                            b.skips[why or "entered"] += 1
                            if not why:
                                break
                self.sig.seen.clear()  # повтор идёт по строкам один раз — повторов нет, память не копим
                if time.time() - last_note > 2:
                    last_note = time.time()
                    self.progress({"stage": "повтор", "done": done, "total": total, "trades": len(self.books[0].trades),
                                   "requests": self.px.requests})
                    self.px.save()
                    self.sig.sweep(int(rows[-1][4]))
        finally:
            con.close()
        self.px.save()
        self.progress({"stage": "готово", "done": total, "total": total, "trades": len(self.books[0].trades),
                       "requests": self.px.requests})


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


def split(trades: Sequence[Trade], cut: int) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    """Итоги: весь период, первые 3/4 (до cut), последняя 1/4."""
    return (stats(trades), stats([t for t in trades if t.entry_t < cut]),
            stats([t for t in trades if t.entry_t >= cut]))


def passed(full: dict[str, Any], last: dict[str, Any], crit: dict[str, Any]) -> bool:
    """Правила окупаются: весь период в плюсе и последняя четверть проходит критерий."""
    return bool(full["net"] > 0 and last["net"] > 0 and (last["pf"] is None or last["pf"] >= crit["pf_ge"])
                and last["trades"] >= crit["trades_ge"] and last["max_dd"] <= crit["max_dd_le"])


def summary(a: argparse.Namespace, rp: Replay, since: int, now: int) -> dict[str, Any]:
    main = rp.books[0]
    trades = main.trades
    cut = since + (now - since) * 3 // 4
    st_all, st_first, st_last = split(trades, cut)
    crit = {"net_gt": 0, "pf_ge": 1.3, "trades_ge": 100, "max_dd_le": round(0.15 * a.deposit, 2)}
    variants = []
    for b in rp.books:
        v_all, _v_first, v_last = split(b.trades, cut)
        variants.append({"key": b.rules.key, "name": b.rules.name, "deposit_end": round(b.equity, 2),
                         "verdict": passed(v_all, v_last, crit), "full": v_all, "last": v_last,
                         "averaged": sum(t.averaged for t in b.trades), "blocked": b.skips.get("flow_against", 0)})

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
        return {"coin": t.coin, "side": t.side, "whales": t.whales, "entry": t.entry_t, "exit": t.exit_t,
                "px_in": t.entry_px, "px_out": t.exit_px, "tp": t.tp, "usd": round(t.pnl, 2),
                "pct": round(t.pnl / t.notional * 100, 3) if t.notional else 0.0, "why": t.why,
                "averaged": t.averaged, "mae": round(t.mae, 2),
                "held_h": round((t.exit_t - t.entry_t) / core.HOUR_MS, 1)}

    return {
        "generated_at": int(time.time()),
        "verdict": passed(st_all, st_last, crit),
        "criteria": crit,
        "period": {"from": since, "to": now, "cut": cut},
        "sizing": {"deposit": a.deposit, "deposit_end": round(main.equity, 2), "margin_pct": lv.MARGIN_FRAC * 100,
                   "leverage": lv.LEVERAGE, "fee_taker_pct": a.fee_taker, "fee_maker_pct": a.fee_maker,
                   "slippage_pct": a.slippage},
        "data": {"signals": sum(main.skips.values()), "requests": rp.px.requests, "errors": rp.px.errors,
                 # Монеты, по которым были данные о потоках на биржи (BTC, токены BSC).
                 "flow_coins": sorted(rp.flow_seen)[:50]},
        "full": st_all, "first": st_first, "last": st_last,
        # Те же сигналы — другие правила выхода (первый — сами правила бота).
        "variants": variants,
        "equity": curve,
        "by_coin": group(lambda t: t.coin),
        "by_session": group(lambda t: "open" if us_open(t.entry_t) else "closed"),
        "by_exit": group(lambda t: t.why),
        "skips": dict(sorted(main.skips.items(), key=lambda kv: -kv[1])),
        "averaged": sum(t.averaged for t in trades),
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
    ap.add_argument("--btc-db", default=os.environ.get("WHALE_BTC_DB_FILE", ""), help="база биткоина бота (потоки на биржи)")
    ap.add_argument("--bscex-db", default=os.environ.get("WHALE_BSCEX_DB_FILE", ""), help="база бирж BSC бота (потоки на биржи)")
    ap.add_argument("--main-db", default="", help="основная база бота (тикеры токенов BSC)")
    ap.add_argument("--slippage", type=float, default=0.05, help="проскальзывание рыночного ордера, %%")
    ap.add_argument("--fee-taker", type=float, default=0.05, help="комиссия тейкера OKX, %%")
    ap.add_argument("--fee-maker", type=float, default=0.02, help="комиссия мейкера OKX (тейк лимитом), %%")
    return ap


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
    prices = Prices(a.cache, f, a.pause, a.offline, now)
    rp = Replay(a, prices, instruments(f, a.offline, a.cache), progress)
    since = now - a.since_days * core.DAY_MS
    rp.run(since, now)
    s = summary(a, rp, since, now)
    main = rp.books[0]

    def fmt(ms: float) -> str:
        return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    def line(st: dict[str, Any]) -> str:
        return (f"сделок {st['trades']}, в плюс {st['win_pct']} %, итог {st['net']:+.2f} $, PF {st['pf']}, "
                f"просадка {st['max_dd']:.2f} $, открыто {st['open']}")

    text = "\n".join([
        "Бэктест-повтор по актуальным правилам живого бота",
        f"Период: {fmt(since)} — {fmt(now)}; капитал {a.deposit:.2f} $ → {main.equity:.2f} $ (закрытые сделки); "
        f"маржа {lv.MARGIN_FRAC * 100:g} % × {lv.LEVERAGE}×, тейк +{lv.TP * 100:g} %, выход вслед за китом в "
        f"убытке, разовое усреднение (было {s['averaged']}).",
        "Весь период: " + line(s["full"]),
        "Первые 3/4: " + line(s["first"]),
        "Последняя 1/4: " + line(s["last"]),
        "Варианты выхода на тех же сигналах:",
        *(f"  {v['name']}: весь период — {line(v['full'])}; последняя 1/4 — {line(v['last'])}" for v in s["variants"]),
        "Сигналы: " + ", ".join(f"{k} — {v}" for k, v in s["skips"].items()),
        f"Запросов к OKX: {prices.requests}" + (f", сбоев: {prices.errors}" if prices.errors else ""),
        "",
        "Сделки:",
        *(f"  {fmt(t.entry_t)} {t.coin} {'лонг' if t.side > 0 else 'шорт'} {t.entry_px:.6g} → {t.exit_px:.6g} "
          f"{t.why}{' (усреднено)' if t.averaged else ''} {t.pnl:+.2f} $" for t in main.trades),
    ]) + "\n"
    with open(os.path.join(a.out, "report.txt"), "w", encoding="utf-8") as fh:
        fh.write(text)
    tmp = os.path.join(a.out, "summary.json.tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(s, fh, ensure_ascii=False)
    os.replace(tmp, os.path.join(a.out, "summary.json"))
    return {"summary": s, "text": text, "trades": main.trades, "skips": dict(main.skips),
            "books": {b.rules.key: b for b in rp.books}}


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
