#!/usr/bin/env python3
"""Скачивает логотипы монет один раз, чтобы мини-апп не ходил за ними наружу.

    python3 -u fetch_logos.py

Кладёт:
    ./coins/bsc/<checksum>.png   спотовые токены BSC
    ./coins/hl/<имя>.svg         перпы Hyperliquid, включая акции
    ./coins_manifest.json        что реально лежит на диске

Папка coins/ едет в репозиторий как html/coins/, манифест остаётся рядом
с whale_api.py на ВМ. Повторный запуск не перекачивает уже скачанное,
прервать можно в любой момент.
"""
import json, os, sqlite3, sys, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.environ.get("WHALE_DB", os.path.expanduser("~/WhaleScanner/whale_bot.db"))
OUT = os.environ.get("LOGO_DIR", os.path.join(HERE, "coins"))
MANIFEST = os.path.join(HERE, "coins_manifest.json")
PAUSE = float(os.environ.get("LOGO_PAUSE", "0.05"))
UA = {"User-Agent": "Mozilla/5.0 (WhaleScanner logo fetcher)"}
HL_INFO = "https://api.hyperliquid.xyz/info"

# --- EIP-55: оба источника отдают логотипы только по адресу в смешанном
# --- регистре, а в token_cache адреса лежат в нижнем.
_KEC_RC = [0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008]
_KEC_R = [[0,36,3,41,18],[1,44,10,45,2],[62,6,43,15,61],[28,55,25,21,56],[27,20,39,8,14]]
_M = (1 << 64) - 1


def _rol(x, n):
    return ((x << n) | (x >> (64 - n))) & _M


def _keccak_f(A):
    for rnd in range(24):
        C = [A[x][0] ^ A[x][1] ^ A[x][2] ^ A[x][3] ^ A[x][4] for x in range(5)]
        D = [C[(x - 1) % 5] ^ _rol(C[(x + 1) % 5], 1) for x in range(5)]
        for x in range(5):
            for y in range(5):
                A[x][y] ^= D[x]
        B = [[0] * 5 for _ in range(5)]
        for x in range(5):
            for y in range(5):
                B[y][(2 * x + 3 * y) % 5] = _rol(A[x][y], _KEC_R[x][y])
        for x in range(5):
            for y in range(5):
                A[x][y] = B[x][y] ^ ((~B[(x + 1) % 5][y]) & B[(x + 2) % 5][y]) & _M
        A[0][0] ^= _KEC_RC[rnd]
    return A


def keccak256(data: bytes) -> bytes:
    rate = 136
    A = [[0] * 5 for _ in range(5)]
    pad = bytearray(data + b"\x01" + b"\x00" * ((-len(data) - 1) % rate))
    pad[-1] ^= 0x80
    for off in range(0, len(pad), rate):
        blk = pad[off:off + rate]
        for i in range(rate // 8):
            A[i % 5][i // 5] ^= int.from_bytes(blk[i * 8:i * 8 + 8], "little")
        _keccak_f(A)
    return b"".join(A[i % 5][i // 5].to_bytes(8, "little") for i in range(4))[:32]


def to_checksum(addr: str) -> str:
    a = (addr or "").lower().replace("0x", "")
    if len(a) != 40:
        return addr or ""
    h = keccak256(a.encode()).hex()
    return "0x" + "".join(c.upper() if c.isalpha() and int(h[i], 16) >= 8 else c
                          for i, c in enumerate(a))


class Source:
    """Один источник картинок со своей статистикой и паузой при 429.

    Раньше ошибки глотались молча: стоило источнику начать придерживать
    запросы, и скрипт перемалывал тысячи токенов вхолостую, а по логу это
    было не отличить от «логотипа просто нет».
    """

    def __init__(self, name: str):
        self.name = name
        self.ok = self.missing = self.errors = self.throttled = 0
        self.cool_until = 0.0

    def fetch(self, url: str, tries: int = 3) -> bytes | None:
        for attempt in range(tries):
            wait = self.cool_until - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            try:
                req = urllib.request.Request(url, headers=UA)
                with urllib.request.urlopen(req, timeout=15) as r:
                    data = r.read()
                # ответ меньше 200 байт картинкой не бывает — это заглушка
                if len(data) > 200:
                    self.ok += 1
                    return data
                self.missing += 1
                return None
            except urllib.error.HTTPError as e:
                if e.code in (403, 429) or e.code >= 500:
                    self.throttled += 1
                    back = min(60.0, 2.0 * (2 ** attempt))
                    self.cool_until = time.monotonic() + back
                    print(f"    {self.name}: код {e.code}, пауза {back:.0f}с",
                          flush=True)
                    continue
                self.missing += 1
                return None
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                self.errors += 1
                if attempt == tries - 1:
                    return None
                time.sleep(1.0 * (attempt + 1))
        return None

    def report(self) -> str:
        return (f"{self.name}: получено {self.ok}, нет логотипа {self.missing}, "
                f"сбоев сети {self.errors}, придержано {self.throttled}")


def save(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def bsc_tokens() -> list[tuple[str, str]]:
    """Все токены из token_cache. Отбор по оборотам убран: он требовал
    соединения с trades, а без индекса по timestamp запрос вешал скрипт."""
    if not os.path.isfile(DB):
        print(f"нет базы {DB}", file=sys.stderr)
        return []
    con = sqlite3.connect(DB)
    con.row_factory = sqlite3.Row
    try:
        rows = con.execute(
            "SELECT symbol, address FROM token_cache "
            "WHERE symbol NOT IN ('UNKNOWN','') AND address LIKE '0x%'"
        ).fetchall()
    except sqlite3.Error as e:
        print("не читается token_cache:", e, file=sys.stderr)
        return []
    finally:
        con.close()
    out, seen = [], set()
    for r in rows:
        a = str(r["address"] or "").lower()
        if len(a) == 42 and a not in seen:
            seen.add(a)
            out.append((str(r["symbol"]), a))
    return out


def hl_coins() -> list[str]:
    try:
        req = urllib.request.Request(
            HL_INFO, data=json.dumps({"type": "allMids"}).encode(),
            headers={**UA, "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as r:
            raw = json.loads(r.read().decode())
        return sorted(str(k) for k in raw) if isinstance(raw, dict) else []
    except Exception as e:
        print("список монет Hyperliquid не получен:", e, file=sys.stderr)
        return []


def write_manifest() -> dict:
    """Список строим по содержимому папки: прерванный прогон всё равно
    оставляет рабочий манифест."""
    man = {"bsc": [], "hl": [], "built": int(time.time())}
    d = os.path.join(OUT, "bsc")
    if os.path.isdir(d):
        man["bsc"] = sorted(f[:-4] for f in os.listdir(d)
                            if f.endswith(".png") and os.path.getsize(os.path.join(d, f)) > 200)
    d = os.path.join(OUT, "hl")
    if os.path.isdir(d):
        names = []
        for f in os.listdir(d):
            if f.endswith(".svg") and os.path.getsize(os.path.join(d, f)) > 200:
                n = f[:-4]
                names.append(n.replace("_", ":") if n.startswith("xyz_") else n)
        man["hl"] = sorted(names)
    with open(MANIFEST, "w", encoding="utf-8") as f:
        json.dump(man, f, ensure_ascii=False, indent=1)
    return man


def main() -> int:
    pcs, tw, hl = Source("pancake"), Source("trustwallet"), Source("hyperliquid")

    toks = bsc_tokens()
    print(f"спот BSC: {len(toks)} токенов", flush=True)
    got = skip = 0
    for i, (_sym, low) in enumerate(toks, 1):
        s = to_checksum(low)
        dest = os.path.join(OUT, "bsc", f"{s}.png")
        if os.path.isfile(dest) and os.path.getsize(dest) > 200:
            skip += 1
            continue
        data = pcs.fetch(f"https://tokens.pancakeswap.finance/images/{s}.png")
        if data is None:
            data = tw.fetch("https://raw.githubusercontent.com/trustwallet/assets/master/"
                            f"blockchains/smartchain/assets/{s}/logo.png")
        if data is not None:
            save(dest, data)
            got += 1
        if i % 50 == 0:
            print(f"  {i}/{len(toks)} — новых {got}, уже было {skip}", flush=True)
        time.sleep(PAUSE)
    print(f"спот готов: новых {got}, уже было {skip}, без логотипа {len(toks)-got-skip}", flush=True)
    print("  " + pcs.report(), flush=True)
    print("  " + tw.report(), flush=True)
    m = write_manifest()
    print(f"манифест после спота: {len(m['bsc'])} записей", flush=True)

    coins = hl_coins()
    print(f"перпы Hyperliquid: {len(coins)} монет", flush=True)
    got = skip = 0
    for i, c in enumerate(coins, 1):
        safe = c.replace(":", "_").replace("/", "_")
        dest = os.path.join(OUT, "hl", f"{safe}.svg")
        if os.path.isfile(dest) and os.path.getsize(dest) > 200:
            skip += 1
            continue
        # двоеточие в xyz:NVDA оставляем: именно с ним адрес и работает
        data = hl.fetch("https://app.hyperliquid.xyz/coins/"
                        + urllib.parse.quote(c, safe=":") + ".svg")
        if data is not None:
            save(dest, data)
            got += 1
        if i % 50 == 0:
            print(f"  {i}/{len(coins)} — новых {got}, уже было {skip}", flush=True)
        time.sleep(PAUSE)
    print(f"перпы готовы: новых {got}, уже было {skip}, без логотипа {len(coins)-got-skip}", flush=True)
    print("  " + hl.report(), flush=True)

    man = write_manifest()
    total = sum(os.path.getsize(os.path.join(d, x))
                for d, _, fs in os.walk(OUT) for x in fs)
    print(f"\nманифест: {MANIFEST} — {len(man['bsc'])} спот + {len(man['hl'])} перп")
    print(f"папка {OUT}: {total/1048576:.1f} МБ")
    if pcs.throttled or tw.throttled or hl.throttled:
        print("\nИсточник придерживал запросы. Часть логотипов могла не скачаться —")
        print("запусти скрипт ещё раз позже, уже скачанное он не тронет.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
