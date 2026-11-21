#!/usr/bin/env python3
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
"""Мутационная проверка реальной торговли: python3 tools/mutate-autotrade.py

Портим tools/autotrade_live.py по одному месту (правило владельца, граница,
знак, защита) и гоняем его тесты. Убитая мутация — тесты заметили порчу;
выжившая — дыра в тестах. Файл после каждой мутации возвращается как был.
Эквивалентные мутации (не меняют ничего наблюдаемого) помечены и в счёт не
идут.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = os.path.join(ROOT, "tools", "autotrade_live.py")
TESTS = ["tests/autotrade_live_test.py", "tests/autotrade_live_security_test.py", "tests/autotrade_live_props_test.py"]
ENV = {**os.environ, "AUTOTRADE_PROPS": "400", "AUTOTRADE_FUZZ": "400", "PYTHONDONTWRITEBYTECODE": "1"}
EQUIV: set[str] = set()

MUTS = [
    # Правила владельца
    ("MIN_WHALES = 2", "MIN_WHALES = 1", "сигнал — от 2 китов"),
    ("MIN_WHALE_USD = 1000", "MIN_WHALE_USD = 100", "кит — от 1000 $"),
    ("WINDOW_MS = 2 * 3_600_000", "WINDOW_MS = 4 * 3_600_000", "окно китов — 2 часа"),
    ("CHASE = 0.003", "CHASE = 0.03", "не догоняем дальше 0,3 %"),
    ("MATCH_TOL = 0.02", "MATCH_TOL = 0.2", "другой актив — расхождение цен больше 2 %"),
    ("MARGIN_FRAC = 0.10", "MARGIN_FRAC = 0.20", "маржа — 10 % капитала"),
    ("LEVERAGE = 10\n", "LEVERAGE = 20\n", "плечо 10×"),
    ("TP = 0.01\n", "TP = 0.005\n", "тейк +1 %"),
    ("COIN_PAUSE_MS = 3 * 3_600_000", "COIN_PAUSE_MS = 3_600_000", "в бумагу — раз в 3 часа"),
    ("if key in seen or now - s[\"ts\"] > FRESH_MS:", "if key in seen:", "сигнал старше 15 минут — не входим"),
    ("        if self.st.get(\"open\"):\n            return f\"у бота уже есть своя позиция", "        if False:\n            return f\"у бота уже есть своя позиция",
     "своя позиция у бота — одна"),
    ("MARGIN_FRAC_SHARED = 0.05", "MARGIN_FRAC_SHARED = 0.10", "при ручной позиции — маржа 5 %"),
    ("        frac = MARGIN_FRAC_SHARED if foreign else MARGIN_FRAC", "        frac = MARGIN_FRAC", "5 % — только когда есть ручная"),
    ("        if inst in foreign:", "        if False:", "на бумагу с ручной позицией — не входим"),
    ("        if unknown:\n", "        if False:\n", "своя непринятая позиция — новых входов нет"),
    ("min(size_of(p.get(\"pos\")), size_of(op.get(\"size\")))", "size_of(p.get(\"pos\"))", "тейк заново — только на объём бота"),
    ("\"ordType\": \"market\", \"sz\": fmt_dec(sz)", "\"ordType\": \"limit\", \"sz\": fmt_dec(sz)", "вход — рыночный"),
    ("\"ordType\": \"limit\", \"px\": op[\"tp_s\"]", "\"ordType\": \"market\", \"px\": op[\"tp_s\"]", "выход — лимитный"),
    ("            body[\"reduceOnly\"] = True\n", "", "тейк в режиме net — только на закрытие"),
    ("{\"posSide\": \"long\" if side > 0 else \"short\"}", "{\"posSide\": \"short\" if side > 0 else \"long\"}", "сторона позиции в long/short"),
    ("sz = (qty / step).to_integral_value(rounding=ROUND_DOWN) * step",
     "sz = (qty / step).to_integral_value(rounding=ROUND_CEILING) * step", "размер — вниз, не больше 10 %×10"),
    ("tp = round_step(fill_px * (1 + s[\"side\"] * TP), fmt_dec(tick), ROUND_CEILING if s[\"side\"] > 0 else ROUND_FLOOR)",
     "tp = round_step(fill_px * (1 + s[\"side\"] * TP), fmt_dec(tick), ROUND_FLOOR if s[\"side\"] > 0 else ROUND_CEILING)",
     "тейк на шаге — не ближе +1 %"),
    ("in_usd = str(ins.get(\"ctValCcy\", \"\")).upper() in (\"USD\", \"USDC\", \"USDT\")\n        qty",
     "in_usd = False\n        qty", "контракт в долларах"),
    # Защита
    ("fill_px = avg if avg > 0 and abs(avg / last - 1) <= FILL_TOL else last", "fill_px = avg if avg > 0 else last",
     "цена исполнения — рядом с тикером"),
    ("        if sz <= 0 or not (op.get(\"px\", 0) > 0 and op.get(\"tp\", 0) > 0\n                           and op[\"side\"] * (op[\"tp\"] / op[\"px\"] - 1) >= TP * 0.999):",
     "        if sz <= 0:", "тейк — только по ту сторону, не ближе +1 %"),
    ("        if tp_try <= 0 or not TP * 0.999 <=", "        if False and not TP * 0.999 <=", "шаг цены слишком крупный — не входим"),
    ("        raise OkxError(str(code), f\"перенаправление", "        return None\n        raise OkxError(str(code), f\"перенаправление",
     "перенаправление — отказ"),
    ("    if not url.startswith(tuple(f\"{b}/\" for b in OKX_BASES)):", "    if not url.startswith(\"https://\"):",
     "запросы — только на адреса OKX"),
    ("    return x if math.isfinite(x) else default", "    return x", "NaN и бесконечность — не числа"),
    ("    return d if d.is_finite() and 0 < d <= DEC_MAX else None", "    return d if d.is_finite() else None",
     "параметр бумаги — положительный"),
    ("[x for x in data if isinstance(x, dict)] if isinstance(data, list)", "data if isinstance(data, list)",
     "в ответе — только объекты"),
    ("abs(px / mark - 1) <= ADOPT_TOL", "True", "подхват — цена входа по рынку"),
    ("BLOCK_CODES = {\"50124\",", "BLOCK_CODES = {\"50000\",", "50124 — бумага исключается"),
    ("now - int(b.get(\"at\", 0)) < (BLOCK_MS if b.get(\"permanent\") else FAIL_PAUSE_MS)", "now - int(b.get(\"at\", 0)) < FAIL_PAUSE_MS",
     "исключение бумаги — на неделю"),
    ("                if e.code == \"50119\":\n                    continue\n", "", "ключ ищется на всех адресах"),
    ("    return \"SWAP\" if inst.endswith(\"-SWAP\") else \"FUTURES\"", "    return \"SWAP\"", "европейский бессрочный — FUTURES"),
    ("                        xperp.setdefault(i.split(\"-\", 1)[0], i)\n", "", "европейский бессрочный — первым"),
    ("startswith(f\"{TAG}in\") and o.get(\"state\")", "startswith(\"\") and o.get(\"state\")", "своя позиция — по метке ордера"),
    ("                    and (1 if o.get(\"side\") == \"buy\" else -1) == side", "                    and True", "своя позиция — та же сторона"),
    ("                \"WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ? ORDER BY ts, tid\", (since, now)",
     "                \"WHERE dir_code IN (1, 2) AND ts >= ? ORDER BY ts, tid\", (since,)", "сделки китов — только до «сейчас»"),
    ("            w.expire(ts - WINDOW_MS)\n            w.add(", "            w.add(", "окно — без старше 2 часов"),
    ("        if self.limit_mb() < BT_NEED_MB:", "        if False:", "бэктест — только при достаточной памяти службы"),
    ("                self.proc.terminate()\n", "", "«Стоп» прерывает пересчёт"),
    ("        cut = int(now * 1000) - COIN_PAUSE_MS", "        cut = int(now * 1000)", "очистка входов — только старше 3 часов"),
    ("            if os.path.getsize(path) > LOG_MAX_BYTES:", "            if False:", "ротация журнала"),
    ("            if now - mtime > CACHE_MAX_DAYS * 86400:", "            if False:", "кеш старше 30 дней — удаляется"),
]


def main() -> int:
    with open(PATH, encoding="utf-8") as f:
        src = f.read()
    alive: list[str] = []
    # MUTATE_ONLY="часть имени|другая" — только эти (перепроверить выживших).
    only = [x for x in os.environ.get("MUTATE_ONLY", "").split("|") if x]
    muts = [m for m in MUTS if not only or any(o in m[2] for o in only)]
    # Контроль: на чистом коде тесты обязаны проходить — иначе каждая мутация
    # «убита» падением, которое было и без неё.
    for t in TESTS:
        if subprocess.run([sys.executable, t], cwd=ROOT, env=ENV, capture_output=True, timeout=300, check=False).returncode:
            print(f"тесты падают и без мутаций: {t} — проверка недействительна")
            return 2
    try:
        for a, b, name in muts:
            if src.count(a) != 1:
                sys.exit(f"мутация «{name}»: текст найден {src.count(a)} раз — обновите список")
            with open(PATH, "w", encoding="utf-8") as f:
                f.write(src.replace(a, b))
            killed = False
            for t in TESTS:
                try:
                    r = subprocess.run([sys.executable, t], cwd=ROOT, env=ENV, capture_output=True, timeout=300, check=False)
                except subprocess.TimeoutExpired:
                    killed = True
                    break
                if r.returncode != 0:
                    killed = True
                    break
            tag = "убита   " if killed else ("экв.    " if name in EQUIV else "ВЫЖИЛА  ")
            print(tag + name, flush=True)
            if not killed and name not in EQUIV:
                alive.append(name)
    finally:
        with open(PATH, "w", encoding="utf-8") as f:
            f.write(src)
    total = len(muts) - len(EQUIV & {m[2] for m in muts})
    print(f"ИТОГ: убито {total - len(alive)} из {total}; выжили: {alive}")
    return 1 if alive else 0


if __name__ == "__main__":
    sys.exit(main())
