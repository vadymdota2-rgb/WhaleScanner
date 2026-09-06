#!/usr/bin/env python3
"""Read-only(+mutations) JSON API over whale_bot.db + hyperliquid.db.
Stdlib only. Run from WhaleScanner working directory on the VPS."""
from __future__ import annotations

import hashlib
import hmac
import json
import math
import os
import re
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

# Петля по умолчанию: наружу API выходит только через nginx. Раньше здесь
# было 0.0.0.0, и обе базы бота слушали любой IP.
HOST = os.environ.get("WHALE_API_HOST", "127.0.0.1")
PORT = int(os.environ.get("WHALE_API_PORT", "8090"))
WS_DIR = os.environ.get("WHALE_SCANNER_DIR") or (
    "/home/vadymdota2/WhaleScanner"
    if os.path.isdir("/home/vadymdota2/WhaleScanner")
    else os.path.abspath(os.path.expanduser("~/WhaleScanner"))
)
if not os.path.isdir(WS_DIR):
    WS_DIR = os.path.abspath(".")


def _db_path(env_key: str, name: str) -> str:
    env = os.environ.get(env_key)
    if env and os.path.isfile(env):
        return os.path.abspath(env)
    for folder in (WS_DIR, os.path.abspath("."), os.path.expanduser("~/WhaleScanner")):
        p = os.path.join(folder, name)
        if os.path.isfile(p):
            return os.path.abspath(p)
    return os.path.abspath(os.path.join(WS_DIR, name))


DB = _db_path("WHALE_DB", "whale_bot.db")
HL_DB = _db_path("WHALE_HL_DB", "hyperliquid.db")
BOT_TOKEN = os.environ.get("WHALE_TG_TOKEN", "")
HL_INFO = "https://api.hyperliquid.xyz/info"

# Кому разрешено обращаться из браузера. Пустой список = заголовок не
# отдаётся вовсе. Звёздочка означала бы, что любой сайт может дёргать API
# из вкладки жертвы её же правами.
ALLOWED_ORIGINS = {
    o.strip().rstrip("/")
    for o in os.environ.get("WHALE_API_ORIGIN", "").split(",")
    if o.strip()
}
# Срок годности подписи Telegram. Без него однажды перехваченная initData
# работала бы вечно.
INIT_DATA_TTL = int(os.environ.get("WHALE_API_INITDATA_TTL", "86400"))
# Те же числа, что в premium.cpp и alert_settings.cpp. Раньше API их не знал
# и пускал мимо лимитов.
FREE_MAX_WALLETS = 1
PREMIUM_MAX_WALLETS = 50
MIN_THRESHOLD_USD = 50.0
MAX_THRESHOLD_USD = 1_000_000_000.0
# Потолок запросов с одного адреса: перебор chat_id упирается в него.
RATE_LIMIT_RPM = int(os.environ.get("WHALE_API_RPM", "120"))
# Общий секрет с nginx. Порт 8090 виден Cloud Run, а значит и всем прочим;
# ключ делает прямое обращение в обход прокси бесполезным.
API_KEY = os.environ.get("WHALE_API_KEY", "")

NANOS = 1_000_000_000.0
ADDR_RE = re.compile(r"^0x[a-fA-F0-9]{40}$")
COIN_RE = re.compile(r"\b([A-Z]{2,12})\b")
_hl_cache: dict[str, tuple[float, dict]] = {}
_pub_lock = threading.Lock()
_pub: dict = {"t": 0.0, "data": None}
PUB_TTL = 30.0
_addr_by_sym: dict[str, str] = {}
_px_hist: dict[str, tuple[float, list]] = {}
_hl_candles: dict[str, tuple[float, list]] = {}

GECKO_IMG = {
    "BTC": "https://coin-images.coingecko.com/coins/images/1/small/bitcoin.png",
    "ETH": "https://coin-images.coingecko.com/coins/images/279/small/ethereum.png",
    "SOL": "https://coin-images.coingecko.com/coins/images/4128/small/solana.png",
    "BNB": "https://coin-images.coingecko.com/coins/images/825/small/bnb-icon2_2x.png",
    "PEPE": "https://coin-images.coingecko.com/coins/images/29850/small/pepe-token.jpeg",
    "CAKE": "https://coin-images.coingecko.com/coins/images/12632/small/pancakeswap-cake-logo_%281%29.png",
    "ARB": "https://coin-images.coingecko.com/coins/images/16547/small/arb.jpg",
    "FLOKI": "https://coin-images.coingecko.com/coins/images/16746/small/PNG_image.png",
    "HYPE": "https://coin-images.coingecko.com/coins/images/50882/small/hyperliquid.jpg",
    "ENA": "https://coin-images.coingecko.com/coins/images/36530/small/ethena.png",
    "PUMP": "https://coin-images.coingecko.com/coins/images/67164/small/pump.jpg",
    "DOGE": "https://coin-images.coingecko.com/coins/images/5/small/dogecoin.png",
    "USDT": "https://coin-images.coingecko.com/coins/images/325/small/Tether.png",
    "USDC": "https://coin-images.coingecko.com/coins/images/6319/small/usdc.png",
    "DAI": "https://coin-images.coingecko.com/coins/images/9956/small/Badge_Dai.png",
}

HL_COIN = {
    "PEPE": "kPEPE",
    "FLOKI": "kFLOKI",
    "SHIB": "kSHIB",
    "BONK": "kBONK",
    "NVDA": "xyz:NVDA",
    "INTC": "xyz:INTC",
    "GOOGL": "xyz:GOOGL",
    "GOOG": "xyz:GOOGL",
}


# --- EIP-55 checksum: PancakeSwap и TrustWallet отдают логотипы только по
# --- адресу в смешанном регистре; в token_cache адреса в нижнем.
_KEC_RC=[0x0000000000000001,0x0000000000008082,0x800000000000808A,0x8000000080008000,
0x000000000000808B,0x0000000080000001,0x8000000080008081,0x8000000000008009,
0x000000000000008A,0x0000000000000088,0x0000000080008009,0x000000008000000A,
0x000000008000808B,0x800000000000008B,0x8000000000008089,0x8000000000008003,
0x8000000000008002,0x8000000000000080,0x000000000000800A,0x800000008000000A,
0x8000000080008081,0x8000000000008080,0x0000000080000001,0x8000000080008008]
_KEC_R=[[0,36,3,41,18],[1,44,10,45,2],[62,6,43,15,61],[28,55,25,21,56],[27,20,39,8,14]]
M=(1<<64)-1
def _rol(x,n): return ((x<<n)|(x>>(64-n)))&M
def _keccak_f(A):
    for rnd in range(24):
        C=[A[x][0]^A[x][1]^A[x][2]^A[x][3]^A[x][4] for x in range(5)]
        D=[C[(x-1)%5]^_rol(C[(x+1)%5],1) for x in range(5)]
        for x in range(5):
            for y in range(5): A[x][y]^=D[x]
        B=[[0]*5 for _ in range(5)]
        for x in range(5):
            for y in range(5): B[y][(2*x+3*y)%5]=_rol(A[x][y],_KEC_R[x][y])
        for x in range(5):
            for y in range(5): A[x][y]=B[x][y]^((~B[(x+1)%5][y])&B[(x+2)%5][y])&M
        A[0][0]^=_KEC_RC[rnd]
    return A
def keccak256(data: bytes) -> bytes:
    rate=136
    A=[[0]*5 for _ in range(5)]
    pad=data+b'\x01'+b'\x00'*((-len(data)-1)%rate)
    pad=bytearray(pad); pad[-1]^=0x80
    for off in range(0,len(pad),rate):
        blk=pad[off:off+rate]
        for i in range(rate//8):
            x,y=(i%5),(i//5)
            A[x][y]^=int.from_bytes(blk[i*8:i*8+8],'little')
        _keccak_f(A)
    out=b''
    for i in range(4):
        x,y=(i%5),(i//5)
        out+=A[x][y].to_bytes(8,'little')
    return out[:32]
def to_checksum(addr: str) -> str:
    a=(addr or '').lower().replace('0x','')
    if len(a)!=40: return addr or ''
    h=keccak256(a.encode()).hex()
    return '0x'+''.join(c.upper() if c.isalpha() and int(h[i],16)>=8 else c for i,c in enumerate(a))


# Логотипы, скачанные fetch_logos.py и уехавшие в образ Cloud Run.
# Манифест нужен, чтобы не предлагать фронту локальный адрес того, чего
# на диске нет: иначе каждый такой значок стоил бы лишнего запроса и 404.
LOGO_MANIFEST = os.environ.get(
    "WHALE_LOGO_MANIFEST",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "coins_manifest.json"),
)
_logo_local: dict[str, set[str]] = {"bsc": set(), "hl": set()}
_logo_mtime = 0.0


def _load_logo_manifest() -> None:
    global _logo_mtime
    try:
        m = os.path.getmtime(LOGO_MANIFEST)
    except OSError:
        return
    if m == _logo_mtime:
        return
    try:
        with open(LOGO_MANIFEST, encoding="utf-8") as f:
            raw = json.load(f)
        _logo_local["bsc"] = {str(x) for x in (raw.get("bsc") or [])}
        _logo_local["hl"] = {str(x) for x in (raw.get("hl") or [])}
        _logo_mtime = m
    except (OSError, ValueError):
        pass


def coin_icon(sym: str, addr: str = "") -> list[str]:
    """Список кандидатов по убыванию доверия. Площадку определяет адрес:
    он есть только у спотовых токенов BSC, у перпов Hyperliquid его нет.
    TradingView убран: он ищет по тикеру, а тикеры не уникальны — под PUMP
    и HYPE там лежали чужие проекты, и картинка грузилась успешно, из-за
    чего до правильной очередь не доходила."""
    _load_logo_manifest()
    key = (sym or "").upper().replace(" ", "")
    out: list[str] = []
    a = (addr or "").lower()
    if a.startswith("0x") and len(a) == 42:
        sumaddr = to_checksum(a)
        if sumaddr in _logo_local["bsc"]:
            out.append(f"/coins/bsc/{sumaddr}.png")
        out.append(f"/pcslogo/{sumaddr}.png")
        out.append(f"/twlogo/{sumaddr}/logo.png")
        return out
    if not key:
        return out
    alias = HL_COIN.get(key, key)
    for name in (alias, key) if alias != key else (alias,):
        if name in _logo_local["hl"]:
            safe = name.replace(":", "_").replace("/", "_")
            out.append(f"/coins/hl/{safe}.svg")
    out.append(f"/hllogo/{alias}.svg")
    if alias != key:
        out.append(f"/hllogo/{key}.svg")
    return out



def now() -> int:
    return int(time.time())


def open_db(path: str, write: bool = False) -> sqlite3.Connection | None:
    if not path or not os.path.isfile(path):
        return None
    con = sqlite3.connect(path, timeout=8, check_same_thread=False)
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA busy_timeout=8000")
    if not write:
        try:
            con.execute("PRAGMA query_only=ON")
        except sqlite3.Error:
            pass
    return con


_table_ok: dict[str, bool] = {}
_sym_cache: dict[str, str] = {}
_building = False


def cap_cache(d: dict, limit: int) -> None:
    """Словари-кэши росли без предела. Переполнился — выкидываем половину
    самых старых по порядку вставки."""
    if len(d) <= limit:
        return
    for k in list(d.keys())[: len(d) - limit // 2]:
        d.pop(k, None)


def table_exists(con: sqlite3.Connection, name: str) -> bool:
    hit = _table_ok.get(name)
    if hit is not None:
        return hit
    row = con.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)
    ).fetchone()
    ok = bool(row)
    _table_ok[name] = ok
    return ok


def cols(con: sqlite3.Connection, table: str) -> set[str]:
    return {r[1] for r in con.execute(f"PRAGMA table_info({table})")}


def usd(nanos) -> float:
    try:
        return float(nanos or 0) / NANOS
    except (TypeError, ValueError):
        return 0.0


def ago(ts: int) -> str:
    d = max(0, now() - int(ts or 0))
    if d < 60:
        return f"{d} сек"
    if d < 3600:
        return f"{d // 60} мин"
    if d < 86400:
        return f"{d // 3600} ч"
    return f"{d // 86400} д"


def short_addr(a: str) -> str:
    a = a or ""
    if len(a) < 12:
        return a
    return a[:6] + "…" + a[-4:]


def verify_init_data(raw: str) -> dict | None:
    """Разбирает initData Telegram и возвращает пользователя ТОЛЬКО при
    сошедшейся подписи. Любая неудача — None, вызывающий обязан считать
    запрос анонимным."""
    # Проверять подпись нечем — значит доверять нечему. Раньше при пустом
    # токене данные принимались как есть.
    if not raw or not BOT_TOKEN:
        return None
    parts = {}
    for chunk in raw.split("&"):
        if "=" not in chunk:
            continue
        k, v = chunk.split("=", 1)
        parts[k] = unquote(v)
    got = parts.pop("hash", "")
    # hash обязателен. Раньше условие было `if BOT_TOKEN and got`, поэтому
    # запрос БЕЗ hash проверку целиком пропускал: достаточно было прислать
    # user={"id":...} и любой чужой аккаунт открывался.
    if not got:
        return None
    check = "\n".join(f"{k}={parts[k]}" for k in sorted(parts))
    secret = hmac.new(b"WebAppData", BOT_TOKEN.encode(), hashlib.sha256).digest()
    expect = hmac.new(secret, check.encode(), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expect, got):
        return None
    # Срок годности. Telegram кладёт время выдачи в auth_date; без проверки
    # однажды утёкшая строка оставалась бы ключом навсегда.
    try:
        auth_date = int(parts.get("auth_date") or 0)
    except ValueError:
        return None
    if auth_date <= 0:
        return None
    age = now() - auth_date
    # Небольшой запас назад: часы клиента могут немного убежать вперёд.
    if age > INIT_DATA_TTL or age < -300:
        return None
    user_raw = parts.get("user") or ""
    try:
        user = json.loads(user_raw) if user_raw else {}
    except json.JSONDecodeError:
        return None
    uid = user.get("id")
    # id обязан быть целым числом: строкой сюда можно было бы протащить
    # что угодно, а он идёт прямо в chat_id.
    if not isinstance(uid, int) or uid <= 0:
        return None
    return {"id": str(uid), "lang": user.get("language_code") or "ru"}


class RateLimiter:
    """Скользящее окно на минуту по ключу. Держим в памяти: процесс один."""

    def __init__(self, rpm: int):
        self.rpm = max(1, rpm)
        self.hits: dict[str, list[float]] = {}
        self.lock = threading.Lock()

    def allow(self, key: str) -> bool:
        t = time.monotonic()
        with self.lock:
            # Заодно подметаем чужие протухшие записи, иначе словарь растёт
            # ровно на столько адресов, сколько к нам стучалось.
            if len(self.hits) > 4096:
                for k in [k for k, v in self.hits.items() if not v or t - v[-1] > 60.0]:
                    self.hits.pop(k, None)
            seq = [x for x in self.hits.get(key, ()) if t - x < 60.0]
            if len(seq) >= self.rpm:
                self.hits[key] = seq
                return False
            seq.append(t)
            self.hits[key] = seq
            return True


_limiter = RateLimiter(RATE_LIMIT_RPM)


def wallet_banned(con: sqlite3.Connection, addr: str) -> bool:
    """Тот же критерий, что в isPermanentlyBanned() бота."""
    if not table_exists(con, "ignored_wallets"):
        return False
    row = con.execute(
        "SELECT 1 FROM ignored_wallets WHERE wallet=? AND permanent=1", (addr.lower(),)
    ).fetchone()
    return row is not None


def wallet_limit(con: sqlite3.Connection, chat: str) -> int:
    """Лимит кошельков по подписке — как в premium.cpp."""
    if not table_exists(con, "users"):
        return FREE_MAX_WALLETS
    cset = cols(con, "users")
    if "is_premium" not in cset:
        return FREE_MAX_WALLETS
    row = con.execute(
        "SELECT is_premium, premium_expire FROM users WHERE chat_id=?", (chat,)
    ).fetchone()
    if row and row["is_premium"] and int(row["premium_expire"] or 0) > now():
        return PREMIUM_MAX_WALLETS
    return FREE_MAX_WALLETS


def hl_post(payload: dict, timeout: float = 6.0):
    req = urllib.request.Request(
        HL_INFO,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, ValueError):
        return None


def hl_state(addr: str) -> dict:
    key = addr.lower()
    hit = _hl_cache.get(key)
    if hit and time.monotonic() - hit[0] < 20:
        return hit[1]
    perp = hl_post({"type": "clearinghouseState", "user": addr}) or {}
    spot = hl_post({"type": "spotClearinghouseState", "user": addr}) or {}
    out = {"perp": perp, "spot": spot}
    _hl_cache[key] = (time.monotonic(), out)
    cap_cache(_hl_cache, 4096)
    return out


def parse_positions(addr: str) -> tuple[list, dict, float]:
    st = hl_state(addr)
    perp = st.get("perp") or {}
    spot = st.get("spot") or {}
    ms = perp.get("marginSummary") or {}
    equity = {
        "total": float(ms.get("accountValue") or 0),
        "spot": 0.0,
        "perp": float(ms.get("accountValue") or 0),
        "hip3": 0.0,
        "vaults": 0.0,
    }
    for b in spot.get("balances") or []:
        try:
            equity["spot"] += float(b.get("total") or 0)
        except (TypeError, ValueError):
            pass
    equity["total"] = equity["spot"] + equity["perp"]
    pos = []
    for ap in perp.get("assetPositions") or []:
        p = ap.get("position") or ap
        try:
            szi = float(p.get("szi") or 0)
        except (TypeError, ValueError):
            continue
        if abs(szi) < 1e-12:
            continue
        lev = p.get("leverage") or {}
        try:
            lev_n = int(float(lev.get("value") or 1))
        except (TypeError, ValueError):
            lev_n = 1
        try:
            entry = float(p.get("entryPx") or 0)
            now_px = float(p.get("positionValue") or 0) / abs(szi) if szi else 0
            pnl = float(p.get("unrealizedPnl") or 0)
            margin = float(p.get("marginUsed") or 0)
            liq = float(p.get("liquidationPx") or 0) if p.get("liquidationPx") not in (None, "") else 0
        except (TypeError, ValueError):
            continue
        pos.append(
            {
                "sym": str(p.get("coin") or "?").upper(),
                "long": szi > 0,
                "lev": max(1, lev_n),
                "size": abs(szi) * now_px if now_px else abs(szi),
                "entry": entry,
                "now": now_px or entry,
                "margin": margin,
                "liq": liq,
                "pnl": pnl,
                "pct": (pnl / margin * 100.0) if margin else 0.0,
                "funding": 0.0,
                "held": "",
                "isolated": str(lev.get("type") or "") == "isolated",
                "hist": [],
            }
        )
    d1 = 0.0
    try:
        d1 = float((perp.get("marginSummary") or {}).get("totalNtlPos") or 0)
        if equity["total"]:
            d1 = 0.0
    except (TypeError, ValueError):
        d1 = 0.0
    return pos, equity, d1


def symbol_of(cur: sqlite3.Connection, token: str) -> str:
    token = token or ""
    hit = _sym_cache.get(token)
    if hit is not None:
        return hit
    if token.startswith("0x") and len(token) >= 8:
        if table_exists(cur, "token_cache"):
            row = cur.execute(
                "SELECT symbol FROM token_cache WHERE lower(address)=?", (token.lower(),)
            ).fetchone()
            if row and row[0]:
                s = str(row[0]).upper().strip()
                if s and s != "UNKNOWN":
                    _sym_cache[token] = s
                    return s
        _sym_cache[token] = ""
        return ""
    _sym_cache[token] = (token.upper()[:12] or "")
    cap_cache(_sym_cache, 8192)
    return _sym_cache[token]


def ts_sec(ts) -> int:
    try:
        v = int(ts or 0)
    except (TypeError, ValueError):
        return 0
    if v > 10_000_000_000:
        return v // 1000
    return v


MAX_SPOT_USD_NANOS = 10_000_000_000_000_000
HL_MIN_CLOSED = 5
HL_MAX_CLOSED_30D = 200
DIR_OPEN_LONG, DIR_OPEN_SHORT = 1, 2
DIR_LIQ_LONG, DIR_LIQ_SHORT, DIR_LIQ_OTHER = 6, 7, 8


def spark(net: float) -> list[int]:
    base = 50
    step = 3 if net >= 0 else -3
    return [max(8, min(92, base + step * i + (i % 3))) for i in range(12)]


PX_FLOOR = {
    "BTC": 5_000.0,
    "ETH": 200.0,
    "SOL": 10.0,
    "BNB": 100.0,
    "XRP": 0.2,
    "HYPE": 1.0,
}


def _px_ok(sym: str, px: float) -> bool:
    try:
        v = float(px or 0)
    except (TypeError, ValueError):
        return False
    if v <= 0:
        return False
    floor = PX_FLOOR.get((sym or "").upper())
    if floor is None:
        return True
    return v >= floor


def _looks_spark(vals: list) -> bool:
    if not vals or len(vals) < 2:
        return False
    try:
        xs = [float(v) for v in vals]
    except (TypeError, ValueError):
        return False
    return min(xs) >= 0 and max(xs) <= 130


def _as_spark(vals: list, last: float) -> list[float]:
    if not vals:
        return []
    try:
        px = float(last or 0) or float(vals[-1] or 0)
    except (TypeError, ValueError):
        return []
    if px <= 0:
        return []
    if _looks_spark(vals) and px > 200:
        return [max(0.0, min(130.0, float(v))) for v in vals]
    out: list[float] = []
    for v in vals:
        try:
            p = float(v)
        except (TypeError, ValueError):
            continue
        if p <= 0:
            continue
        out.append(round(max(0.0, min(140.0, (p / px - 0.9) * 220.0)), 3))
    return out


def _down(vals: list[float], n: int) -> list[float]:
    if not vals:
        return []
    if len(vals) <= n:
        return vals
    last = n - 1
    return [vals[int(round(i * (len(vals) - 1) / last))] for i in range(n)]


def _chg_at(pts: list[tuple[int, float]], hours: int) -> float:
    if len(pts) < 2:
        return 0.0
    last_t, last_p = pts[-1]
    if last_p <= 0:
        return 0.0
    want = last_t - hours * 3600
    prev = pts[0][1]
    for t, p in pts:
        if t <= want:
            prev = p
        else:
            break
    if prev <= 0:
        return 0.0
    return round(100.0 * (last_p - prev) / prev, 2)


def _ensure_addr_map(cur: sqlite3.Connection) -> None:
    if _addr_by_sym or not table_exists(cur, "token_cache"):
        return
    try:
        rows = cur.execute("SELECT address, symbol FROM token_cache").fetchall()
    except sqlite3.Error:
        return
    for r in rows:
        s = str(r["symbol"] or "").upper().strip()
        a = str(r["address"] or "").lower()
        if s and s != "UNKNOWN" and a.startswith("0x") and len(a) >= 40:
            _addr_by_sym.setdefault(s, a)


def addr_of_sym(cur: sqlite3.Connection, sym: str, token: str = "") -> str:
    tok = (token or "").lower()
    if tok.startswith("0x") and len(tok) >= 40:
        return tok
    _ensure_addr_map(cur)
    return _addr_by_sym.get((sym or "").upper(), "")


def hist_spot(cur: sqlite3.Connection, addr: str) -> list[tuple[int, float]]:
    if not addr or not table_exists(cur, "token_price_history"):
        return []
    hit = _px_hist.get(addr)
    if hit and time.monotonic() - hit[0] < 30:
        return hit[1]
    since = now() - 30 * 86400
    try:
        rows = cur.execute(
            "SELECT ts, price_nanos FROM token_price_history "
            "WHERE address=? AND ts>=? AND price_nanos>0 ORDER BY ts",
            (addr, since),
        ).fetchall()
    except sqlite3.Error:
        rows = []
    pts = [(int(r["ts"]), usd(r["price_nanos"])) for r in rows if r["price_nanos"]]
    pts = [(t, p) for t, p in pts if p > 0]
    _px_hist[addr] = (time.monotonic(), pts)
    return pts


def hl_mids() -> dict[str, float]:
    hit = _hl_cache.get("_mids")
    if hit and time.monotonic() - hit[0] < 20:
        return hit[1]
    raw = hl_post({"type": "allMids"}, timeout=3.0) or {}
    out: dict[str, float] = {}
    if isinstance(raw, dict):
        for k, v in raw.items():
            try:
                out[str(k).upper()] = float(v)
            except (TypeError, ValueError):
                pass
    _hl_cache["_mids"] = (time.monotonic(), out)
    return out


def hist_perp(coin: str) -> list[tuple[int, float]]:
    key = (coin or "").upper()
    if not key:
        return []
    alias = HL_COIN.get(key, key)
    hit = _hl_candles.get(alias)
    if hit and time.monotonic() - hit[0] < 90:
        return hit[1]
    start = int((time.time() - 30 * 86400) * 1000)
    raw = hl_post(
        {
            "type": "candleSnapshot",
            "req": {"coin": alias, "interval": "1h", "startTime": start, "endTime": int(time.time() * 1000)},
        },
        timeout=3.0,
    )
    pts: list[tuple[int, float]] = []
    if isinstance(raw, list):
        for c in raw:
            if not isinstance(c, dict):
                continue
            try:
                px = float(c.get("c") or 0)
                ts = int(c.get("t") or 0)
            except (TypeError, ValueError):
                continue
            if px <= 0:
                continue
            if ts > 10_000_000_000:
                ts //= 1000
            pts.append((ts, px))
    pts.sort()
    _hl_candles[alias] = (time.monotonic(), pts)
    return pts


def _slices(pts: list[tuple[int, float]]) -> dict:
    if not pts:
        return {}
    tnow = pts[-1][0]
    def window(hours, n):
        cut = tnow - hours * 3600
        vals = [p for t, p in pts if t >= cut]
        if len(vals) < 2:
            vals = [p for _, p in pts[-max(2, n):]]
        return _down(vals, n)
    last = pts[-1][1]
    chg = _chg_at(pts, 24)
    return {
        "price": last,
        "chg": chg,
        "hists": {
            "1h": window(12, 12),
            "24h": window(24, 24),
            "7d": window(168, 28),
            "30d": window(720, 30),
        },
        "spark": window(24, 12),
        "c1": _chg_at(pts, 1),
        "c6": _chg_at(pts, 6),
        "c24": chg,
    }


def _sparkify(sl: dict, sym: str) -> dict:
    px = float(sl.get("price") or 0)
    if px <= 0:
        return sl
    hists = sl.get("hists") or {}
    out_h = {}
    for k, vals in hists.items():
        if not vals:
            out_h[k] = []
        elif _looks_spark(vals) and _px_ok(sym, px):
            out_h[k] = [float(v) for v in vals]
        else:
            out_h[k] = _as_spark(vals, px)
    sl["hists"] = out_h
    sp = sl.get("spark") or []
    if sp and not (_looks_spark(sp) and _px_ok(sym, px)):
        sl["spark"] = _as_spark(sp, px)
    return sl


def price_pack(cur: sqlite3.Connection, hl: sqlite3.Connection | None, sym: str, token: str = "", http: bool = False) -> dict:
    key = (sym or "").upper()
    addr = addr_of_sym(cur, sym, token)
    pts = hist_spot(cur, addr) if addr else []
    last = pts[-1][1] if pts else 0.0
    want_perp = (
        http
        or key in PX_FLOOR
        or len(pts) < 3
        or not _px_ok(key, last)
        or _looks_spark([p for _, p in pts])
    )
    if want_perp:
        extra = hist_perp(sym)
        if extra and (not _px_ok(key, last) or len(extra) >= max(3, len(pts))):
            pts = extra
    sl = _slices(pts)
    mids = hl_mids()
    mid = mids.get(key, 0.0)
    if mid > 0 and (not sl or not _px_ok(key, sl.get("price") or 0)):
        if sl:
            sl["price"] = mid
        else:
            sl = {"price": mid, "chg": 0.0, "hists": {}, "spark": [], "c1": 0, "c6": 0, "c24": 0}
    if not sl:
        return {}
    sl = _sparkify(sl, key)
    sl["addr"] = addr
    sl["icon"] = coin_icon(sym, addr)
    sl["real"] = True
    return sl


def parse_alert(msg: str, ts: int, name: str = "") -> dict:
    text = re.sub(r"<[^>]+>", " ", msg or "")
    text = re.sub(r"\s+", " ", text).strip()
    side = "LONG"
    if re.search(r"short|шорт|прода", text, re.I):
        side = "SHORT"
    elif re.search(r"long|лонг|куп", text, re.I):
        side = "LONG"
    skip = {"USD", "USDT", "USDC", "BSC", "HL", "PNL", "ROI", "WALLET", "ALERT"}
    sym = "—"
    for m in COIN_RE.findall(text.upper()):
        if m not in skip:
            sym = m
            break
    notional = 0.0
    m = re.search(r"\$[\s]?([0-9][0-9.,]*)[kKmMб]?", text)
    if m:
        raw = m.group(1).replace(",", "")
        try:
            notional = float(raw)
            if "m" in (m.group(0) or "").lower() or "м" in m.group(0).lower():
                notional *= 1_000_000
            elif "k" in (m.group(0) or "").lower() or "к" in m.group(0).lower():
                notional *= 1_000
        except ValueError:
            notional = 0.0
    return {
        "name": name or short_addr(""),
        "side": side,
        "sym": sym,
        "notional": notional,
        "account": 0,
        "margin": 0,
        "roi": 0,
        "t": ago(ts),
        "raw": text[:240],
    }


def load_me(cur: sqlite3.Connection, chat: str) -> dict:
    plan, thr, lang, prem_until = "free", 10000.0, "ru", 0
    if table_exists(cur, "users"):
        cset = cols(cur, "users")
        fields = ["threshold_nanos", "language"]
        if "is_premium" in cset:
            fields += ["is_premium", "premium_expire"]
        row = cur.execute(
            f"SELECT {', '.join(fields)} FROM users WHERE chat_id=?", (chat,)
        ).fetchone()
        if row:
            thr = max(50.0, usd(row["threshold_nanos"]))
            lang = row["language"] or "ru"
            if "is_premium" in cset and row["is_premium"] and int(row["premium_expire"] or 0) > now():
                plan = "premium"
                prem_until = int(row["premium_expire"]) * 1000
    alerts_today = alerts_30 = 0
    if table_exists(cur, "deliveries") and table_exists(cur, "alerts"):
        alerts_today = cur.execute(
            "SELECT COUNT(*) FROM deliveries d JOIN alerts a ON a.id=d.alert_id "
            "WHERE d.chat_id=? AND a.created_at>=?",
            (chat, now() - 86400),
        ).fetchone()[0]
        alerts_30 = cur.execute(
            "SELECT COUNT(*) FROM deliveries d JOIN alerts a ON a.id=d.alert_id "
            "WHERE d.chat_id=? AND a.created_at>=?",
            (chat, now() - 30 * 86400),
        ).fetchone()[0]
    return {
        "plan": plan,
        "limit": 50 if plan == "premium" else 1,
        "threshold": thr,
        "lang": lang,
        "alertsToday": int(alerts_today or 0),
        "alerts30d": int(alerts_30 or 0),
        "premUntil": prem_until,
        "updatedKey": "justNow",
    }


def load_wallets(cur: sqlite3.Connection, chat: str) -> list[dict]:
    if not table_exists(cur, "user_whales"):
        return []
    uw_cols = cols(cur, "user_whales")
    primary_sql = "uw.is_primary" if "is_primary" in uw_cols else "0"
    rows = cur.execute(
        f"SELECT wa.id AS id, wa.address AS addr, uw.label AS label, {primary_sql} AS is_primary "
        "FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id "
        "WHERE uw.user_id=? ORDER BY uw.is_primary DESC, uw.created_at ASC",
        (chat,),
    ).fetchall() if "is_primary" in uw_cols else cur.execute(
        "SELECT wa.id AS id, wa.address AS addr, uw.label AS label, 0 AS is_primary "
        "FROM user_whales uw JOIN whale_addresses wa ON wa.id=uw.whale_id "
        "WHERE uw.user_id=? ORDER BY uw.created_at ASC",
        (chat,),
    ).fetchall()
    wallets = []
    for i, r in enumerate(rows):
        addr = (r["addr"] or "").lower()
        name = (r["label"] or "").strip() or short_addr(addr)
        try:
            pos, equity, _ = parse_positions(addr)
        except Exception:
            pos, equity = [], {"total": 0.0, "spot": 0.0, "perp": 0.0, "hip3": 0.0, "vaults": 0.0}
        stats = wallet_stats(cur, addr)
        bal = equity.get("total") or stats["bal"] or 0
        d1 = (stats["net"] / bal * 100.0) if bal else 0.0
        wallets.append(
            {
                "id": f"w{r['id']}",
                "name": name,
                "addr": addr,
                "short": short_addr(addr),
                "primary": bool(r["is_primary"]),
                "score": stats["score"],
                "bal": bal,
                "trades": stats["tr"],
                "win": stats["win"],
                "pf": stats["pf"],
                "net": stats["net"],
                "dd": stats["dd"],
                "spot": stats["spot"],
                "perp": stats["perp"],
                "d1": d1,
                "equity": equity,
                "pos": pos,
            }
        )
    if wallets and not any(w["primary"] for w in wallets):
        wallets[0]["primary"] = True
    return wallets


def wallet_stats(cur: sqlite3.Connection, addr: str) -> dict:
    out = {"score": 50, "bal": 0, "tr": 0, "win": 0, "pf": 1, "net": 0, "dd": 0, "spot": "—", "perp": "—", "d1": 0}
    if table_exists(cur, "trades"):
        row = cur.execute(
            "SELECT COUNT(*) c, "
            "SUM(CASE WHEN is_buy=1 THEN usd_nanos ELSE 0 END) buy, "
            "SUM(CASE WHEN is_buy=0 THEN usd_nanos ELSE 0 END) sell "
            "FROM (SELECT is_buy, usd_nanos FROM trades WHERE wallet=? "
            "ORDER BY rowid DESC LIMIT 300)",
            (addr,),
        ).fetchone()
        buy, sell = usd(row["buy"] if row else 0), usd(row["sell"] if row else 0)
        out["d1"] = 0.0
        out["tr"] = int(row["c"] or 0) if row else 0
        out["net"] = buy - sell
    return out


def load_alerts(cur: sqlite3.Connection, chat: str, wallets: list) -> tuple[list, list]:
    names = {w["addr"]: w["name"] for w in wallets}
    alerts, feed = [], []
    if not (table_exists(cur, "alerts") and table_exists(cur, "deliveries")):
        return alerts, feed
    rows = cur.execute(
        "SELECT a.message, a.created_at FROM alerts a "
        "JOIN deliveries d ON d.alert_id=a.id "
        "WHERE d.chat_id=? AND a.created_at>=? "
        "ORDER BY a.created_at DESC LIMIT 40",
        (chat, now() - 2 * 86400),
    ).fetchall()
    for r in rows:
        parsed = parse_alert(r["message"], r["created_at"], "")
        alerts.append(parsed)
        feed.append(
            {
                "t": parsed["t"],
                "sym": parsed["sym"] if parsed["sym"] != "—" else "BTC",
                "w": parsed["name"] or "бот",
                "act": "купил" if parsed["side"] == "LONG" else "продаёт",
                "v": parsed["notional"],
                "up": parsed["side"] == "LONG",
            }
        )
    if wallets:
        for a in alerts:
            if not a["name"]:
                a["name"] = wallets[0]["name"]
    _ = names
    return alerts, feed


def load_flow(cur: sqlite3.Connection) -> dict:
    out = {}
    if not table_exists(cur, "trades"):
        return out
    ign = table_exists(cur, "ignored_wallets")
    windows = (("1", 3600), ("6", 21600), ("24", 86400), ("168", 604800), ("720", 2592000))
    tnow = now()
    ban = (
        "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw "
        "WHERE iw.wallet = t.wallet AND iw.permanent = 1) "
        if ign
        else ""
    )
    sql = (
        "SELECT t.token, "
        "SUM(CASE WHEN t.is_buy=1 THEN t.usd_nanos ELSE 0 END) buy, "
        "SUM(CASE WHEN t.is_buy=0 THEN t.usd_nanos ELSE 0 END) sell, "
        "COUNT(DISTINCT t.wallet) w "
        "FROM trades t "
        "WHERE t.timestamp >= ? AND t.usd_nanos > 0 AND t.usd_nanos <= ? "
        f"{ban}"
        "GROUP BY t.token "
        "ORDER BY ABS(SUM(CASE WHEN t.is_buy=1 THEN t.usd_nanos ELSE -t.usd_nanos END)) DESC "
        "LIMIT 80"
    )
    by_win = {}
    for key, sec in windows:
        rows = cur.execute(sql, (tnow - sec, MAX_SPOT_USD_NANOS)).fetchall()
        coins = []
        buy_t = sell_t = 0.0
        for r in rows:
            sym = symbol_of(cur, r["token"])
            if not sym:
                continue
            b, s = usd(r["buy"]), usd(r["sell"])
            buy_t += b
            sell_t += s
            coins.append(
                {
                    "sym": sym,
                    "token": r["token"],
                    "net": b - s,
                    "buy": b,
                    "sell": s,
                    "w": int(r["w"] or 0),
                    "top": min(0.95, (b / (b + s)) if (b + s) else 0.5),
                    "c1": 0,
                    "c6": 0,
                    "c24": 0,
                    "sp": spark(b - s),
                }
            )
        coins = coins[:20]
        by_win[key] = {
            "net": buy_t - sell_t,
            "coins": len(coins),
            "buy": buy_t,
            "sell": sell_t,
            "rows": coins,
        }
    chg = {}
    for key in ("1", "6", "24"):
        for r in by_win.get(key, {}).get("rows") or []:
            tot = r["buy"] + r["sell"]
            chg.setdefault(r["sym"], {})[key] = (100.0 * r["net"] / tot) if tot else 0.0
    for bkt in by_win.values():
        for r in bkt["rows"]:
            c = chg.get(r["sym"]) or {}
            r["c1"] = round(c.get("1") or 0, 1)
            r["c6"] = round(c.get("6") or 0, 1)
            r["c24"] = round(c.get("24") or 0, 1)
            pack = price_pack(cur, None, r["sym"], r.get("token") or "", http=False)
            if not pack:
                continue
            if pack.get("addr"):
                r["addr"] = pack["addr"]
            if pack.get("spark"):
                r["sp"] = pack["spark"]
            if pack.get("c1") or pack.get("c24"):
                r["c1"] = pack.get("c1") or r["c1"]
                r["c6"] = pack.get("c6") or r["c6"]
                r["c24"] = pack.get("c24") or r["c24"]
    return by_win


def _map_rank(arr, days: int, n: int = 100) -> list:
    mapped = []
    for e in arr[:n]:
        if not isinstance(e, dict):
            continue
        hold_s = int(e.get("h") or 0)
        mapped.append(
            {
                "a": e.get("w") or "",
                "pnl": usd(e.get("p")),
                "roi": float(e.get("r") or 0),
                "win": float(e.get("wr") or 0),
                "tr": int(e.get("t") or 0),
                "dd": 0,
                "days": days,
                "hold": f"{max(1, hold_s // 3600)}ч" if hold_s else None,
            }
        )
    return mapped


def _read_rank_key(cur, key: str, days: int) -> list:
    row = cur.execute("SELECT payload FROM ranking_cache WHERE cache_key=?", (key,)).fetchone()
    if not row or not row[0]:
        return []
    raw = row[0]
    if isinstance(raw, bytes):
        raw = raw.decode("utf-8", "ignore")
    try:
        arr = json.loads(raw) if isinstance(raw, str) and raw.lstrip().startswith("[") else []
    except json.JSONDecodeError:
        return []
    return _map_rank(arr, days, 100)


def load_perp_rank(hl: sqlite3.Connection | None, days: int = 30) -> dict:
    empty = {"pnl": [], "roi": [], "win": [], "act": []}
    if not hl or not table_exists(hl, "hl_fills"):
        return empty
    since_ms = (now() - days * 86400) * 1000
    max_tr = max(HL_MIN_CLOSED, (HL_MAX_CLOSED_30D * days + 29) // 30)
    cset = cols(hl, "hl_fills")
    if "closed_pnl_nanos" not in cset:
        return empty
    fee = "COALESCE(fee_nanos,0)" if "fee_nanos" in cset else "0"
    lev = "leverage" if "leverage" in cset else "0"
    ban = (
        "AND NOT EXISTS (SELECT 1 FROM hl_banned b WHERE b.wallet = f.wallet)"
        if table_exists(hl, "hl_banned")
        else ""
    )
    close_f = "AND (f.closed_pnl_nanos != 0"
    if "flat" in cset:
        close_f += " OR f.flat = 1"
    if "dir_code" in cset:
        close_f += " OR f.dir_code >= 5"
    close_f += ")"
    sql = (
        f"SELECT f.wallet, "
        f"SUM(f.closed_pnl_nanos) pnl, SUM({fee}) fees, COUNT(*) trades, "
        f"SUM(CASE WHEN f.closed_pnl_nanos > 0 THEN 1 ELSE 0 END) wins, "
        f"AVG(CASE WHEN {lev} > 0 THEN {lev} END) lev "
        f"FROM hl_fills f WHERE f.ts >= ? {close_f} {ban} "
        f"GROUP BY f.wallet HAVING COUNT(*) >= ? AND COUNT(*) <= ? "
        f"ORDER BY (SUM(f.closed_pnl_nanos) - SUM({fee})) DESC LIMIT 100"
    )
    try:
        rows = hl.execute(sql, (since_ms, HL_MIN_CLOSED, max_tr)).fetchall()
    except sqlite3.Error as e:
        sys.stderr.write(f"[api] perp rank: {e}\n")
        return empty
    mapped = []
    for r in rows:
        pnl = usd(r["pnl"]) - usd(r["fees"])
        tr = int(r["trades"] or 0)
        wins = int(r["wins"] or 0)
        lev_v = float(r["lev"] or 0)
        mapped.append(
            {
                "a": r["wallet"] or "",
                "pnl": pnl,
                "roi": round((100.0 * pnl / max(abs(pnl) * 0.4, 1.0)), 1) if pnl else 0,
                "win": int(round(100.0 * wins / tr)) if tr else 0,
                "tr": tr,
                "dd": 0,
                "days": days,
                "lev": int(round(lev_v)) if lev_v else None,
            }
        )
    return {
        "pnl": list(mapped),
        "roi": sorted(mapped, key=lambda x: -x["roi"]),
        "win": sorted(mapped, key=lambda x: -x["win"]),
        "act": sorted(mapped, key=lambda x: -x["tr"]),
    }


def load_rank(cur: sqlite3.Connection, hl: sqlite3.Connection | None = None) -> dict:
    empty = {"pnl": [], "roi": [], "win": [], "act": []}
    rank = {"spot": {k: [] for k in empty}, "perp": {k: [] for k in empty}, "wins": {}}
    kind_map = {"pnl": "pnl", "roi": "roi", "winrate": "win", "active": "act"}
    has_cache = table_exists(cur, "ranking_cache")
    for days in (30, 90, 180, 365):
        spot = {k: [] for k in empty}
        if has_cache:
            for kind, key in kind_map.items():
                rows = _read_rank_key(cur, f"global_{kind}_{days}", days)
                if not rows and days == 30:
                    rows = _read_rank_key(cur, f"global_{kind}", 30)
                spot[key] = rows
            if not spot["roi"]:
                spot["roi"] = list(spot["pnl"])
            if not spot["act"]:
                spot["act"] = list(spot["pnl"])
        perp = load_perp_rank(hl, days) if days == 30 else {k: [] for k in empty}
        rank["wins"][str(days)] = {"spot": spot, "perp": perp}
        if days == 30:
            rank["spot"] = spot
            rank["perp"] = perp
    return rank


def load_trades(cur: sqlite3.Connection, hl: sqlite3.Connection | None) -> dict:
    spot, perp, liq = [], [], []
    since = now() - 86400
    ign = table_exists(cur, "ignored_wallets")
    if table_exists(cur, "trades"):
        ban = (
            "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw "
            "WHERE iw.wallet = t.wallet AND iw.permanent = 1) "
            if ign
            else ""
        )
        rows = cur.execute(
            "SELECT t.wallet, t.token, t.is_buy, t.usd_nanos, t.timestamp "
            "FROM trades t "
            "WHERE t.timestamp >= ? AND t.usd_nanos > 0 AND t.usd_nanos <= ? "
            f"{ban}"
            "GROUP BY t.wallet "
            "HAVING t.usd_nanos = MAX(t.usd_nanos) "
            "ORDER BY t.usd_nanos DESC LIMIT 30",
            (since, MAX_SPOT_USD_NANOS),
        ).fetchall()
        for r in rows:
            sym = symbol_of(cur, r["token"])
            if not sym:
                continue
            spot.append(
                {
                    "sym": sym,
                    "v": usd(r["usd_nanos"]),
                    "side": "покупка" if r["is_buy"] else "продажа",
                    "w": short_addr(r["wallet"] or ""),
                    "t": ago(r["timestamp"]),
                }
            )
    if hl and table_exists(hl, "hl_fills"):
        since_ms = since * 1000
        ban = (
            "AND wallet NOT IN (SELECT wallet FROM hl_banned) "
            if table_exists(hl, "hl_banned")
            else ""
        )
        cset = cols(hl, "hl_fills")
        dirc = "dir_code" if "dir_code" in cset else "0"
        lev = "leverage" if "leverage" in cset else "0"
        q = (
            f"SELECT wallet, coin, dir, notional_nanos, {lev} lev, {dirc} dirc, ts "
            f"FROM hl_fills WHERE ts >= ? AND notional_nanos > 0 {ban} "
            f"AND {dirc} IN (1,2) "
            f"GROUP BY wallet HAVING notional_nanos = MAX(notional_nanos) "
            f"ORDER BY notional_nanos DESC LIMIT 30"
        )
        try:
            for r in hl.execute(q, (since_ms,)):
                code = int(r["dirc"] or 0)
                lv = int(r["lev"] or 0)
                side = f"{'лонг' if code == DIR_OPEN_LONG else 'шорт'} {lv}×" if lv else ("лонг" if code == 1 else "шорт")
                perp.append(
                    {
                        "sym": str(r["coin"] or "?").upper(),
                        "v": usd(r["notional_nanos"]),
                        "side": side,
                        "w": short_addr(r["wallet"] or ""),
                        "t": ago(ts_sec(r["ts"])),
                    }
                )
        except sqlite3.Error as e:
            sys.stderr.write(f"[api] perp trades: {e}\n")
        q2 = (
            f"SELECT wallet, coin, dir, notional_nanos, {dirc} dirc, ts "
            f"FROM hl_fills WHERE ts >= ? AND notional_nanos > 0 {ban} "
            f"AND {dirc} IN (6,7,8) "
            f"GROUP BY wallet HAVING notional_nanos = MAX(notional_nanos) "
            f"ORDER BY notional_nanos DESC LIMIT 30"
        )
        try:
            for r in hl.execute(q2, (since_ms,)):
                code = int(r["dirc"] or 0)
                side = "вынесло шорт" if code == DIR_LIQ_SHORT else "вынесло лонг"
                liq.append(
                    {
                        "sym": str(r["coin"] or "?").upper(),
                        "v": usd(r["notional_nanos"]),
                        "side": side,
                        "w": short_addr(r["wallet"] or ""),
                        "t": ago(ts_sec(r["ts"])),
                    }
                )
        except sqlite3.Error as e:
            sys.stderr.write(f"[api] liq trades: {e}\n")
    return {"spot": spot[:20], "perp": perp[:20], "liq": liq[:20]}


def load_feed_market(cur: sqlite3.Connection, hl: sqlite3.Connection | None) -> list:
    items = []
    ign = table_exists(cur, "ignored_wallets")
    if table_exists(cur, "trades"):
        ban = (
            "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw "
            "WHERE iw.wallet = t.wallet AND iw.permanent = 1) "
            if ign
            else ""
        )
        for r in cur.execute(
            "SELECT t.wallet, t.token, t.is_buy, t.usd_nanos, t.timestamp FROM trades t "
            f"WHERE t.usd_nanos > 0 AND t.usd_nanos <= ? {ban} "
            "ORDER BY t.timestamp DESC LIMIT 20",
            (MAX_SPOT_USD_NANOS,),
        ):
            sym = symbol_of(cur, r["token"])
            if not sym:
                continue
            items.append(
                {
                    "t": ago(r["timestamp"]),
                    "sym": sym,
                    "w": short_addr(r["wallet"] or ""),
                    "act": "купил" if r["is_buy"] else "продал",
                    "v": usd(r["usd_nanos"]),
                    "up": bool(r["is_buy"]),
                    "_ts": int(r["timestamp"] or 0),
                }
            )
    if hl and table_exists(hl, "hl_fills"):
        ban = (
            "AND wallet NOT IN (SELECT wallet FROM hl_banned) "
            if table_exists(hl, "hl_banned")
            else ""
        )
        cset = cols(hl, "hl_fills")
        dirc = "dir_code" if "dir_code" in cset else "0"
        for r in hl.execute(
            f"SELECT wallet, coin, dir, notional_nanos, ts, {dirc} dirc FROM hl_fills "
            f"WHERE notional_nanos > 0 {ban} ORDER BY ts DESC LIMIT 20"
        ):
            code = int(r["dirc"] or 0)
            up = code in (DIR_OPEN_LONG, 4, DIR_LIQ_SHORT)
            items.append(
                {
                    "t": ago(ts_sec(r["ts"])),
                    "sym": str(r["coin"] or "?").upper(),
                    "w": short_addr(r["wallet"] or ""),
                    "act": r["dir"] or "сделка",
                    "v": usd(r["notional_nanos"]),
                    "up": up,
                    "_ts": ts_sec(r["ts"]),
                }
            )
    items.sort(key=lambda x: -x.get("_ts", 0))
    for it in items:
        it.pop("_ts", None)
    return items[:24]


def load_funding(hl: sqlite3.Connection | None) -> list:
    if not hl or not table_exists(hl, "hl_funding_rate"):
        return []
    cset = cols(hl, "hl_funding_rate")
    oi_sel = "oi_nanos" if "oi_nanos" in cset else "0"
    try:
        rows = hl.execute(
            f"SELECT coin, rate_nanos, {oi_sel} oi FROM hl_funding_rate "
            "WHERE hour_ts=(SELECT MAX(hour_ts) FROM hl_funding_rate) "
            "ORDER BY ABS(rate_nanos) DESC LIMIT 15"
        ).fetchall()
    except sqlite3.Error:
        return []
    out = []
    for r in rows:
        rate = usd(r["rate_nanos"])
        out.append(
            {
                "sym": str(r["coin"] or "?").upper(),
                "rate": rate * 100,
                "apr": rate * 24 * 365 * 100,
                "oi": usd(r["oi"]),
                "side": "лонги платят" if rate >= 0 else "шорты платят",
            }
        )
    return out


def load_rot(cur: sqlite3.Connection) -> dict:
    rot = {"24": [], "168": []}
    if not table_exists(cur, "trades"):
        return rot
    ign = table_exists(cur, "ignored_wallets")
    ban = (
        "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw "
        "WHERE iw.wallet = t.wallet AND iw.permanent = 1) "
        if ign
        else ""
    )
    for key, sec in (("24", 86400), ("168", 604800)):
        rows = cur.execute(
            "SELECT t.wallet, t.token, t.is_buy, t.usd_nanos FROM trades t "
            f"WHERE t.timestamp >= ? AND t.usd_nanos > 0 AND t.usd_nanos <= ? {ban} "
            "ORDER BY t.wallet, t.timestamp",
            (now() - sec, MAX_SPOT_USD_NANOS),
        ).fetchall()
        last_sell: dict[str, str] = {}
        agg: dict[tuple[str, str], list] = {}
        for r in rows:
            w = (r["wallet"] or "").lower()
            tok = symbol_of(cur, r["token"])
            if not tok:
                continue
            if r["is_buy"]:
                src = last_sell.get(w)
                if src and src != tok:
                    slot = agg.setdefault((src, tok), [0.0, set()])
                    slot[0] += usd(r["usd_nanos"])
                    slot[1].add(w)
            else:
                last_sell[w] = tok
        links = [
            {"from": a, "to": b, "usd": v[0], "w": len(v[1])}
            for (a, b), v in agg.items()
            if v[0] > 0
        ]
        links.sort(key=lambda x: -x["usd"])
        rot[key] = links[:12]
    return rot


def _count_ready(cur: sqlite3.Connection, perp: bool) -> int:
    try:
        row = cur.execute(
            "SELECT COUNT(*) n FROM ai_events e "
            "WHERE filled_at>0 AND price_then>0 AND price_24h>0 "
            "AND window_days=24 AND venue=? AND buy_nanos!=sell_nanos "
            "AND ABS(price_24h-price_then)*50>=price_then "
            "AND NOT EXISTS ("
            "  SELECT 1 FROM ai_events e2 WHERE e2.token=e.token AND e2.venue=e.venue "
            "  AND e2.window_days=24 AND e2.filled_at>0 AND e2.price_then>0 AND e2.price_24h>0 "
            "  AND e2.ts/86400=e.ts/86400 AND e2.id<e.id)",
            (1 if perp else 0,),
        ).fetchone()
        return int(row["n"] if row else 0)
    except sqlite3.Error:
        return 0


def _ai_trained(cur: sqlite3.Connection, perp: bool) -> tuple[bool, float | None]:
    if not table_exists(cur, "ai_weights"):
        return False, None
    n_key, acc_key = (300, 302) if perp else (100, 102)
    w0, w1 = (500, 511) if perp else (400, 411)
    try:
        n = cur.execute("SELECT v FROM ai_weights WHERE k=?", (n_key,)).fetchone()
        acc = cur.execute("SELECT v FROM ai_weights WHERE k=?", (acc_key,)).fetchone()
        got = cur.execute(
            "SELECT COUNT(*) c FROM ai_weights WHERE k>=? AND k<?", (w0, w1)
        ).fetchone()
        ns = float(n["v"]) if n else 0
        trained = bool(got and got["c"] >= 11 and ns >= 400)
        a = float(acc["v"]) if acc and trained else None
        return trained, a
    except sqlite3.Error:
        return False, None


def _plan(px: float, conf: float, is_long: bool, is_perp: bool) -> dict:
    if px <= 0:
        px = 0.0
    stop_pct = 0.025
    stop_pct = max(0.015, min(0.15, stop_pct))
    stop = px * (1.0 - stop_pct) if is_long else px * (1.0 + stop_pct)
    t1 = px * (1.0 + stop_pct * 1.5) if is_long else px * (1.0 - stop_pct * 1.5)
    t2 = px * (1.0 + stop_pct * 3.0) if is_long else px * (1.0 - stop_pct * 3.0)
    confidence = max(0.0, min(1.0, (conf / 100.0 - 0.5) * 2.5))
    cap = 0.15 if is_perp else 0.06
    risk_budget = min(cap, 0.02 + 0.13 * confidence * 0.25)
    lev = risk_budget / stop_pct if stop_pct else 1
    lev = max(1, min(10, int(lev + 0.5)))
    return {
        "entry": px,
        "lo": px * 0.995,
        "hi": px * 1.005,
        "stop": stop,
        "stopPct": round(stop_pct * 100, 1),
        "t1": t1,
        "t2": t2,
        "risk": round(stop_pct * 100, 1),
        "lev": lev if is_perp else 1,
    }


def _heuristic(wallets: int, one_share: float, usd_vol: float, net: float, perp: bool) -> float:
    if wallets < 3 or one_share > 0.70 or usd_vol <= 0:
        return 0.0
    direction = net / usd_vol
    scaled = usd_vol / 10.0 if perp else usd_vol
    return direction * math.log1p(wallets) * (1.0 - one_share) * math.log1p(max(scaled, 0.0))


def _conf_from_score(score: float, trained: bool) -> int:
    a = abs(score)
    if trained:
        v = int(min(99, max(1, 50 + a * 8)))
    else:
        v = 40 + int(min(40.0, a * 5.0))
        v = max(35, min(80, v))
    return v


def _why(net: float, wallets: int, top_share: float, liq: float) -> list:
    out = []
    if net >= 0:
        out.append("flow")
        if wallets >= 8:
            out.append("volume")
        if top_share > 0.15:
            out.append("top100")
        if wallets >= 6:
            out.append("breadth")
    else:
        out.append("top100-out")
        if liq:
            out.append("liq-skew")
    return out or ["flow"]


def _take_sides(rows: list, n: int = 5) -> list:
    buys = [r for r in rows if (r.get("score") or 0) > 0]
    sells = [r for r in rows if (r.get("score") or 0) < 0]
    buys.sort(key=lambda x: -abs(x.get("score") or 0))
    sells.sort(key=lambda x: -abs(x.get("score") or 0))
    return buys[:n] + sells[:n]


def _price_of(cur, hl, key: str, perp: bool) -> float:
    try:
        if perp and hl:
            if table_exists(hl, "hl_mids"):
                row = hl.execute("SELECT px FROM hl_mids WHERE coin=?", (key,)).fetchone()
                if row and row[0]:
                    return float(row[0])
            if table_exists(hl, "hl_marks"):
                cset = cols(hl, "hl_marks")
                col = "px" if "px" in cset else ("mark" if "mark" in cset else "")
                if col:
                    row = hl.execute(f"SELECT {col} FROM hl_marks WHERE coin=?", (key,)).fetchone()
                    if row and row[0]:
                        return float(row[0])
            return 0.0
        if table_exists(cur, "token_prices"):
            cset = cols(cur, "token_prices")
            col = "price_nanos" if "price_nanos" in cset else ("price" if "price" in cset else "")
            if col:
                row = cur.execute(
                    f"SELECT {col} FROM token_prices WHERE lower(address)=?", (key.lower(),)
                ).fetchone()
                if row and row[0]:
                    v = float(row[0])
                    return usd(v) if col == "price_nanos" or v > 1e6 else v
        if table_exists(cur, "token_cache"):
            cset = cols(cur, "token_cache")
            for col in ("price_nanos", "usd_price", "price"):
                if col not in cset:
                    continue
                row = cur.execute(
                    f"SELECT {col} FROM token_cache WHERE lower(address)=?", (key.lower(),)
                ).fetchone()
                if row and row[0]:
                    v = float(row[0])
                    return usd(v) if "nanos" in col or v > 1e9 else v
    except (sqlite3.Error, TypeError, ValueError):
        return 0.0
    return 0.0


def _slot_put(bucket: dict, key: str, buy: float, sell: float, wallet: str, liq: float = 0.0):
    if buy <= 0 and sell <= 0 and liq <= 0:
        return
    slot = bucket.setdefault(key, [0.0, 0.0, set(), {}, 0.0, 0.0])
    slot[0] += buy
    slot[1] += sell
    vol = buy + sell
    if wallet and vol > 0:
        slot[2].add(wallet)
        slot[3][wallet] = slot[3].get(wallet, 0.0) + vol
    slot[4] += vol
    slot[5] += liq


def _signals_from_slots(slots: dict, hours: int, trained: bool, perp: bool, cur, hl) -> list:
    raw = []
    for key, (b, s, ws, per, tot, liq) in slots.items():
        wallets = len(ws)
        one = max(per.values()) / tot if tot and per else 0.0
        net = b - s
        sc = _heuristic(wallets, one, tot, net, perp)
        if sc == 0:
            continue
        if perp:
            sym = str(key or "").upper()
        else:
            sym = symbol_of(cur, key)
        if not sym:
            continue
        conf = _conf_from_score(sc, trained)
        side = "buy" if net >= 0 else "sell"
        px = _price_of(cur, hl, key, perp)
        plan = _plan(px, conf, side == "buy", perp)
        raw.append(
            {
                "sym": sym,
                "side": side,
                "conf": conf,
                "net": net,
                "w": wallets,
                **plan,
                "why": _why(net, wallets, 0, liq),
                "winH": hours,
                "venue": "perp" if perp else "spot",
                "score": sc,
            }
        )
    out = _take_sides(raw, 5)
    for s in out:
        s.pop("score", None)
    return out


def _spot_windows(cur: sqlite3.Connection) -> dict[int, dict]:
    wins: dict[int, dict] = {1: {}, 6: {}, 24: {}}
    if not table_exists(cur, "trades"):
        return wins
    tnow = now()
    t1, t6, t24 = tnow - 3600, tnow - 21600, tnow - 86400
    ign = table_exists(cur, "ignored_wallets")
    ban = (
        "AND NOT EXISTS (SELECT 1 FROM ignored_wallets iw "
        "WHERE iw.wallet = t.wallet AND iw.permanent = 1) "
        if ign
        else ""
    )
    try:
        rows = cur.execute(
            "SELECT t.token, lower(t.wallet) w, "
            "SUM(CASE WHEN t.timestamp>=? AND t.is_buy=1 THEN t.usd_nanos ELSE 0 END) b1, "
            "SUM(CASE WHEN t.timestamp>=? AND t.is_buy=0 THEN t.usd_nanos ELSE 0 END) s1, "
            "SUM(CASE WHEN t.timestamp>=? AND t.is_buy=1 THEN t.usd_nanos ELSE 0 END) b6, "
            "SUM(CASE WHEN t.timestamp>=? AND t.is_buy=0 THEN t.usd_nanos ELSE 0 END) s6, "
            "SUM(CASE WHEN t.is_buy=1 THEN t.usd_nanos ELSE 0 END) b24, "
            "SUM(CASE WHEN t.is_buy=0 THEN t.usd_nanos ELSE 0 END) s24 "
            "FROM trades t "
            "WHERE t.timestamp>=? AND t.usd_nanos>0 AND t.usd_nanos<=? "
            f"{ban}"
            "GROUP BY t.token, lower(t.wallet)",
            (t1, t1, t6, t6, t24, MAX_SPOT_USD_NANOS),
        ).fetchall()
    except sqlite3.Error as e:
        sys.stderr.write(f"[api] sonar spot: {e}\n")
        return wins
    for r in rows:
        tok = r["token"] or ""
        w = r["w"] or ""
        if not tok:
            continue
        _slot_put(wins[1], tok, usd(r["b1"]), usd(r["s1"]), w)
        _slot_put(wins[6], tok, usd(r["b6"]), usd(r["s6"]), w)
        _slot_put(wins[24], tok, usd(r["b24"]), usd(r["s24"]), w)
    return wins


def _perp_windows(hl: sqlite3.Connection | None) -> dict[int, dict]:
    wins: dict[int, dict] = {1: {}, 6: {}, 24: {}}
    if not hl or not table_exists(hl, "hl_fills"):
        return wins
    tnow = now()
    t1, t6, t24 = (tnow - 3600) * 1000, (tnow - 21600) * 1000, (tnow - 86400) * 1000
    ban = (
        "AND lower(wallet) NOT IN (SELECT lower(wallet) FROM hl_banned) "
        if table_exists(hl, "hl_banned")
        else ""
    )
    cset = cols(hl, "hl_fills")
    dirc = "dir_code" if "dir_code" in cset else "0"
    try:
        rows = hl.execute(
            f"SELECT coin, lower(wallet) w, {dirc} dirc, "
            "SUM(CASE WHEN ts>=? THEN notional_nanos ELSE 0 END) v1, "
            "SUM(CASE WHEN ts>=? THEN notional_nanos ELSE 0 END) v6, "
            "SUM(notional_nanos) v24 "
            "FROM hl_fills "
            f"WHERE ts>=? AND notional_nanos>0 AND {dirc} IN (1,2,6,7,8) {ban}"
            f"GROUP BY coin, lower(wallet), {dirc}",
            (t1, t6, t24),
        ).fetchall()
    except sqlite3.Error as e:
        sys.stderr.write(f"[api] sonar perp: {e}\n")
        return wins
    for r in rows:
        coin = str(r["coin"] or "").upper()
        w = r["w"] or ""
        if not coin:
            continue
        code = int(r["dirc"] or 0)
        for hours, col in ((1, "v1"), (6, "v6"), (24, "v24")):
            v = usd(r[col])
            if v <= 0:
                continue
            if code in (DIR_LIQ_LONG, DIR_LIQ_SHORT, DIR_LIQ_OTHER):
                _slot_put(wins[hours], coin, 0.0, 0.0, w, v)
            elif code == DIR_OPEN_LONG:
                _slot_put(wins[hours], coin, v, 0.0, w)
            else:
                _slot_put(wins[hours], coin, 0.0, v, w)
    return wins


def load_sonar(cur: sqlite3.Connection, hl: sqlite3.Connection | None = None) -> dict:
    sonar = {
        "need": 400,
        "ready": {"spot": 0, "perp": 0},
        "trained": False,
        "trainedSpot": False,
        "trainedPerp": False,
        "acc": None,
        "accSpot": None,
        "accPerp": None,
        "list": [],
        "hist": {"hit": 0, "of": 0, "won": 0, "tp": 0, "sl": 0, "missed": 0, "broken": 0, "avg": 0, "items": []},
    }
    if table_exists(cur, "ai_events"):
        sonar["ready"]["spot"] = _count_ready(cur, False)
        sonar["ready"]["perp"] = _count_ready(cur, True)
    ts, accs = _ai_trained(cur, False)
    tp, accp = _ai_trained(cur, True)
    sonar["trainedSpot"] = ts
    sonar["trainedPerp"] = tp
    sonar["trained"] = ts
    sonar["accSpot"] = round(accs * 100) if accs else None
    sonar["accPerp"] = round(accp * 100) if accp else None
    sonar["acc"] = sonar["accSpot"]

    signals = []
    spot_w = _spot_windows(cur)
    perp_w = _perp_windows(hl)
    for hours in (1, 6, 24):
        signals.extend(_signals_from_slots(spot_w.get(hours) or {}, hours, ts, False, cur, hl))
        signals.extend(_signals_from_slots(perp_w.get(hours) or {}, hours, tp, True, cur, hl))
    sonar["list"] = signals

    items = []
    if table_exists(cur, "ai_events"):
        try:
            hist_rows = cur.execute(
                "SELECT name, venue, buy_nanos, sell_nanos, price_then, price_24h, ts "
                "FROM ai_events WHERE window_days=24 AND filled_at>0 "
                "AND price_then>0 AND price_24h>0 ORDER BY ts DESC LIMIT 40"
            ).fetchall()
        except sqlite3.Error:
            hist_rows = []
        tp_n = sl_n = 0
        signed = []
        for r in hist_rows:
            buy, sell = usd(r["buy_nanos"]), usd(r["sell_nanos"])
            then, later = usd(r["price_then"]), usd(r["price_24h"])
            if buy == sell or then <= 0:
                continue
            was_long = buy > sell
            ret = 100.0 * (later - then) / then
            win = ret > 0 if was_long else ret < 0
            moved = ret if was_long else -ret
            plan = 1 if moved >= 3.75 else (-1 if moved <= -2.5 else 0)
            if plan == 1:
                tp_n += 1
            elif plan == -1:
                sl_n += 1
            signed.append(moved)
            items.append(
                {
                    "sym": (r["name"] or "?").upper(),
                    "long": was_long,
                    "ret": round(ret, 1),
                    "t": ago(int(r["ts"] or 0)),
                    "win": win,
                    "venue": "perp" if int(r["venue"] or 0) else "spot",
                }
            )
        of = len(items)
        won = sum(1 for i in items if i["win"])
        sonar["hist"] = {
            "hit": int(round(100.0 * won / of)) if of else 0,
            "of": of,
            "won": won,
            "tp": tp_n,
            "sl": sl_n,
            "missed": max(0, of - won),
            "broken": sl_n,
            "avg": round(sum(signed) / len(signed), 1) if signed else 0,
            "items": items,
        }
    return sonar


def load_coins(cur: sqlite3.Connection, hl: sqlite3.Connection | None, flow: dict, wallets: list) -> dict:
    coins = {}
    flow24 = {(r.get("sym") or ""): r for r in ((flow.get("24") or {}).get("rows") or []) if r.get("sym")}
    rows = []
    for k in ("24", "6", "1", "168", "720"):
        rows.extend((flow.get(k) or {}).get("rows") or [])
    who_by: dict[str, list] = {}
    extra_sym = []
    entry_w: dict[str, list[tuple[float, float]]] = {}
    for w in wallets or []:
        for p in w.get("pos") or []:
            sym = p.get("sym") or ""
            if not sym:
                continue
            who_by.setdefault(sym, []).append({"w": w.get("name") or "", "v": p.get("size") or 0, "t": "сейчас"})
            extra_sym.append({"sym": sym, "net": 0, "buy": 0, "sell": 0, "w": 0, "token": "", "c24": 0, "sp": []})
            try:
                ent = float(p.get("entry") or 0)
                sz = abs(float(p.get("size") or 0))
            except (TypeError, ValueError):
                continue
            if ent > 0 and sz > 0:
                entry_w.setdefault(sym, []).append((ent, sz))
    seen = set()
    need_http: list[str] = []
    ordered = rows + extra_sym
    for r in ordered:
        sym = r.get("sym") or ""
        if not sym or sym in seen:
            continue
        seen.add(sym)
        r24 = flow24.get(sym) or r
        pack = price_pack(cur, hl, sym, r24.get("token") or r.get("token") or r.get("addr") or "", http=True)
        if not pack.get("hists") or not _px_ok(sym, pack.get("price") or 0):
            need_http.append(sym)
        hist24 = (pack.get("hists") or {}).get("24h") or pack.get("spark") or r.get("sp") or spark(r24.get("net") or 0)
        ww = int(r24.get("w") or r.get("w") or 0)
        wsum = 0.0
        wtot = 0.0
        for ent, sz in entry_w.get(sym) or []:
            wsum += ent * sz
            wtot += sz
        entry = (wsum / wtot) if wtot else 0.0
        coins[sym] = {
            "price": pack.get("price") or 0,
            "chg": pack.get("chg") if pack else (r24.get("c24") or 0),
            "entry": entry,
            "hist": hist24,
            "hists": pack.get("hists") or {},
            "real": bool(pack.get("real")),
            "addr": pack.get("addr") or r.get("addr") or "",
            "icon": pack.get("icon") or coin_icon(sym, pack.get("addr") or r.get("addr") or ""),
            "spark": pack.get("spark") or hist24,
            "c1": pack.get("c1") or 0,
            "c6": pack.get("c6") or 0,
            "c24": pack.get("c24") or pack.get("chg") or (r24.get("c24") or 0),
            "net": r24.get("net") or 0,
            "buy": r24.get("buy") or 0,
            "sell": r24.get("sell") or 0,
            "w": ww,
            "mcap": "—",
            "liq": "—",
            "who": who_by.get(sym, []),
            "cons": {"top": min(24, ww), "of": max(ww, 24), "first": "—", "also": []},
        }
    if need_http:
        lock = threading.Lock()

        def pull(sym: str):
            pts = hist_perp(sym)
            if len(pts) < 3:
                return
            sl = _sparkify(_slices(pts), sym)
            if not sl:
                return
            with lock:
                c = coins.get(sym)
                if not c:
                    return
                if _px_ok(sym, sl.get("price") or 0):
                    c["price"] = sl["price"]
                c["chg"] = sl["chg"]
                c["hist"] = (sl.get("hists") or {}).get("24h") or sl.get("spark") or c["hist"]
                c["hists"] = sl.get("hists") or {}
                c["spark"] = sl.get("spark") or c.get("spark")
                c["c1"] = sl.get("c1") or c.get("c1") or 0
                c["c6"] = sl.get("c6") or c.get("c6") or 0
                c["c24"] = sl.get("c24") or c.get("c24") or 0
                c["real"] = True
                c["icon"] = c.get("icon") or coin_icon(sym)

        th = []
        for sym in need_http[:24]:
            t = threading.Thread(target=pull, args=(sym,), daemon=True)
            t.start()
            th.append(t)
        for t in th:
            t.join(timeout=6.0)
    mids = hl_mids()
    for sym, c in coins.items():
        mid = mids.get(sym.upper())
        if mid and (not _px_ok(sym, c.get("price") or 0)):
            c["price"] = mid
        elif mid and not c.get("price"):
            c["price"] = mid
    return coins


def build_public(cur: sqlite3.Connection, hl: sqlite3.Connection | None) -> dict:
    t0 = time.monotonic()
    flow, rank, trades, market_feed, funding, rot, sonar = {}, {}, {"spot": [], "perp": [], "liq": []}, [], [], {"24": [], "168": []}, {
        "need": 400, "ready": {"spot": 0, "perp": 0}, "trained": False, "acc": None,
        "list": [], "hist": {"hit": 0, "of": 0, "won": 0, "tp": 0, "sl": 0, "missed": 0, "broken": 0, "avg": 0, "items": []},
    }

    def take(name, fn, fallback, must=False):
        if not must and time.monotonic() - t0 > 8.0:
            sys.stderr.write(f"[api] cache skip {name}\n")
            return fallback
        try:
            return fn()
        except Exception as e:
            sys.stderr.write(f"[api] cache {name}: {e}\n")
            return fallback

    rank = take("rank", lambda: load_rank(cur, hl), rank, True)
    flow = take("flow", lambda: load_flow(cur), flow, True)
    sonar = take("sonar", lambda: load_sonar(cur, hl), sonar, True)
    trades = take("trades", lambda: load_trades(cur, hl), trades, True)
    market_feed = take("feed", lambda: load_feed_market(cur, hl), market_feed, True)
    funding = take("funding", lambda: load_funding(hl), funding)
    rot = take("rot", lambda: load_rot(cur), rot)
    coins = take("coins", lambda: load_coins(cur, hl, flow, []), {})
    return {
        "flow": flow,
        "rank": rank,
        "trades": trades,
        "marketFeed": market_feed,
        "funding": funding,
        "rot": rot,
        "sonar": sonar,
        "coins": coins,
        "cachedAt": now(),
    }


def get_public(cur: sqlite3.Connection, hl: sqlite3.Connection | None) -> dict:
    global _building
    with _pub_lock:
        data, ts = _pub["data"], _pub["t"]
        building = _building
    if data is not None and (time.monotonic() - ts) < PUB_TTL:
        return data
    if data is not None:
        if not building:
            threading.Thread(target=_bg_refresh, daemon=True).start()
        return data
    started = False
    with _pub_lock:
        if not _building:
            _building = True
            started = True
    if not started:
        t_end = time.monotonic() + 12.0
        while time.monotonic() < t_end:
            time.sleep(0.05)
            with _pub_lock:
                if _pub["data"] is not None:
                    return _pub["data"]
        return {}
    try:
        data = build_public(cur, hl)
        with _pub_lock:
            _pub["data"] = data
            _pub["t"] = time.monotonic()
        return data
    except Exception as e:
        sys.stderr.write(f"[api] build_public: {e}\n")
        return {}
    finally:
        with _pub_lock:
            _building = False


def _bg_refresh() -> None:
    global _building
    with _pub_lock:
        if _building:
            return
        _building = True
    cur = open_db(DB)
    hl = open_db(HL_DB)
    if not cur:
        with _pub_lock:
            _building = False
        return
    try:
        data = build_public(cur, hl)
        with _pub_lock:
            _pub["data"] = data
            _pub["t"] = time.monotonic()
        sys.stderr.write("[api] public cache refreshed\n")
    except Exception as e:
        sys.stderr.write(f"[api] cache refresh: {e}\n")
    finally:
        with _pub_lock:
            _building = False
        try:
            cur.close()
        except Exception:
            pass
        if hl:
            try:
                hl.close()
            except Exception:
                pass


def warmup() -> None:
    time.sleep(0.3)
    cur = open_db(DB)
    hl = open_db(HL_DB)
    if not cur:
        return
    try:
        get_public(cur, hl)
        sys.stderr.write("[api] public cache ready\n")
    except Exception as e:
        sys.stderr.write(f"[api] warmup: {e}\n")
    finally:
        try:
            cur.close()
        except Exception:
            pass
        if hl:
            try:
                hl.close()
            except Exception:
                pass


def bootstrap(chat: str) -> dict:
    cur = open_db(DB)
    hl = open_db(HL_DB)
    if not cur:
        return {"ok": False, "live": False, "error": "db_missing", "db": DB}
    errors: list[str] = []
    t0 = time.monotonic()

    def piece(name: str, fn, fallback, must: bool = False):
        if not must and time.monotonic() - t0 > 18.0:
            errors.append(f"{name}:skip")
            return fallback
        try:
            return fn()
        except Exception as e:
            errors.append(f"{name}:{type(e).__name__}:{e}")
            sys.stderr.write(f"[api] bootstrap {name}: {e}\n")
            return fallback

    try:
        empty_me = {
            "plan": "free", "limit": 1, "threshold": 10000, "lang": "ru",
            "alertsToday": 0, "alerts30d": 0, "premUntil": 0, "updatedKey": "justNow",
        }
        empty_rank = {"spot": {"pnl": [], "roi": [], "win": [], "act": []}, "perp": {"pnl": [], "roi": [], "win": [], "act": []}}
        empty_sonar = {
            "need": 400, "ready": {"spot": 0, "perp": 0},
            "trained": False, "trainedSpot": False, "trainedPerp": False,
            "acc": None, "accSpot": None, "accPerp": None,
            "list": [], "hist": {"hit": 0, "of": 0, "won": 0, "tp": 0, "sl": 0, "missed": 0, "broken": 0, "avg": 0, "items": []},
        }
        pub = piece("pub", lambda: get_public(cur, hl), {}, True) or {}
        me = piece("me", lambda: load_me(cur, chat) if chat else empty_me, empty_me)
        if isinstance(me, dict):
            me = dict(me)
            me.pop("lang", None)
        wallets = piece("wallets", lambda: load_wallets(cur, chat) if chat else [], [])
        alerts, feed = piece(
            "alerts",
            lambda: load_alerts(cur, chat, wallets) if chat else ([], []),
            ([], []),
        )
        flow = pub.get("flow") or {}
        rank_raw = pub.get("rank") or empty_rank
        rank = {
            "spot": (rank_raw.get("spot") if isinstance(rank_raw, dict) else None) or empty_rank["spot"],
            "perp": (rank_raw.get("perp") if isinstance(rank_raw, dict) else None) or empty_rank["perp"],
        }
        trades = pub.get("trades") or {"spot": [], "perp": [], "liq": []}
        market_feed = pub.get("marketFeed") or []
        funding = pub.get("funding") or []
        rot = pub.get("rot") or {"24": [], "168": []}
        sonar = pub.get("sonar") or empty_sonar
        coins = piece("coins", lambda: load_coins(cur, hl, flow, wallets), pub.get("coins") or {})
        out = {
            "ok": True,
            "live": True,
            "me": me,
            "wallets": wallets,
            "feed": feed,
            "alerts": alerts,
            "flow": flow,
            "rank": rank,
            "sonar": sonar,
            "trades": trades,
            "funding": funding,
            "rot": rot,
            "coins": coins,
            "marketFeed": market_feed,
        }
        if errors:
            out["partial"] = errors[:8]
        if pub.get("cachedAt"):
            out["cachedAt"] = pub["cachedAt"]
        return out
    except Exception as e:
        sys.stderr.write(f"[api] bootstrap fatal: {e}\n")
        return {"ok": False, "live": False, "error": str(e)}
    finally:
        try:
            cur.close()
        except Exception:
            pass
        if hl:
            try:
                hl.close()
            except Exception:
                pass


def mutate(chat: str, kind: str, body: dict) -> dict:
    if not chat:
        return {"ok": False, "error": "no_user"}
    con = open_db(DB, write=True)
    if not con:
        return {"ok": False, "error": "db_missing"}
    try:
        con.execute("INSERT OR IGNORE INTO users(chat_id, language, threshold_nanos, created_at) VALUES(?,?,?,?)",
                    (chat, "ru", 100000000000, now()))
        if kind == "add":
            addr = (body.get("addr") or "").strip().lower()
            name = (body.get("name") or "").strip()[:64] or short_addr(addr)
            if not ADDR_RE.match(addr):
                return {"ok": False, "error": "bad_addr"}
            # Бан кошелька в боте действует для всех, значит и здесь.
            # Раньше через мини-апп забаненного кита можно было вернуть.
            if wallet_banned(con, addr):
                return {"ok": False, "error": "banned"}
            n = con.execute("SELECT COUNT(*) FROM user_whales WHERE user_id=?", (chat,)).fetchone()[0]
            # Лимит подписки. Раньше он только СООБЩАЛСЯ фронту в load_me,
            # а на запись не проверялся — бесплатный аккаунт добавлял сколько
            # угодно кошельков в обход премиума.
            lim = wallet_limit(con, chat)
            if n >= lim:
                return {"ok": False, "error": "limit", "limit": lim}
            con.execute("INSERT OR IGNORE INTO whale_addresses(address) VALUES(?)", (addr,))
            wid = con.execute("SELECT id FROM whale_addresses WHERE address=?", (addr,)).fetchone()[0]
            exists = con.execute(
                "SELECT 1 FROM user_whales WHERE user_id=? AND whale_id=?", (chat, wid)
            ).fetchone()
            if exists:
                return {"ok": False, "error": "dup"}
            first = n == 0
            con.execute(
                "INSERT INTO user_whales(user_id,whale_id,label,created_at,is_primary) VALUES(?,?,?,?,?)",
                (chat, wid, name, now(), 1 if first else 0),
            )
        elif kind == "remove":
            addr = (body.get("addr") or "").strip().lower()
            row = con.execute("SELECT id FROM whale_addresses WHERE address=?", (addr,)).fetchone()
            if not row:
                return {"ok": False, "error": "missing"}
            con.execute("DELETE FROM user_whales WHERE user_id=? AND whale_id=?", (chat, row[0]))
        elif kind == "primary":
            addr = (body.get("addr") or "").strip().lower()
            row = con.execute("SELECT id FROM whale_addresses WHERE address=?", (addr,)).fetchone()
            if not row:
                return {"ok": False, "error": "missing"}
            con.execute("UPDATE user_whales SET is_primary=0 WHERE user_id=?", (chat,))
            con.execute(
                "UPDATE user_whales SET is_primary=1 WHERE user_id=? AND whale_id=?",
                (chat, row[0]),
            )
        elif kind == "rename":
            addr = (body.get("addr") or "").strip().lower()
            name = (body.get("name") or "").strip()[:64]
            row = con.execute("SELECT id FROM whale_addresses WHERE address=?", (addr,)).fetchone()
            if not row or not name:
                return {"ok": False, "error": "missing"}
            con.execute(
                "UPDATE user_whales SET label=? WHERE user_id=? AND whale_id=?",
                (name, chat, row[0]),
            )
        elif kind == "threshold":
            try:
                usd_v = float(body.get("usd") or 0)
            except (TypeError, ValueError):
                return {"ok": False, "error": "bad_value"}
            # NaN и бесконечность проходили сравнение `< 50` и падали уже
            # на int(); заодно ставим верхнюю границу бота ($1B).
            if not math.isfinite(usd_v):
                return {"ok": False, "error": "bad_value"}
            if usd_v < MIN_THRESHOLD_USD:
                return {"ok": False, "error": "min"}
            if usd_v > MAX_THRESHOLD_USD:
                return {"ok": False, "error": "max"}
            con.execute(
                "UPDATE users SET threshold_nanos=? WHERE chat_id=?",
                (int(usd_v * NANOS), chat),
            )
        else:
            return {"ok": False, "error": "unknown"}
        con.commit()
        return {"ok": True}
    except Exception as e:
        try:
            con.rollback()
        except Exception:
            pass
        return {"ok": False, "error": str(e)}
    finally:
        con.close()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[api] " + (fmt % args) + "\n")

    def _cors(self):
        # Только известные адреса. Раньше стояла звёздочка, и любая страница
        # в интернете могла читать и менять данные из браузера жертвы.
        origin = (self.headers.get("Origin") or "").rstrip("/")
        if origin and origin in ALLOWED_ORIGINS:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
            self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Telegram-Init-Data")
            self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
            self.send_header("Access-Control-Max-Age", "600")

    def _json(self, code: int, obj: dict):
        def clean(o):
            if isinstance(o, float):
                if o != o or o in (float("inf"), float("-inf")):
                    return 0.0
                return o
            if isinstance(o, dict):
                return {str(k): clean(v) for k, v in o.items()}
            if isinstance(o, (list, tuple)):
                return [clean(v) for v in o]
            if isinstance(o, (str, int, bool)) or o is None:
                return o
            return str(o)

        try:
            raw = json.dumps(clean(obj), ensure_ascii=False, allow_nan=False).encode()
        except Exception as e:
            raw = json.dumps({"ok": False, "error": f"json:{e}"}).encode()
            code = 500
        self.send_response(code)
        self._cors()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def _user(self, qs: dict) -> str:
        """Возвращает chat_id только по сошедшейся подписи, иначе пустую
        строку. Прежний хвост `return qs["tg"]` пускал в чужой аккаунт по
        одному номеру в адресе запроса."""
        init = self.headers.get("X-Telegram-Init-Data") or qs.get("init", [""])[0]
        parsed = verify_init_data(init)
        return parsed["id"] if parsed else ""

    def _keyed(self) -> bool:
        if not API_KEY:
            return True
        return hmac.compare_digest(self.headers.get("X-Api-Key") or "", API_KEY)

    def _peer(self) -> str:
        # За nginx реальный адрес приходит заголовком; своему прокси верим.
        fwd = (self.headers.get("X-Forwarded-For") or "").split(",")[0].strip()
        return fwd or self.client_address[0]

    def _body(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        raw = self.rfile.read(n)
        try:
            return json.loads(raw.decode() or "{}")
        except json.JSONDecodeError:
            return {}

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        try:
            u = urlparse(self.path)
            qs = parse_qs(u.query)
            path = u.path.rstrip("/") or "/"
            if path not in ("/health", "/api/health"):
                if not self._keyed():
                    self._json(403, {"ok": False, "error": "forbidden"})
                    return
                if not _limiter.allow(self._peer()):
                    self._json(429, {"ok": False, "error": "rate_limit"})
                    return
            if path in ("/health", "/api/health"):
                # Абсолютные пути к базам отсюда убраны: проверка живости не
                # обязана рассказывать наружу устройство файловой системы.
                self._json(200, {
                    "ok": True,
                    "db": os.path.isfile(DB),
                    "hl": os.path.isfile(HL_DB),
                })
                return
            if path in ("/bootstrap", "/api/bootstrap", "/api/me"):
                chat = self._user(qs)
                self._json(200, bootstrap(chat))
                return
            if path in ("/market", "/api/market"):
                cur = open_db(DB)
                hl = open_db(HL_DB)
                if not cur:
                    self._json(200, {"ok": False, "live": False})
                    return
                try:
                    pub = get_public(cur, hl) or {}
                    rank_raw = pub.get("rank") or {}
                    self._json(200, {
                        "ok": True,
                        "live": True,
                        "flow": pub.get("flow") or {},
                        "rank": {
                            "spot": rank_raw.get("spot") or {"pnl": [], "roi": [], "win": [], "act": []},
                            "perp": rank_raw.get("perp") or {"pnl": [], "roi": [], "win": [], "act": []},
                        },
                        "trades": pub.get("trades") or {"spot": [], "perp": [], "liq": []},
                        "marketFeed": pub.get("marketFeed") or [],
                        "funding": pub.get("funding") or [],
                        "rot": pub.get("rot") or {"24": [], "168": []},
                        "sonar": pub.get("sonar") or {},
                        "coins": pub.get("coins") or {},
                    })
                finally:
                    try:
                        cur.close()
                    except Exception:
                        pass
                    if hl:
                        try:
                            hl.close()
                        except Exception:
                            pass
                return
            if path in ("/quotes", "/api/quotes"):
                cur = open_db(DB)
                hl = open_db(HL_DB)
                if not cur:
                    self._json(200, {})
                    return
                try:
                    pub = get_public(cur, hl) or {}
                    coins = pub.get("coins") or {}
                    out = {}
                    for sym, c in coins.items():
                        if not isinstance(c, dict):
                            continue
                        out[sym] = {
                            "price": c.get("price") or 0,
                            "chg": c.get("chg") or 0,
                            "c1": c.get("c1") or 0,
                            "c6": c.get("c6") or 0,
                            "c24": c.get("chg") or 0,
                            "hist": c.get("hist") or [],
                            "hists": c.get("hists") or {},
                            "ohlc": c.get("ohlc") or {},
                            "spark": c.get("spark") or c.get("hist") or [],
                            "icon": c.get("icon") or "",
                            "real": True,
                        }
                    self._json(200, out)
                finally:
                    try:
                        cur.close()
                    except Exception:
                        pass
                    if hl:
                        try:
                            hl.close()
                        except Exception:
                            pass
                return
            self._json(404, {"ok": False, "error": "not_found"})
        except Exception as e:
            sys.stderr.write(f"[api] GET fail: {e}\n")
            try:
                self._json(500, {"ok": False, "error": str(e)})
            except Exception:
                pass

    def do_POST(self):
        try:
            u = urlparse(self.path)
            qs = parse_qs(u.query)
            path = u.path.rstrip("/") or "/"
            if not self._keyed():
                self._body()
                self._json(403, {"ok": False, "error": "forbidden"})
                return
            if not _limiter.allow(self._peer()):
                self._body()
                self._json(429, {"ok": False, "error": "rate_limit"})
                return
            chat = self._user(qs)
            # Тело читаем всегда: иначе непрочитанные байты остаются в сокете
            # и портят следующий запрос на том же keep-alive соединении.
            body = self._body()
            if not chat:
                self._json(401, {"ok": False, "error": "unauthorized"})
                return
            kind = {
                "/api/wallets": "add",
                "/api/wallets/remove": "remove",
                "/api/wallets/primary": "primary",
                "/api/wallets/rename": "rename",
                "/api/threshold": "threshold",
            }.get(path)
            if not kind:
                self._json(404, {"ok": False, "error": "not_found"})
                return
            res = mutate(chat, kind, body)
            if res.get("ok"):
                res = {**res, **bootstrap(chat)}
            self._json(200 if res.get("ok") else 400, res)
        except Exception as e:
            sys.stderr.write(f"[api] POST fail: {e}\n")
            try:
                self._json(500, {"ok": False, "error": str(e)})
            except Exception:
                pass


def main():
    # Без токена подпись не проверяется, а значит любой запрос анонимен и
    # мини-апп бесполезен. Раньше служба в этом случае тихо поднималась и
    # пускала всех.
    if not BOT_TOKEN:
        sys.stderr.write("[api] WHALE_TG_TOKEN не задан — запуск отменён\n")
        raise SystemExit(1)
    if not API_KEY:
        sys.stderr.write("[api] WHALE_API_KEY не задан: порт принимает запросы в обход nginx\n")
    if not ALLOWED_ORIGINS:
        sys.stderr.write("[api] WHALE_API_ORIGIN пуст: обращения из браузера напрямую будут отклонены\n")
    print(
        f"[api] ws={WS_DIR} db={DB} db_ok={os.path.isfile(DB)} "
        f"hl={HL_DB} hl_ok={os.path.isfile(HL_DB)} listen={HOST}:{PORT} "
        f"origins={len(ALLOWED_ORIGINS)} rpm={RATE_LIMIT_RPM} key={'yes' if API_KEY else 'no'}",
        flush=True,
    )
    threading.Thread(target=warmup, daemon=True).start()
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
