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
Итог — summary.json того же вида, что у прежнего бэктеста (вкладка его
рисует), и report.txt.

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
from collections.abc import Callable, Sequence
from datetime import datetime, timezone
from decimal import ROUND_CEILING, ROUND_DOWN, ROUND_FLOOR, Decimal
from typing import Any

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autotrade_backtest as bt
import autotrade_live as lv

BAR_MS = 300_000                  # 5-минутные свечи OKX
BLOCK_MS = 100 * BAR_MS           # блок кеша — 100 свечей (один запрос)
SYNC_MS = 15_000                  # живой бот сверяет счёт раз в 15 с
Fetch = Callable[[dict[str, Any]], object]


class Prices:
    """5-минутные свечи OKX по блокам из 100 (один запрос на блок), кеш на
    диске по бумагам; без сети (--offline) — только кеш."""

    def __init__(self, cache: str, fetch: Fetch, pause: float, offline: bool, now_ms: int) -> None:
        self.cache, self.fetch, self.pause, self.offline, self.now_ms = cache, fetch, pause, offline, now_ms
        self.blocks: dict[str, dict[int, list[list[float]]]] = {}
        self.fresh: dict[tuple[str, int], list[list[float]]] = {}  # незаконченные блоки — только на этот прогон
        self.dirty: set[str] = set()
        self.requests = 0
        self.errors = 0
        os.makedirs(cache, exist_ok=True)

    def _file(self, inst: str) -> str:
        return os.path.join(self.cache, f"replay_{bt.safe_name(inst)}_5m.json")

    def _load(self, inst: str) -> dict[int, list[list[float]]]:
        if inst not in self.blocks:
            got: dict[int, list[list[float]]] = {}
            try:
                with open(self._file(inst), encoding="utf-8") as f:
                    raw = json.load(f)
                if isinstance(raw, dict):
                    got = {int(k): v for k, v in raw.items() if isinstance(v, list)}
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
        # Блок, ещё не закончившийся «сейчас», в кеш навсегда не кладём.
        if b in have and end <= self.now_ms:
            return have[b]
        if (inst, b) in self.fresh:
            return self.fresh[(inst, b)]
        if self.offline:
            return have.get(b, [])
        if self.requests and self.pause:
            time.sleep(self.pause)
        self.requests += 1
        try:
            rows = self.fetch({"okx": "/api/v5/market/history-candles",
                               "params": {"instId": inst, "bar": "5m", "after": end, "limit": 100}})
        except Exception:  # noqa: BLE001 — сеть: сбой — блок без свечей, а не падение
            self.errors += 1
            rows = None
        out: list[list[float]] = []
        for r in rows if isinstance(rows, list) else []:
            try:
                t, o, h, lo, c = int(r[0]), float(r[1]), float(r[2]), float(r[3]), float(r[4])
            except (IndexError, TypeError, ValueError):
                continue
            if b * BLOCK_MS <= t < end and min(o, h, lo, c) > 0 and all(map(math.isfinite, (o, h, lo, c))):
                out.append([t, o, h, lo, c])
        out.sort()
        if end <= self.now_ms:
            have[b] = out
            self.dirty.add(inst)
        else:
            self.fresh[(inst, b)] = out
        return out

    def candle(self, inst: str, t: int) -> list[float] | None:
        """Свеча, в которую попадает момент t."""
        start = t // BAR_MS * BAR_MS
        for c in self.block(inst, t // BLOCK_MS):
            if c[0] == start:
                return c
        return None

    def candles(self, inst: str, start: int, end: int) -> Any:
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


class Replay:
    """Повтор: строки сделок открытия по времени → Signals живого бота →
    правила входа; открытая позиция ведётся по свечам до выхода."""

    def __init__(self, a: argparse.Namespace, prices: Prices, instr: dict[str, dict[str, Any]],
                 progress: Callable[[dict[str, Any]], None]) -> None:
        self.a, self.px, self.instr, self.progress = a, prices, instr, progress
        self.sig = lv.Signals(a.db)
        self.equity = float(a.deposit)
        self.trades: list[bt.Trade] = []
        self.signals: list[bt.Signal] = []
        self.entered: dict[str, int] = {}
        self.busy_until = 0
        self.skips: defaultdict[str, int] = defaultdict(int)
        self.averaged = 0

    # правила входа — как Trader.try_enter, без биржи
    def decide(self, s: dict[str, Any], now: int) -> str:
        if now < self.busy_until:
            return "own_open"
        if now - self.entered.get(s["coin"], -10**15) < lv.COIN_PAUSE_MS:
            return "coin_pause"
        # Бумаги нет на OKX — сразу (живой бот проверяет это позже, но итог тот же,
        # а повтор не тратит запрос к базе на каждый такой сигнал).
        base, scale = bt.okx_candidate(s["coin"])
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
        if self.equity <= 0:
            return "no_equity"
        sz = contracts(ins, self.equity * lv.MARGIN_FRAC * lv.LEVERAGE, last)
        if sz is None:
            return "min_size"
        tick = lv.dec(ins.get("tickSz"))
        if tick is None:
            return "bad_params"
        tp_try = lv.round_step(last * (1 + s["side"] * lv.TP), tick, ROUND_CEILING if s["side"] > 0 else ROUND_FLOOR)
        if tp_try <= 0 or not lv.TP * 0.999 <= s["side"] * (float(tp_try) / last - 1) <= lv.TP + lv.TP_SLACK:
            return "tick"
        self.open_trade(s, inst, ins, sz, last, now)
        return ""

    def open_trade(self, s: dict[str, Any], inst: str, ins: dict[str, Any], sz: Decimal, last: float, now: int) -> None:
        a, side = self.a, s["side"]
        slip = a.slippage / 100
        entry = last * (1 + side * slip)
        tick = lv.dec(ins.get("tickSz")) or Decimal("0.0001")
        coin_per = coin_per_contract(ins, entry)
        qty = float(sz) * coin_per      # монет в позиции
        avg_px = entry
        fees = qty * entry * a.fee_taker / 100
        tp = float(lv.round_step(avg_px * (1 + side * lv.TP), tick, ROUND_CEILING if side > 0 else ROUND_FLOOR))
        wallets = s.get("wallets") or {}
        exit_at = self.loss_exit_time(s["coin"], side, wallets, now)
        add_at = self.add_time(s["coin"], side, wallets, now)
        averaged = False
        mae = 0.0
        why, t_out, px_out = "открыта", self.px.now_ms, avg_px
        for c in self.px.candles(inst, now, self.px.now_ms):
            t, o, h, lo, cl = c
            end = t + BAR_MS
            # Кит вышел в убыток — бот замечает на сверке (до 15 с) и закрывает по рынку.
            if exit_at is not None and exit_at + SYNC_MS < end:
                why, t_out, px_out = "кит вышел в убыток", max(t, exit_at + SYNC_MS), cl * (1 - side * slip)
                fees += qty * px_out * a.fee_taker / 100
                break
            worst = lo if side > 0 else h
            mae = max(mae, qty * side * (avg_px - worst))
            # Разовое усреднение: кит уже добрал, минус больше 1 % — по цене порога (или хуже, если открылись за ним).
            if not averaged and add_at is not None and add_at < end:
                edge = avg_px * (1 - side * lv.AVG_LOSS)
                if side * (worst - edge) < 0:
                    fill = (o if side * (o - edge) < 0 else edge) * (1 + side * slip)
                    add_sz = contracts(ins, max(0.0, self.equity) * lv.AVG_FRAC * lv.LEVERAGE, fill)
                    averaged = True
                    if add_sz is not None:
                        add_q = float(add_sz) * coin_per_contract(ins, fill)
                        avg_px = (avg_px * qty + fill * add_q) / (qty + add_q)
                        qty += add_q
                        fees += add_q * fill * a.fee_taker / 100
                        tp = float(lv.round_step(avg_px * (1 + side * lv.TP), tick,
                                                 ROUND_CEILING if side > 0 else ROUND_FLOOR))
                        self.averaged += 1
            best = h if side > 0 else lo
            if side * (best - tp) >= 0:
                why, t_out, px_out = "тейк", t, tp
                fees += qty * tp * a.fee_maker / 100
                break
            t_out, px_out = end, cl
        pnl = qty * side * (px_out - avg_px) - fees
        notional = qty * avg_px
        if why != "открыта":
            self.equity += pnl
        self.entered[s["coin"]] = now
        self.busy_until = t_out if why != "открыта" else 10**18
        sg = bt.Signal(wallet=next(iter(wallets), ""), coin=s["coin"], side=side, ts=s["ts"],
                       notional=float(s.get("usd") or 0.0), account=0.0, leverage=0, px=s["ref_px"],
                       wallets=int(s.get("whales") or 0), ref_px=s["ref_px"])
        self.trades.append(bt.Trade(sg, now, t_out, pnl / notional if notional else 0.0, pnl, why, "5m", mae,
                                    entry, tp, px_out))

    def loss_exit_time(self, coin: str, side: int, wallets: dict[str, Any], since: int) -> int | None:
        """Когда кто-то из китов сигнала впервые оказался в убытке по закрытиям
        (как Signals.whale_loss, но по времени): сумма closed_pnl закрытий с
        его первой сделки в окне < 0 — после нашего входа."""
        rows = self.whale_rows(coin, side, wallets, closes=True)
        run: dict[str, int] = defaultdict(int)
        for w, ts, p in rows:
            run[w] += p
            if ts > since and run[w] < 0:
                return int(ts)
        return None

    def add_time(self, coin: str, side: int, wallets: dict[str, Any], since: int) -> int | None:
        """Когда кит сигнала добрал позицию после нашего входа на 1000 $ и больше."""
        run: dict[str, int] = defaultdict(int)
        for w, ts, n in self.whale_rows(coin, side, wallets, closes=False):
            if ts > since:
                run[w] += n
                if run[w] >= lv.MIN_WHALE_USD * lv.NANOS:
                    return int(ts)
        return None

    def whale_rows(self, coin: str, side: int, wallets: dict[str, Any], closes: bool) -> list[tuple[str, int, int]]:
        since = {str(k): int(lv.num(v)) for k, v in wallets.items()}
        if not since:
            return []
        con = bt.open_ro(self.a.db)
        try:
            marks = ",".join("?" * len(since))
            if closes:
                codes = bt.CLOSES_FOR[1 if side > 0 else -1]
                q = (f"SELECT wallet, ts, closed_pnl_nanos FROM hl_fills WHERE coin = ? AND wallet IN ({marks}) "  # noqa: S608 — только «?»
                     f"AND dir_code IN ({','.join('?' * len(codes))}) AND ts >= ? "
                     f"AND (dir_code != {bt.FLIP} OR dir LIKE ?) ORDER BY ts, tid")
                args: tuple[Any, ...] = (coin, *since, *codes, min(since.values()),
                                         "%Long > Short%" if side > 0 else "%Short > Long%")
            else:
                q = (f"SELECT wallet, ts, notional_nanos FROM hl_fills WHERE coin = ? AND wallet IN ({marks}) "  # noqa: S608
                     "AND dir_code = ? AND ts >= ? ORDER BY ts, tid")
                args = (coin, *since, bt.OPEN_LONG if side > 0 else bt.OPEN_SHORT, min(since.values()))
            rows = con.execute(q, args).fetchall()
        finally:
            con.close()
        return [(str(w), int(t), int(v or 0)) for w, t, v in rows if int(t) >= since.get(str(w), 10**18)]

    def run(self, since: int, until: int) -> None:
        con = bt.open_ro(self.a.db)
        try:
            total = con.execute("SELECT COUNT(*) FROM hl_fills WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ?",
                                (since, until)).fetchone()[0]
            cur = con.execute(
                "SELECT tid, wallet, coin, dir_code, ts, notional_nanos, ABS(CAST(sz AS REAL)) FROM hl_fills "
                "WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ? ORDER BY ts, tid", (since, until))
            done = 0
            last_note = 0.0
            while True:
                rows = cur.fetchmany(5000)
                if not rows:
                    break
                for row in rows:
                    done += 1
                    for s in reversed(self.sig.feed([row])):  # как candidates: встречный «крупный» — первым
                        now = s["ts"] + self.a.delay * 1000
                        why = self.decide(s, now)
                        self.skips[why or "entered"] += 1
                        if not why:
                            self.signals.append(self.trades[-1].sig)
                            break
                    self.sig.seen.clear()  # повтор идёт по строкам один раз — память не копим
                if time.time() - last_note > 2:
                    last_note = time.time()
                    self.progress({"stage": "повтор", "done": done, "total": total, "trades": len(self.trades),
                                   "requests": self.px.requests})
                    self.px.save()
                    self.sig.sweep(int(rows[-1][4]))
        finally:
            con.close()
        self.px.save()
        self.progress({"stage": "готово", "done": total, "total": total, "trades": len(self.trades),
                       "requests": self.px.requests})


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
    ap.add_argument("--keep-going", action="store_true", help=argparse.SUPPRESS)
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
    f = fetch or bt.http_fetch
    instr = instruments(f, a.offline, a.cache)
    prices = Prices(a.cache, f, a.pause, a.offline, now)
    rp = Replay(a, prices, instr, progress)
    since = now - a.since_days * bt.DAY_MS
    rp.run(since, now)
    return finish(a, rp, since, now)


def finish(a: argparse.Namespace, rp: Replay, since: int, now: int) -> dict[str, Any]:
    trades = rp.trades
    sizing = bt.Sizing(a.deposit, lv.MARGIN_FRAC, lv.LEVERAGE, 1, 1 / (2 * lv.LEVERAGE), cross=True)
    cut = since + (now - since) * 3 // 4
    train = [t for t in trades if t.entry_t < cut]
    hold = [t for t in trades if t.entry_t >= cut]

    def passes(st: bt.Stats) -> bool:
        return st.net > 0 and st.pf >= 1.3 and st.trades >= 100 and st.max_dd <= 0.15 * a.deposit

    prm = bt.Params(a.delay, bt.Filters(0, 0, 0, False, lv.COIN_PAUSE_MS // 60_000, lv.MIN_WHALES, lv.CHASE,
                                        lv.MIN_WHALE_USD, True, 0),
                    bt.Exits(lv.TP, 0.0, 0, True))
    st_train, st_hold, st_all = bt.stats(train), bt.stats(hold), bt.stats(trades)
    verdict = st_all.net > 0 and passes(st_hold)
    ns = argparse.Namespace(deposit=a.deposit, margin_pct=lv.MARGIN_FRAC * 100, leverage=lv.LEVERAGE, max_open=1,
                            markets="all", cluster_hours=lv.WINDOW_MS / 3_600_000, min_whale=str(lv.MIN_WHALE_USD),
                            fee=a.fee_taker, slippage=a.slippage)
    md = argparse.Namespace(requests=rp.px.requests, errors=[""] * rp.px.errors)
    sigs = rp.signals or [bt.Signal("", "", 1, since, 0, 0, 0), bt.Signal("", "", 1, now, 0, 0, 0)]
    summary = bt.build_summary(ns, sizing, sigs, cut, {"5m": len(trades)}, {}, md,  # type: ignore[arg-type]
                               [(prm, st_train, st_hold)], verdict, trades, passes, None)
    summary["period"] = {"from": since, "to": now, "cut": cut}
    summary["data"]["signals"] = sum(rp.skips.values())
    summary["kind"] = "replay"
    summary["skips"] = dict(sorted(rp.skips.items(), key=lambda kv: -kv[1]))
    summary["averaged"] = rp.averaged
    summary["deposit_end"] = round(rp.equity, 2)
    days = max(1.0, (now - since) / bt.DAY_MS)
    summary["per_day"] = round(len(trades) / days, 2)

    def fmt(ms: float) -> str:
        return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    lines = [
        "Бэктест-повтор по актуальным правилам живого бота",
        f"Период: {fmt(since)} — {fmt(now)} ({days:.0f} дн.); капитал {a.deposit:.0f} $ → {rp.equity:.2f} $ "
        f"(закрытые сделки); маржа {lv.MARGIN_FRAC * 100:g} % × {lv.LEVERAGE}×, тейк +{lv.TP * 100:g} %, "
        f"выход вслед за китом в убытке, разовое усреднение (было {rp.averaged}).",
        "Весь период: " + ", ".join(f"{k} {v}" for k, v in st_all.row().items()),
        "Первые 3/4: " + ", ".join(f"{k} {v}" for k, v in st_train.row().items()),
        "Последняя 1/4: " + ", ".join(f"{k} {v}" for k, v in st_hold.row().items()),
        "Пропуски сигналов: " + ", ".join(f"{k} — {v}" for k, v in summary["skips"].items()),
        f"Запросов к OKX: {rp.px.requests}" + (f", сбоев: {rp.px.errors}" if rp.px.errors else ""),
        "",
        "Сделки:",
        *(f"  {fmt(t.entry_t)} {t.sig.coin} {'лонг' if t.sig.side > 0 else 'шорт'} {t.entry_px:.6g} → "
          f"{t.last_px:.6g} {t.why} {t.pnl_usd:+.2f} $" for t in trades),
    ]
    text = "\n".join(lines) + "\n"
    with open(os.path.join(a.out, "report.txt"), "w", encoding="utf-8") as fh:
        fh.write(text)
    tmp = os.path.join(a.out, "summary.json.tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, ensure_ascii=False)
    os.replace(tmp, os.path.join(a.out, "summary.json"))
    return {"summary": summary, "text": text, "trades": trades, "skips": dict(rp.skips)}


def main(argv: Sequence[str] | None = None) -> int:
    a = parser().parse_args(argv)
    if not a.out:
        a.out = tempfile.mkdtemp(prefix="autotrade-replay-")
    bt.limit_memory(a.max_mem_mb)
    try:
        r = run(a)
    except (MemoryError, SystemError, sqlite3.Error) as e:
        if not bt.out_of_memory(e, a.max_mem_mb > 0):
            raise
        sys.stderr.write(f"бэктест: не хватило предела памяти {a.max_mem_mb} МБ — остановлен сам, сервер не тронут "
                         f"({type(e).__name__}). Уменьшите --since-days.\n")
        return 3
    sys.stdout.write(r["text"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
