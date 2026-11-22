# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Карта ликвидаций на момент в прошлом — для бэктеста-повтора
(autotrade_replay.py).

Живой бот берёт карту у whale_api (Wallet-Tracker, /api/liqmap, окно —
сутки). Повтору нужна карта на момент каждого сигнала, поэтому здесь —
история бирж с концом в прошлом и та же модель (wt_liqmap.py из
Wallet-Tracker, подключается по пути --wt-dir):

- ряды — 15-минутные (как у окна «сутки»): свеча, открытый интерес в $ и
  доля агрессивных покупок; биржи — Binance (USDT-M; интерес — только за
  последние 30 дней, больше Binance не хранит) и Bybit (linear; интерес — с
  одной стороны: singleOpenInterest, нет — половина openInterest, как в
  whale_api); доля покупок у Bybit — Binance (whale_api тоже берёт
  рыночную долю у бирж, что своей не отдают);
- дешёвые монеты Binance и Bybit держат как 1000PEPE: не нашлась под своим
  именем — ищем под множителем и делим цену обратно;
- на момент t — только закрытые свечи (снимок + 15 мин ≤ t: незакрытая
  свеча заглянула бы в будущее), окно — последние 96 (сутки), учёт — ещё
  три дня до окна (старые позиции горят и закрываются, на карту окна не
  идут — как предыстория карты сервера, только короче);
- биржа, чья цена дальше 3 % от цены сделки, — другой актив: не берём.

Отличия от карты сервера: две биржи вместо семи, предыстория — три дня
вместо трёх лет, без фандинга и плеч по справочникам бирж. Правило входа то
же (autotrade_core.liq_ok). Ряды кешируются на диске; без сети — только кеш.
"""
from __future__ import annotations

import importlib
import json
import math
import os
import sys
import time
import urllib.parse
import urllib.request
from collections.abc import Callable
from typing import Any

import autotrade_core as core

ROW_MS = 900_000                 # 15-минутные снимки
WINDOW = 96                      # окно «сутки» — 96 свечей
PRE = 3 * WINDOW                 # учёт — ещё три дня до окна
MIN_ROWS = 8                     # меньше — биржа не в счёт (как у whale_api)
PX_TOL = 0.03                    # цена биржи дальше 3 % от цены сделки — другой актив
MULTS = ("1000", "1000000")      # 1000PEPE, 1000000MOG
BINANCE = "https://fapi.binance.com"
BYBIT = "https://api.bybit.com/v5/market"
Row = tuple[int, float, float, float, float, float]   # (t, с; макс; мин; закрытие; интерес $; доля покупок)
Get = Callable[[str], object]


def http_json(url: str) -> object:
    """GET JSON публичного API биржи; сбой — None."""
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (compatible; backtest)"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read(16 << 20))
    except (OSError, ValueError):
        return None


def load_model(wt_dir: str) -> Any:
    """Модель карты (wt_liqmap.py) из Wallet-Tracker; нет — None (повтор без карты)."""
    path = os.path.abspath(os.path.expanduser(wt_dir))
    if not os.path.isfile(os.path.join(path, "wt_liqmap.py")):
        return None
    if path not in sys.path:
        sys.path.append(path)
    try:
        return importlib.import_module("wt_liqmap")
    except Exception:  # noqa: BLE001 — модель не загрузилась — повтор без карты, а не падение
        return None


def _f(v: object) -> float:
    try:
        x = float(v)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return math.nan
    return x if math.isfinite(x) else math.nan


def _t(v: object) -> int:
    """Метка времени биржи (мс, числом или строкой) → секунды на сетке 15 мин; мусор — 0."""
    x = _f(v)
    return int(x) // ROW_MS * ROW_MS // 1000 if x > 0 else 0


def clean(rows: object) -> list[Row]:
    """Ряды без мусора: конечные числа, 0 < мин ≤ макс, закрытие > 0, интерес
    ≥ 0, доля покупок 0–1 (иначе 0,5); по времени, без повторов."""
    out: dict[int, Row] = {}
    for r in rows if isinstance(rows, (list, tuple)) else []:
        if not isinstance(r, (list, tuple)) or len(r) != 6:
            continue
        t, hi, low, c, oi, bs = (_f(x) for x in r)
        if not (t > 0 and 0 < low <= hi and c > 0 and oi >= 0):
            continue
        out[int(t)] = (int(t), max(hi, c), min(low, c), c, oi, bs if 0 <= bs <= 1 else 0.5)
    return [out[t] for t in sorted(out)]


def _list(v: object, *path: str) -> list[Any]:
    for k in path:
        v = v.get(k) if isinstance(v, dict) else None
    return v if isinstance(v, list) else []


def _spans(start: int, end: int, n: int) -> list[tuple[int, int]]:
    """Отрезки [a, b] (мс) по n свечей от start до end — по одному запросу."""
    out = []
    a = start // ROW_MS * ROW_MS
    while a <= end:
        b = min(end, a + n * ROW_MS - 1)
        out.append((a, b))
        a = b + 1
    return out


def binance(get: Get, sym: str, start: int, end: int) -> list[Row]:
    """Binance USDT-M: свечи, интерес ($) и доля покупок за [start, end], мс."""
    s = urllib.parse.quote(f"{sym}USDT")
    oi: dict[int, float] = {}
    kl: dict[int, list[Any]] = {}
    tk: dict[int, float] = {}
    for a, b in _spans(start, end, 500):
        q = f"symbol={s}&period=15m&limit=500&startTime={a}&endTime={b}"
        for r in _list(get(f"{BINANCE}/futures/data/openInterestHist?{q}")):
            if isinstance(r, dict):
                oi[_t(r.get("timestamp"))] = _f(r.get("sumOpenInterestValue"))
        for r in _list(get(f"{BINANCE}/futures/data/takerlongshortRatio?{q}")):
            if isinstance(r, dict):
                x = _f(r.get("buySellRatio"))
                tk[_t(r.get("timestamp"))] = x / (1 + x) if x >= 0 else 0.5
    for a, b in _spans(start, end, 1500):
        for k in _list(get(f"{BINANCE}/fapi/v1/klines?symbol={s}&interval=15m&limit=1500&startTime={a}&endTime={b}")):
            if isinstance(k, list) and len(k) >= 5:
                kl[_t(k[0])] = k
    return [(t, _f(kl[t][2]), _f(kl[t][3]), _f(kl[t][4]), v, tk.get(t, 0.5))
            for t, v in sorted(oi.items()) if t and t in kl]


def _bybit_oi(r: dict[str, Any]) -> float:
    """Интерес Bybit с одной стороны: singleOpenInterest, нет — половина openInterest."""
    one = _f(r.get("singleOpenInterest"))
    return one if one > 0 else _f(r.get("openInterest")) / 2


def bybit(get: Get, sym: str, start: int, end: int) -> list[Row]:
    """Bybit linear: свечи и интерес (монеты × закрытие = $) за [start, end], мс.
    Доля покупок — 0,5 (её заменит доля Binance, см. join)."""
    s = urllib.parse.quote(f"{sym}USDT")
    oi: dict[int, float] = {}
    kl: dict[int, list[Any]] = {}
    for a, b in _spans(start, end, 200):
        for r in _list(get(f"{BYBIT}/open-interest?category=linear&symbol={s}&intervalTime=15min&limit=200"
                           f"&startTime={a}&endTime={b}"), "result", "list"):
            if isinstance(r, dict):
                oi[_t(r.get("timestamp"))] = _bybit_oi(r)
    for a, b in _spans(start, end, 1000):
        for k in _list(get(f"{BYBIT}/kline?category=linear&symbol={s}&interval=15&limit=1000&start={a}&end={b}"),
                       "result", "list"):
            if isinstance(k, list) and len(k) >= 5:
                kl[_t(k[0])] = k
    return [(t, _f(kl[t][2]), _f(kl[t][3]), _f(kl[t][4]), q * _f(kl[t][4]), 0.5)
            for t, q in sorted(oi.items()) if t and t in kl]


def with_mult(fn: Callable[[Get, str, int, int], list[Row]]) -> Callable[[Get, str, int, int], list[Row]]:
    """Не нашлось под своим именем — под множителем (1000PEPE), цена — обратно за монету."""
    def wrapped(get: Get, sym: str, start: int, end: int) -> list[Row]:
        rows = fn(get, sym, start, end)
        if len(rows) >= MIN_ROWS:
            return rows
        for m in MULTS:
            got = fn(get, m + sym, start, end)
            if len(got) >= MIN_ROWS:
                k = float(m)
                return [(t, h / k, lo / k, c / k, v, bs) for t, h, lo, c, v, bs in got]
        return rows
    return wrapped


SOURCES: dict[str, Callable[[Get, str, int, int], list[Row]]] = {"Binance": with_mult(binance), "Bybit": with_mult(bybit)}


class LiqHist:
    """Ряды бирж по монете за весь повтор (один раз, кеш на диске) и карта
    на момент t: cum(sym, t, px) — поле «cum» карты ({2: {"L", "S"}}) или None."""

    def __init__(self, model: Any, cache: str, get: Get, start: int, end: int, offline: bool, pause: float) -> None:
        self.model, self.cache, self.get = model, cache, get
        self.start, self.end, self.offline, self.pause = start, end, offline, pause
        self.rows: dict[str, dict[str, list[Row]]] = {}
        self.memo: dict[tuple[str, int], object] = {}
        self.requests = 0
        os.makedirs(cache, exist_ok=True)

    def _get(self, url: str) -> object:
        if self.requests and self.pause:
            time.sleep(self.pause)
        self.requests += 1
        try:
            return self.get(url)
        except Exception:  # noqa: BLE001 — сеть: сбой — без рядов этой биржи, а не падение
            return None

    def _file(self, sym: str) -> str:
        return os.path.join(self.cache, f"liq_{core.safe_name(sym)}_15m.json")

    def series(self, sym: str) -> dict[str, list[Row]]:
        """Ряды бирж по монете: из кеша, недостающее — запросом."""
        if sym in self.rows:
            return self.rows[sym]
        have: dict[str, list[Row]] = {}
        try:
            with open(self._file(sym), encoding="utf-8") as f:
                raw = json.load(f)
            if isinstance(raw, dict):
                have = {str(k): clean(v) for k, v in raw.items() if isinstance(v, list)}
        except (OSError, ValueError):
            pass
        # Кеш покрывает отрезок — без сети; конец — последней закрытой свечой
        # до end (свежая история дописывается следующим прогоном).
        need = (self.end - ROW_MS) // ROW_MS * ROW_MS // 1000
        got: dict[str, list[Row]] = {}
        for ex, fn in SOURCES.items():
            rows = have.get(ex, [])
            if not self.offline and (not rows or rows[0][0] * 1000 > self.start or rows[-1][0] < need):
                fresh = clean(fn(self._get, sym, self.start, self.end))
                merged = {r[0]: r for r in rows} | {r[0]: r for r in fresh}
                rows = [merged[t] for t in sorted(merged)]
            if rows:
                got[ex] = rows
        if got != have and got:
            tmp = self._file(sym) + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({k: [list(r) for r in v] for k, v in got.items()}, f)
            os.replace(tmp, self._file(sym))
        self.rows[sym] = got
        return got

    def cum(self, sym: str, t: int, px: float) -> object:
        """Карта «сутки» на момент t (мс) при цене px: поле «cum» или None."""
        key = (sym, t // ROW_MS)
        if key in self.memo:
            return self.memo[key]
        cut = (t - ROW_MS) // 1000            # снимок + 15 мин ≤ t — свеча закрыта
        got: dict[str, list[Row]] = {}
        series = self.series(sym)
        for ex, rows in series.items():
            part = [r for r in rows if r[0] <= cut][-(WINDOW + PRE):]
            if len(part) >= MIN_ROWS and cut - part[-1][0] < 3600 and abs(part[-1][3] / px - 1) <= PX_TOL:
                got[ex] = part
        res: object = None
        if got and px > 0:
            # Доля покупок у бирж без своей — Binance (как рыночная у whale_api).
            share = {r[0]: r[5] for r in got.get("Binance", [])}
            got = {ex: rows if ex == "Binance" else [(*r[:5], share.get(r[0], r[5])) for r in rows]
                   for ex, rows in got.items()}
            ref = max(got.values(), key=lambda r: r[-1][4])[-WINDOW:]
            if len(ref) >= WINDOW // 2:
                m = self.model.build_map(sym, core.LIQ_RANGE, got, ref, t // 1000, px)
                res = m.get("cum") if isinstance(m, dict) else None
        self.memo[key] = res
        return res
