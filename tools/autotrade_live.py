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
- только фьючерсы (крипта, акции, металлы), бумага должна быть на OKX и цена
  сходиться с ценой китов (±2 %) — иначе это другой актив;
- не догоняем: цена OKX хуже средней цены китов больше чем на 0,3 % — пропуск;
- маржа — 10 % капитала счёта, плечо 10×, кросс;
- всегда одна открытая позиция: пока на счёте есть ЛЮБАЯ позиция (и
  открытая владельцем руками) — новую не открываем;
- выход — только тейк +1 % движения цены (лимитный ордер на бирже), без
  стопа и срока: пересиживаем; бот позицию сам никогда не закрывает;
- в одну и ту же бумагу — не чаще раза в 3 часа.

Кнопки «Старт»/«Стоп» во вкладке пишут switch.json: «Стоп» — новых входов
нет; открытая позиция и её тейк остаются (правило «никогда не закрываем»).

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
from typing import Any

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autotrade_backtest as bt  # noqa: E402 — общий код: окно китов, имена OKX, открытие базы на чтение

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
    "50124": "ключу или счёту закрыт рынок бессрочных фьючерсов: у ключа включите право «Торговля», "
             "а на счёте OKX — доступ к Perpetual (фьючерсам)",
}
NANOS = bt.NANOS

# Правила владельца.
MIN_WHALES = 2
MIN_WHALE_USD = 1000
WINDOW_MS = 2 * 3_600_000
CHASE = 0.003
MATCH_TOL = 0.02
MARGIN_FRAC = 0.10
LEVERAGE = 10
TP = 0.01
COIN_PAUSE_MS = 3 * 3_600_000

FRESH_MS = 15 * 60_000     # сигнал старше — не входим (бот мог отстать, момент ушёл)
POLL_SEC = 5               # опрос базы
SYNC_SEC = 15              # сверка счёта
INSTR_TTL = 3600           # каталог OKX — раз в час
TAG = "wt"                 # приставка clOrdId наших ордеров
# Отказы OKX по самой бумаге (не по счёту): этой бумагой торговать нельзя —
# исключить её на неделю и искать следующие сигналы, а не долбить ордерами.
# 50124 — рынок закрыт для ключа/страны (у OKX EEA — часть контрактов),
# 51001 — бумаги нет, 51155/51156 — ограничения по стране (compliance),
# 51010 — режим счёта не позволяет эту бумагу.
BLOCK_CODES = {"50124", "51001", "51155", "51156", "51010"}
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

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # type: ignore[no-untyped-def]  # noqa: ANN001, PLR0913
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
    req = urllib.request.Request(url, data=body, method=method,  # noqa: S310 — адрес проверен выше, https
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

    def call(self, method: str, path: str, params: dict | None = None, body: dict | None = None,
             private: bool = True) -> list:
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
    def instruments(self, inst_type: str = "SWAP") -> list[dict]:
        return self.call("GET", "/api/v5/public/instruments", {"instType": inst_type}, private=False)

    def ticker(self, inst: str) -> float:
        d = self.call("GET", "/api/v5/market/ticker", {"instId": inst}, private=False)
        return max(0.0, num(d[0].get("last"))) if d and isinstance(d[0], dict) else 0.0

    def config(self) -> dict:
        d = self.call("GET", "/api/v5/account/config")
        return d[0] if d and isinstance(d[0], dict) else {}

    def equity(self) -> float:
        d = self.call("GET", "/api/v5/account/balance")
        return max(0.0, num(d[0].get("totalEq"))) if d and isinstance(d[0], dict) else 0.0

    def positions(self) -> list[dict]:
        """ВСЕ позиции счёта — бессрочные, срочные фьючерсы, маржа, опционы:
        правило «одна позиция за раз» считает и ручные сделки владельца любого
        вида (раньше — только бессрочные: срочный фьючерс владельца бот не видел)."""
        return [p for p in self.call("GET", "/api/v5/account/positions")
                if isinstance(p, dict) and num(p.get("pos")) != 0]

    def set_leverage(self, inst: str, lever: int) -> None:
        self.call("POST", "/api/v5/account/set-leverage", body={"instId": inst, "lever": str(lever), "mgnMode": "cross"})

    def order(self, body: dict) -> str:
        d = self.call("POST", "/api/v5/trade/order", body=body)
        return str(d[0].get("ordId") or "") if d and isinstance(d[0], dict) else ""

    def get_order(self, inst: str, ord_id: str = "", cl_id: str = "") -> dict:
        p = {"instId": inst, **({"ordId": ord_id} if ord_id else {"clOrdId": cl_id})}
        d = self.call("GET", "/api/v5/trade/order", p)
        return d[0] if d and isinstance(d[0], dict) else {}

    def pending(self, inst: str) -> list[dict]:
        # Вид контракта — по имени: европейский бессрочный (…_XPERP-…) у OKX — FUTURES.
        return self.call("GET", "/api/v5/trade/orders-pending", {"instType": inst_type(inst), "instId": inst})

    def cancel(self, inst: str, ord_id: str) -> None:
        self.call("POST", "/api/v5/trade/cancel-order", body={"instId": inst, "ordId": ord_id})

    def history(self, inst: str) -> list[dict]:
        """Последние ордера по бумаге (за 7 дней) — чтобы узнать свои по метке clOrdId."""
        return self.call("GET", "/api/v5/trade/orders-history", {"instType": inst_type(inst), "instId": inst, "limit": "50"})

    def closed(self, inst: str) -> list[dict]:
        return self.call("GET", "/api/v5/account/positions-history", {"instType": inst_type(inst), "instId": inst})


def inst_type(inst: str) -> str:
    """Вид контракта OKX по имени: «…-SWAP» — SWAP, остальное (в т. ч.
    европейский бессрочный «BTC-USD_UM_XPERP-310815») — FUTURES."""
    return "SWAP" if inst.endswith("-SWAP") else "FUTURES"


# ── округление под правила бумаги ────────────────────────────────────────────

def fmt_dec(x: Decimal) -> str:
    s = format(x.normalize(), "f")
    return s if s not in ("-0", "") else "0"


def round_step(value: float, step: Any, mode: str) -> Decimal:
    """Округлить до шага бумаги. Шаг или значение — мусор (не конечное
    положительное) — 0: вызывающий такое не отправляет."""
    st = step if isinstance(step, Decimal) and step.is_finite() and step > 0 else dec(step)
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
        self.windows: dict[tuple[str, int], bt.Window] = {}
        self.last_ts = 0
        self.seen: dict[int, int] = {}   # tid → ts (против повторов при перекрытии опроса)
        self.limit = [MIN_WHALE_USD * NANOS]

    def _con(self) -> sqlite3.Connection:
        return bt.open_ro(self.db)

    def poll(self, now: int) -> list[dict]:
        """Новые строки базы → окна; сигналы: бумага и сторона, где сейчас ≥ 2
        китов от 1000 $ за 2 часа, и строка, на которой это случилось."""
        since = max(self.last_ts - 10 * 60_000, now - WINDOW_MS) if self.last_ts else now - WINDOW_MS
        con = self._con()
        try:
            rows = con.execute(
                "SELECT tid, wallet, coin, dir_code, ts, notional_nanos, ABS(CAST(sz AS REAL)) FROM hl_fills "
                "WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ? ORDER BY ts, tid", (since, now)).fetchall()
        finally:
            con.close()
        out: list[dict] = []
        for tid, wallet, coin, code, ts, nanos, units in rows:
            if tid in self.seen or not bt.is_perp(coin):
                continue
            self.seen[tid] = ts
            self.last_ts = max(self.last_ts, ts)
            side = 1 if code == bt.OPEN_LONG else -1
            w = self.windows.setdefault((coin, side), bt.Window(self.limit))
            w.expire(ts - WINDOW_MS)
            w.add(ts, wallet, int(nanos or 0), float(units or 0.0))
            if w.count[0] >= MIN_WHALES and w.big_u > 0:
                out.append({"coin": coin, "side": side, "ts": ts, "whales": w.count[0],
                            "ref_px": (w.big_n / NANOS) / w.big_u})
        cut = now - WINDOW_MS - 20 * 60_000
        self.seen = {t: s for t, s in self.seen.items() if s >= cut}
        return out

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
BT_PAUSE_SEC = 15                 # между запросами к Hyperliquid
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
    return math.inf if raw == "max" else int(raw) / 2**20


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
        self.script = script or os.path.join(os.path.dirname(os.path.abspath(__file__)), "autotrade_backtest.py")
        self.limit_mb = limit_mb
        self.proc: subprocess.Popen | None = None
        self.log = None
        self.st = self._read()
        self.st.update(running=False)
        if not self.st.get("next"):
            self.st["next"] = next_run(time.time())

    def _read(self) -> dict:
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
            if not on:
                self.proc.terminate()
                self.proc.wait(timeout=30)
                self._done(None, "остановлено кнопкой «Стоп»")
            elif rc is None and now - self.st.get("started", now) > BT_TIMEOUT_SEC:
                self.proc.kill()
                self.proc.wait(timeout=30)
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
             os.path.join(self.dir, "cache"), "--pause", str(BT_PAUSE_SEC), "--max-mem-mb", str(BT_MEM_MB)],
            stdout=self.log, stderr=subprocess.STDOUT, preexec_fn=lambda: os.nice(19))  # noqa: PLW1509 — однопоточная служба
        self.st.update(running=True, started=int(now), error="")
        self._write()

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
        self.st.update(running=False, finished=int(time.time()), error=why)
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
        tr.st["entered"] = {c: t for c, t in tr.st.get("entered", {}).items() if int(t) >= cut}
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

class Trader:
    def __init__(self, okx: Okx, signals: Signals, data_dir: str, clock: Callable[[], int] = now_ms,
                 log: Callable[[str], None] | None = None) -> None:
        self.okx, self.sig, self.clock = okx, signals, clock
        self.dir = os.path.join(data_dir, "live")
        self.switch = os.path.join(data_dir, "switch.json")
        os.makedirs(self.dir, exist_ok=True)
        self._log = log or (lambda s: print(s, flush=True))
        self.st = self._load()
        self.instr: dict[str, dict] = {}
        self.instr_at = 0.0
        self.pos_mode = ""
        self.lever_set: set[str] = set()
        self.positions: list[dict] = []
        self.equity = 0.0
        self.last_sync = 0.0

    # состояние
    def _load(self) -> dict:
        try:
            with open(os.path.join(self.dir, "state.json"), encoding="utf-8") as f:
                st = json.load(f)
                if isinstance(st, dict):
                    return st
        except (OSError, ValueError):
            pass
        return {"open": None, "entered": {}, "error": "", "events": []}

    def save(self) -> None:
        self.st.update(heartbeat=self.clock(), simulated=self.okx.simulated, equity=self.equity,
                       positions=[{k: p.get(k) for k in ("instId", "pos", "posSide", "avgPx", "markPx", "upl", "lever",
                                                         "mgnMode", "cTime")} for p in self.positions],
                       on=self.is_on(), rules={"whales": MIN_WHALES, "whale_usd": MIN_WHALE_USD, "window_h": 2,
                                               "chase_pct": CHASE * 100, "margin_pct": MARGIN_FRAC * 100,
                                               "leverage": LEVERAGE, "tp_pct": TP * 100, "coin_pause_h": 3})
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
    def instruments(self) -> dict[str, dict]:
        """Каталог бессрочных контрактов. Глобальный OKX — «BTC-USDT-SWAP»;
        европейский OKX (EEA) торгует бессрочными как FUTURES вида
        «BTC-USD_UM_XPERP-310815», а SWAP в каталоге видны, но торговать ими
        нельзя (50124). Где есть XPERP — берём его."""
        if not self.instr or time.time() - self.instr_at > INSTR_TTL:
            got: dict[str, dict] = {}
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
            self.st["account"] = {"posMode": self.pos_mode, "acctLv": cfg.get("acctLv")}
        self.positions = self.okx.positions()
        self.equity = self.okx.equity()
        self.last_sync = time.time()
        self.find_own()
        self._track_open()

    def _track_open(self) -> None:
        """Наша позиция: есть — тейк на месте? нет — закрылась (тейк или руками):
        записать сделку."""
        op = self.st.get("open")
        if not op:
            return
        live = [p for p in self.positions if p.get("instId") == op["inst"]]
        if live:
            p = live[0]
            op.update(mark=num(p.get("markPx")), upl=num(p.get("upl")))
            if not any(o.get("ordId") == op.get("tp_id") for o in self.okx.pending(op["inst"])):
                self._place_tp(op, abs(num(p.get("pos"))))
            return
        if self.clock() - op["opened"] < 60_000:
            return  # биржа ещё не показала только что открытую позицию
        hist = [h for h in self.okx.closed(op["inst"]) if int(h.get("uTime") or 0) >= op["opened"]]
        if not hist and self.clock() - op.get("gone_at", self.clock()) < 300_000:
            op.setdefault("gone_at", self.clock())  # позиции нет, а истории ещё нет — подождать до 5 минут
            return
        h = hist[0] if hist else {}
        pnl = num(h.get("realizedPnl"), num(h.get("pnl")))
        trade = {"coin": op["coin"], "inst": op["inst"], "side": op["side"], "opened": op["opened"],
                 "closed": int(num(h.get("uTime"), self.clock())), "px_in": op["px"], "px_out": num(h.get("closeAvgPx")),
                 "tp": op["tp"], "size": op["size"], "pnl": round(pnl, 4), "fee": num(h.get("fee")),
                 "funding": num(h.get("fundingFee")), "notional": op["notional"],
                 "why": "тейк" if num(h.get("closeAvgPx")) > 0 and abs(num(h.get("closeAvgPx")) / op["tp"] - 1) < 0.002 else "закрыта"}
        with open(os.path.join(self.dir, "trades.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps(trade, ensure_ascii=False) + "\n")
        for o in self.okx.pending(op["inst"]):  # оставшийся тейк — снять
            if str(o.get("clOrdId", "")).startswith(TAG):
                self.okx.cancel(op["inst"], str(o.get("ordId")))
        self.st["open"] = None
        self.event(f"закрыта {op['inst']} {'лонг' if op['side'] > 0 else 'шорт'}: {trade['why']}, PnL {pnl:+.2f} $")

    # вход
    def _ps(self, side: int) -> dict:
        return {"posSide": "long" if side > 0 else "short"} if self.pos_mode == "long_short_mode" else {}

    def _place_tp(self, op: dict, size: float) -> None:
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

    def candidates(self, sigs: list[dict]) -> list[dict]:
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

    def try_enter(self, s: dict) -> str:
        """Вход по сигналу; возвращает причину пропуска ('' — вошли)."""
        now = self.clock()
        if self.st.get("open") or self.positions:
            names = ", ".join(str(p.get("instId")) for p in self.positions) or str((self.st.get("open") or {}).get("inst"))
            return f"на счёте уже есть позиция ({names})"
        if now - int(self.st["entered"].get(s["coin"], 0)) < COIN_PAUSE_MS:
            return "в эту бумагу входили меньше 3 часов назад"
        base, scale = bt.okx_candidate(s["coin"])
        inst = self.inst_for(base)
        blocked = self.st.setdefault("blocked", {})
        b = blocked.get(inst)
        if b and now - int(b.get("at", 0)) < (BLOCK_MS if b.get("permanent") else FAIL_PAUSE_MS):
            return f"{inst}: OKX не даёт торговать ({b.get('why', '')})"
        ins = self.instruments().get(inst)
        if not ins:
            return f"{inst} нет на OKX"
        last = self.okx.ticker(inst)
        if last <= 0:
            return "нет цены OKX"
        hl_px = last * scale
        if abs(hl_px / s["ref_px"] - 1) > MATCH_TOL:
            return f"{inst} — другой актив (цена {last} против {s['ref_px'] / scale:.6g})"
        if s["side"] * (hl_px / s["ref_px"] - 1) > CHASE:
            return f"цена ушла от китов дальше {CHASE * 100:g} %"
        if self.equity <= 0:
            return "капитал счёта 0"
        notional = self.equity * MARGIN_FRAC * LEVERAGE
        # Параметры бумаги из каталога — только конечные положительные: мусор
        # в каталоге не должен дать ордер неверного размера.
        ct_val, ct_mult = dec(ins.get("ctVal")), dec(ins.get("ctMult") or "1")
        step, min_sz, tick = dec(ins.get("lotSz")), dec(ins.get("minSz")), dec(ins.get("tickSz"))
        if ct_val is None or ct_mult is None or step is None or min_sz is None or tick is None:
            return f"{inst}: неверные параметры бумаги в каталоге OKX"
        ct_d = ct_val * ct_mult
        ct = float(ct_d)
        # Десятичными, не двоичными дробями: 10 000 / (10 000 000 × 0,00001) — ровно 100, а не 99,999…
        # Размер контракта — в монете (ctValCcy = BTC) или уже в долларах (USD/USDC/USDT).
        in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
        qty = Decimal(repr(round(notional, 6))) / (ct_d if in_usd else ct_d * Decimal(repr(last)))
        sz = (qty / step).to_integral_value(rounding=ROUND_DOWN) * step
        if sz < min_sz:
            return f"позиция {notional:.0f} $ меньше минимальной на OKX"
        cl = f"{TAG}in{now % 10**12}"
        try:
            if inst not in self.lever_set:
                self.okx.set_leverage(inst, LEVERAGE)
                self.lever_set.add(inst)
            # Сначала — запись «входим» (если служба упадёт посреди, при старте
            # ордер найдётся по clOrdId, а не будет повторён).
            self.st["entering"] = {"inst": inst, "cl": cl, "at": now, "coin": s["coin"], "side": s["side"],
                                   "whales": s["whales"], "ref_px": s["ref_px"] / scale, "signal_ts": s["ts"]}
            self.save()
            ord_id = self.okx.order({"instId": inst, "tdMode": "cross", "side": "buy" if s["side"] > 0 else "sell",
                                     "ordType": "market", "sz": fmt_dec(sz), "clOrdId": cl, **self._ps(s["side"])})
        except OkxError as e:
            # Ордер не принят — позиции нет. Бумагу — в исключённые (по самой
            # бумаге — на неделю, иначе — на 15 минут), сигналы — дальше.
            self.st.pop("entering", None)
            permanent = e.code in BLOCK_CODES
            blocked[inst] = {"at": now, "why": str(e).split(" [")[0][:200], "permanent": permanent}
            self.event(f"{inst}: ордер не принят — {e}; "
                       + ("бумага исключена на 7 дней" if permanent else "пауза по бумаге 15 минут"))
            return f"{inst}: ордер не принят ({e.code})"
        o: dict = {}
        for _ in range(10):
            o = self.okx.get_order(inst, ord_id)
            if o.get("state") == "filled":
                break
            time.sleep(0.5)
        # Цена и объём исполнения — с биржи; мусор или ноль — цена тикера и наш объём.
        fill_px = num(o.get("avgPx")) if num(o.get("avgPx")) > 0 else last
        filled = num(o.get("accFillSz")) if 0 < num(o.get("accFillSz")) <= float(sz) else float(sz)
        tp = round_step(fill_px * (1 + s["side"] * TP), fmt_dec(tick), ROUND_CEILING if s["side"] > 0 else ROUND_FLOOR)
        op = {"coin": s["coin"], "inst": inst, "side": s["side"], "opened": now, "px": fill_px, "size": filled,
              "notional": round(filled * ct * (1 if in_usd else fill_px), 2), "tp": float(tp), "tp_s": fmt_dec(tp), "whales": s["whales"],
              "ref_px": s["ref_px"] / scale, "signal_ts": s["ts"], "in_id": ord_id}
        self.st["open"] = op
        self.st["entered"][s["coin"]] = now
        self.st.pop("entering", None)
        self.event(f"вход {inst} {'лонг' if s['side'] > 0 else 'шорт'} {fmt_dec(sz)} по {fill_px} "
                   f"(≈{op['notional']:.0f} $, китов {s['whales']}), тейк {op['tp_s']}")
        self._place_tp(op, filled)
        self.save()
        return ""

    def _side_of(self, p: dict) -> int:
        ps = str(p.get("posSide") or "net")
        if ps in ("long", "short"):
            return 1 if ps == "long" else -1
        return 1 if num(p.get("pos")) > 0 else -1

    def adopt(self, p: dict, order: dict, info: dict | None = None) -> None:
        """Позиция открыта нашим ордером, но не записана (служба упала между
        ордером и записью): принять как свою и поставить тейк."""
        inst = str(p["instId"])
        side = self._side_of(p)
        ins = self.instruments().get(inst, {})
        px = num(p.get("avgPx")) or num(order.get("avgPx"))
        size = abs(num(p.get("pos")))
        tp = round_step(px * (1 + side * TP), ins.get("tickSz", "0.0001"), ROUND_CEILING if side > 0 else ROUND_FLOOR)
        in_usd = str(ins.get("ctValCcy", "")).upper() in ("USD", "USDC", "USDT")
        ct = num(ins.get("ctVal")) * num(ins.get("ctMult"), 1.0)
        info = info or {}
        opened = int(order.get("fillTime") or order.get("cTime") or info.get("at") or self.clock())
        coin = info.get("coin") or inst.split("-", 1)[0]
        op = {"coin": coin, "inst": inst, "side": side, "opened": opened, "px": px, "size": size,
              "notional": round(size * ct * (1 if in_usd else px), 2), "tp": float(tp), "tp_s": fmt_dec(tp),
              "whales": info.get("whales"), "ref_px": info.get("ref_px"), "signal_ts": info.get("signal_ts"),
              "in_id": str(order.get("ordId") or ""), "adopted": True}
        self.st["open"] = op
        self.st["entered"][coin] = opened
        self.event(f"найдена своя позиция без записи: {inst} {'лонг' if side > 0 else 'шорт'} {fmt_dec(Decimal(str(size)))} "
                   f"по {px} — принята, тейк {op['tp_s']}")
        pend = [o for o in self.okx.pending(inst) if str(o.get("clOrdId", "")).startswith(TAG)]
        if pend:  # тейк уже стоит (упали после него) — не дублировать
            op["tp_id"] = str(pend[0].get("ordId"))
        else:
            self._place_tp(op, size)
        self.save()

    def find_own(self) -> None:
        """Позиции на счёте, которые бот не ведёт: открыты нашим рыночным
        ордером (метка clOrdId «wtin…» за последние 7 дней) — свои,
        принять. Нет такой метки — чужие (владельца): не трогать. Каждую
        бумагу — не чаще раза в час."""
        if self.st.get("open"):
            return
        checked = self.__dict__.setdefault("_checked", {})
        for p in self.positions:
            inst = str(p.get("instId") or "")
            if not inst or time.time() - checked.get(inst, 0) < 3600:
                continue
            checked[inst] = time.time()
            side = self._side_of(p)
            try:
                hist = self.okx.history(inst)
            except OkxError as e:
                self.event(f"{inst}: история ордеров недоступна — {e}")
                continue
            mine = [o for o in hist
                    if str(o.get("clOrdId", "")).startswith(f"{TAG}in") and o.get("state") in ("filled", "partially_filled")
                    and (1 if o.get("side") == "buy" else -1) == side
                    and self.clock() - int(o.get("cTime") or 0) < 7 * 86_400_000]
            if mine:
                info = self.st.get("entering") if (self.st.get("entering") or {}).get("inst") == inst else None
                self.adopt(p, mine[0], info)
                self.st.pop("entering", None)
                return

    def recover(self) -> None:
        """После перезапуска: вход был начат, но не записан. Ордер исполнен и
        позиция есть — принять её как свою и поставить тейк; ордера нет —
        входа не было."""
        e = self.st.get("entering")
        if not e:
            return
        try:
            o = self.okx.get_order(e["inst"], cl_id=e["cl"])
        except OkxError:
            o = {}
        self.sync()
        live = [p for p in self.positions if p.get("instId") == e["inst"]]
        if o and o.get("state") in ("filled", "partially_filled") and live and not self.st.get("open"):
            self.adopt(live[0], o, e)
        elif o:
            self.event(f"после перезапуска: ордер входа {e['inst']} — {o.get('state')}, позиции нет")
        self.st.pop("entering", None)
        self.save()

    def step(self) -> None:
        sigs = self.sig.poll(self.clock())
        if time.time() - self.last_sync >= SYNC_SEC or sigs:
            self.sync()
        if not self.is_on():
            self.save()
            return
        for s in self.candidates(sigs):
            why = self.try_enter(s)
            if not why:
                break
            self.st["last_skip"] = {"coin": s["coin"], "side": s["side"], "why": why, "at": self.clock()}
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
    me = [os.path.abspath(__file__), bt.__file__]
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
            except OSError as e:
                tr.event(f"очистка: {e}")
        try:
            bts.tick(tr.is_on())
        except (OSError, subprocess.SubprocessError) as e:
            tr.event(f"бэктест: {e}")
        try:
            tr.step()
            tr.st["error"] = ""
            fails = 0
        except Exception as e:  # noqa: BLE001 — шаг не должен ронять службу: сбой — в журнал и пауза
            fails += 1
            tr.st["error"] = str(e)[:300]
            tr.event(f"сбой: {e}")
            try:
                tr.save()
            except OSError:
                pass
            time.sleep(min(300, 5 * 2 ** min(fails, 6)))
        # Код обновился (автодеплой) — выйти: systemd поднимет с новым. Идёт
        # пересчёт — дождаться его (иначе systemd убьёт его вместе со службой).
        if bts.proc is None and [os.path.getmtime(p) for p in me] != stamp:
            tr.event("код обновился — перезапуск")
            return 0
        time.sleep(POLL_SEC)


if __name__ == "__main__":
    sys.exit(main())
