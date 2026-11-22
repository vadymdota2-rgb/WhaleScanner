# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Общее для торговли (autotrade_live.py) и бэктеста-повтора
(autotrade_replay.py): база бота только на чтение, коды сделок, окно китов,
имя бумаги на OKX, публичный запрос к OKX, предел памяти.

Раньше это жило в бэктесте-сетке (autotrade_backtest.py) вместе с ~1500
строками, которые служба больше не запускает; сетка удалена, общее — здесь.
"""
from __future__ import annotations

import json
import os
import re
import sqlite3
import urllib.parse
import urllib.request
from collections import deque
from typing import Any, ClassVar

OKX_API = "https://www.okx.com"
# Имена на OKX, которые отличаются от Hyperliquid (сверено по цене в ноябре
# 2026). Совпадение имени ничего не доказывает: xyz:STX — акции Seagate, а STX
# на OKX — токен Stacks; поэтому вход всё равно сверяет цену (±2 %).
OKX_ALIASES = {"GOLD": "XAU", "SILVER": "XAG", "COPPER": "XCU", "PLATINUM": "XPT", "PALLADIUM": "XPD",
               "NATGAS": "NG", "BRENTOIL": "BZ", "SP500": "US500", "XYZ100": "US100", "ANTH": "ANTHROPIC",
               "OAI": "OPENAI", "SMSN": "SAMSUNG", "SKHX": "SKHYNIX", "TCNT": "TENCENT"}
NANOS = 1_000_000_000
FLOW_MIN_DAYS = 7           # потоки на биржи: истории меньше 7 суток — правило к монете не применяется
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

    count — кошельков от порога (китов); big_n / big_u — их нано $ и штуки
    (у кого штуки известны) — для средней цены китов. У кошелька — ещё и
    объём без плеча (маржа: номинал ÷ плечо; плечо неизвестно — номинал):
    по нему сравниваются стороны «за» и «против» (правило владельца)."""
    __slots__ = ("big_k", "big_n", "big_u", "count", "limit", "orders", "per")

    def __init__(self, limit: int) -> None:
        self.limit = limit
        self.orders: deque[tuple[int, str, int, float, int]] = deque()
        self.per: dict[str, list[Any]] = {}   # кошелёк → [нано $, штук, сделок, сделок со штуками, нано $ без плеча]
        self.count = 0
        self.big_n = 0
        self.big_u = 0.0
        self.big_k = 0

    def _change(self, wallet: str, dn: int, du: float, dk: int, dku: int, dm: int) -> None:
        v = self.per.get(wallet)
        n0, u0, k0, ku0, m0 = v if v is not None else (0, 0.0, 0, 0, 0)
        k1, ku1 = k0 + dk, ku0 + dku
        n1 = n0 + dn if k1 else 0
        m1 = m0 + dm if k1 else 0
        u1 = u0 + du if ku1 else 0.0
        self.count += (k1 > 0 and n1 >= self.limit) - (k0 > 0 and n0 >= self.limit)
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
            self.per[wallet] = [n1, u1, k1, ku1, m1]
        else:
            self.per.pop(wallet, None)

    def add(self, ts: int, wallet: str, nanos: int, units: float, margin: int | None = None) -> None:
        m = nanos if margin is None else margin
        item = (ts, wallet, nanos, units, m)
        if self.orders and ts < self.orders[-1][0]:
            # Опоздавшая сделка (бот записал её позже) — на своё место по
            # времени: выброс старше окна идёт слева и требует порядка.
            i = len(self.orders)
            while i > 0 and self.orders[i - 1][0] > ts:
                i -= 1
            self.orders.insert(i, item)
        else:
            self.orders.append(item)
        self._change(wallet, nanos, units, 1, int(units > 0), m)

    def expire(self, before: int) -> None:
        while self.orders and self.orders[0][0] < before:
            _t, w, n, u, m = self.orders.popleft()
            self._change(w, -n, -u, -1, -int(u > 0), -m)


class Flows:
    """Заводы монеты на биржи и выводы с бирж (правило владельца: завод —
    плюс шорту, вывод — плюс лонгу). Данные — свои, из баз бота, только
    чтение: биткоин — btc.db (btc_flow: по блокам и биржам, сатоши), токены
    BSC — bsc_ex.db (bsc_ex_flow: по 10 минут, биржам и токенам, нано $;
    «native:bnb» — BNB). Тикер токена BSC — из whale_bot.db (token_cache), у
    известных — свой. Остального (ETH, SOL, HYPE, акций, металлов) нет —
    z() отдаёт None, и правило к монете не применяется.

    z(coin, t) — насколько чистый завод за 24 часа до t (заводы минус выводы)
    необычен для этой монеты: (завод за 24 ч − среднее) / разброс дневных
    заводов за 30 дней до этого. Плюс — заводят (к продаже), минус — выводят.
    Единицы не важны: монета сравнивается сама с собой. Истории меньше
    FLOW_MIN_DAYS дней или разброс 0 — None."""

    BSC_SYMS: ClassVar[dict[str, str]] = {"native:bnb": "BNB", "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c": "BNB",
                "0x2170ed0880ac9a755fd29b2688956bd959f933f8": "ETH", "0x0e09fabb73bd3ade0a17ecc321fd13a19e81ce82": "CAKE"}
    STABLE: ClassVar[frozenset[str]] = frozenset({"USDT", "USDC", "BUSD", "FDUSD", "USD1", "DAI", "TUSD"})

    def __init__(self, btc_db: str, bscex_db: str, main_db: str) -> None:
        self.btc_db, self.bscex_db = btc_db, bscex_db
        self.cache: dict[tuple[str, int], float | None] = {}
        self.bsc: dict[str, list[str]] = {}   # тикер → токены BSC
        if not os.path.isfile(bscex_db):
            return
        con = open_ro(bscex_db)
        try:
            toks = [str(r[0]) for r in con.execute("SELECT DISTINCT token FROM bsc_ex_flow")]
        except sqlite3.Error:
            toks = []
        finally:
            con.close()
        names: dict[str, str] = {}
        if os.path.isfile(main_db) and toks:
            m = open_ro(main_db)
            try:
                names = {str(a).lower(): str(sy) for a, sy in m.execute("SELECT address, symbol FROM token_cache")}
            except sqlite3.Error:
                names = {}
            finally:
                m.close()
        for t in toks:
            sym = (self.BSC_SYMS.get(t.lower()) or names.get(t.lower()) or "").upper()
            if sym and sym not in self.STABLE and re.fullmatch(r"[A-Z0-9]{1,12}", sym):
                self.bsc.setdefault(sym, []).append(t)

    def covered(self, coin: str) -> bool:
        base, _ = okx_candidate(coin)
        return ":" not in coin and ((base == "BTC" and os.path.isfile(self.btc_db)) or base in self.bsc)

    def _days(self, coin: str, t_s: int) -> list[tuple[int, int]] | None:
        """[(завод, вывод)] за 31 сутки до t_s: [0] — последние 24 часа."""
        base, _ = okx_candidate(coin)
        if ":" in coin:
            return None
        args: tuple[Any, ...]
        if base == "BTC" and os.path.isfile(self.btc_db):
            path, q, args = (self.btc_db, "SELECT (? - ts) / 86400, SUM(in_sats), SUM(out_sats) FROM btc_flow "
                             "WHERE ts > ? AND ts <= ? GROUP BY 1", (t_s, t_s - 31 * 86400, t_s))
            first_q = "SELECT MIN(ts) FROM btc_flow"
        elif base in self.bsc:
            toks = self.bsc[base]
            marks = ",".join("?" * len(toks))
            path = self.bscex_db
            # Строка — 10 минут с её начала (ts): берём только закончившиеся к t_s — без будущего.
            q = (f"SELECT (? - ts) / 86400, SUM(in_usd), SUM(out_usd) FROM bsc_ex_flow "  # noqa: S608 — только «?»
                 f"WHERE token IN ({marks}) AND ts > ? AND ts <= ? GROUP BY 1")
            args = (t_s, *toks, t_s - 31 * 86400, t_s - 600)
            first_q = "SELECT MIN(ts) FROM bsc_ex_flow"
        else:
            return None
        con = open_ro(path)
        try:
            first = con.execute(first_q).fetchone()[0]
            rows = con.execute(q, args).fetchall()
        finally:
            con.close()
        if first is None:
            return None
        have = min(31, max(0, (t_s - int(first)) // 86400))  # сколько полных суток покрыто данными
        days = [(0, 0)] * have
        for d, i, o in rows:
            if 0 <= int(d) < have:
                days[int(d)] = (int(i or 0), int(o or 0))
        return days

    def z(self, coin: str, t_ms: int) -> float | None:
        t_s = t_ms // 1000
        key = (coin, t_s // 600)
        if key in self.cache:
            return self.cache[key]
        days = self._days(coin, t_s)
        out: float | None = None
        if days is not None and len(days) - 1 >= FLOW_MIN_DAYS:
            nets = [i - o for i, o in days]
            past = nets[1:]
            mean = sum(past) / len(past)
            sd = (sum((x - mean) ** 2 for x in past) / len(past)) ** 0.5
            out = (nets[0] - mean) / sd if sd > 0 else None
        if len(self.cache) > 50_000:
            self.cache.clear()
        self.cache[key] = out
        return out


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
