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
BT_PATH = os.path.join(ROOT, "tools", "autotrade_backtest.py")  # окно китов — общее с бэктестом
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
    ("        if self.st.get(\"open\"):\n            return self._skip(\"own_open\"", "        if False:\n            return self._skip(\"own_open\"",
     "своя позиция у бота — одна"),
    ("MARGIN_FRAC_SHARED = 0.05", "MARGIN_FRAC_SHARED = 0.10", "при ручной позиции — маржа 5 %"),
    ("        frac = MARGIN_FRAC_SHARED if foreign else MARGIN_FRAC", "        frac = MARGIN_FRAC", "5 % — только когда есть ручная"),
    ("        if inst in foreign:", "        if False:", "на бумагу с ручной позицией — не входим"),
    ("        if unknown:\n", "        if False:\n", "своя непринятая позиция — новых входов нет"),
    ("min(size_of(p.get(\"pos\")), size_of(op.get(\"size\")))", "size_of(p.get(\"pos\"))", "тейк заново — только на объём бота"),
    ("\"ordType\": \"market\", \"sz\": fmt_dec(sz), \"clOrdId\": cl", "\"ordType\": \"limit\", \"sz\": fmt_dec(sz), \"clOrdId\": cl", "вход — рыночный"),
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
    # Ревизия: ни второго рыночного ордера, ни чужой позиции за свою
    ("        if self.st.get(\"entering\"):\n            return self._skip(\"entering\"", "        if False:\n            return self._skip(\"entering\"",
     "вход не подтверждён — новых нет"),
    ("if self.st.get(\"entering\") and not (e.code.isdigit() and len(e.code) == 5):", "if False:",
     "ошибка шлюза на ордер — не отказ, а сверка"),
    ("if o.get(\"state\") not in (\"filled\", \"partially_filled\") or got_sz <= 0:", "if False:",
     "исполнение не подтверждено — позиции не записываем"),
    ("if o.get(\"state\") in (\"canceled\", \"mmp_canceled\") and got_sz <= 0:", "if False:",
     "рыночный отменён без исполнения — входа нет"),
    ("or self.clock() - num(e.get(\"at\")) > 600_000:", ":", "неподтверждённый вход снимается через 10 минут"),
    ("        if self.unverified:\n", "        if False:\n", "позиции не проверены — входов нет"),
    ("                self.unverified.add(inst)\n", "", "сбой истории — позиция не проверена"),
    ("        live = [p for p in self.positions if p.get(\"instId\") == op[\"inst\"] and self._side_of(p) == op[\"side\"]]\n        if live:",
     "        live = [p for p in self.positions if p.get(\"instId\") == op[\"inst\"]]\n        if live:", "своя позиция — та же сторона (long/short)"),
    ("            if tp.get(\"state\") == \"filled\":", "            if False:", "тейк исполнен — сделка закрыта"),
    ("        if op is not None and not (isinstance(op, dict)", "        if False and not (isinstance(op, dict)", "испорченная запись позиции — забыть"),
    ("        if not os.path.isfile(self.db):", "        if False:", "нет базы — ошибка шага, не выход"),
    # Выход вслед за китом в убытке
    (" for wl, p in sorted(pnl.items()) if p < 0]", " for wl, p in sorted(pnl.items()) if p != 0]", "выход — только когда кит закрыл в убыток"),
    ("            if ts >= since.get(wl, now + 1):", "            if True:", "убыток — только после входа кита в окно"),
    ("        codes = bt.CLOSES_FOR[1 if side > 0 else -1]", "        codes = bt.CLOSES_FOR[-1 if side > 0 else 1]",
     "закрытие кита — той же стороны"),
    ("        if lost:\n            return self._skip(\"whale_exit\"", "        if False:\n            return self._skip(\"whale_exit\"",
     "кит уже вышел в убыток — не входим"),
    ("        self.follow_whales()\n", "\n", "сверка следит за китами"),
    ("            if op.get(\"exit\"):\n                return  # выходим", "            if False:\n                return  # выходим",
     "выходим — тейк заново не ставится"),
    ("        if now - num(op[\"exit\"].get(\"tried\")) < 60_000:", "        if False:", "повтор закрытия — раз в минуту"),
    ("                body[\"reduceOnly\"] = True  # только закрытие", "                pass  # только закрытие",
     "закрытие вслед за китом — reduceOnly"),
    ("min(size_of(live[0].get(\"pos\")), size_of(op.get(\"size\")))", "size_of(live[0].get(\"pos\"))",
     "закрываем только свой объём"),
    ("\"wallets\": info.get(\"wallets\") if isinstance(info.get(\"wallets\"), dict) else {}}", "\"wallets\": {}}",
     "принятая позиция помнит китов"),
    ("                    and str(o.get(\"ordId\") or \"\") not in self.st[\"closed_ids\"]]", "]",
     "закрытая сделка — её вход не повод принять позицию"),
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
    ("BLOCK_CODES = {\"51001\",", "BLOCK_CODES = {\"50000\",", "отказ по бумаге — бумага исключается"),
    ("            if e.code in MARKET_CODES:", "            if False:", "50124 — запрет ключу: рынок на паузе, бумаги не исключаются"),
    ("        if isinstance(mb, dict) and now - num(mb.get(\"at\")) < MARKET_PAUSE_MS:", "        if False:",
     "рынок на паузе — без ордера на каждый сигнал"),
    ("            self.st.pop(\"key_problem\", None)", "            pass", "ордер прошёл — «проблема ключа» снимается"),
    ("MARKET_PAUSE_MS = 30 * 60_000", "MARKET_PAUSE_MS = 300 * 60_000", "пауза рынка — 30 минут"),
    ("                         if not (isinstance(b, dict) and \"50124\" in str(b.get(\"why\", \"\")))}",
     "                         if True}", "прежние недельные исключения за 50124 — снимаются"),
    ("        if key in self._skip_seen:\n            return\n", "", "счётчик: сигнал с причиной — раз в час"),
    ("and num(h) > hour - 24 * 3_600_000}", "}", "счётчик: только сутки"),
    ("isinstance(b, dict) and now - num(b.get(\"at\")) < (BLOCK_MS if b.get(\"permanent\") else FAIL_PAUSE_MS)",
     "isinstance(b, dict) and now - num(b.get(\"at\")) < FAIL_PAUSE_MS",
     "исключение бумаги — на неделю"),
    ("                if e.code == \"50119\":\n                    continue\n", "", "ключ ищется на всех адресах"),
    ("    return \"SWAP\" if inst.endswith(\"-SWAP\") else \"FUTURES\"", "    return \"SWAP\"", "европейский бессрочный — FUTURES"),
    ("                        xperp.setdefault(i.split(\"-\", 1)[0], i)\n", "", "европейский бессрочный — первым"),
    ("startswith(f\"{TAG}in\") and o.get(\"state\")", "startswith(\"\") and o.get(\"state\")", "своя позиция — по метке ордера"),
    ("                    and (1 if o.get(\"side\") == \"buy\" else -1) == side", "                    and True", "своя позиция — та же сторона"),
    ("                \"WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ? ORDER BY ts, tid\", (since, now)",
     "                \"WHERE dir_code IN (1, 2) AND ts >= ? ORDER BY ts, tid\", (since,)", "сделки китов — только до «сейчас»"),
    ("            w.expire(newest - WINDOW_MS)\n", "", "окно — без старше 2 часов"),
    ("        if self.limit_mb() < BT_NEED_MB:", "        if False:", "бэктест — только при достаточной памяти службы"),
    ("            proc.terminate()\n", "", "«Стоп» прерывает пересчёт"),
    ("            except subprocess.TimeoutExpired:\n                pass\n        proc.kill()", "            except subprocess.TimeoutExpired:\n                pass\n            return\n        proc.kill()",
     "не вышел по «Стоп» — kill"),
    ("        cut = int(now * 1000) - COIN_PAUSE_MS", "        cut = int(now * 1000)", "очистка входов — только старше 3 часов"),
    ("and ms - num(b.get(\"at\")) < (BLOCK_MS", "and ms - num(b.get(\"at\")) < 10**15 + (BLOCK_MS", "очистка: истёкшие запреты бумаг"),
    ("            if os.path.getsize(path) > LOG_MAX_BYTES:", "            if False:", "ротация журнала"),
    ("            if now - mtime > CACHE_MAX_DAYS * 86400:", "            if False:", "кеш старше 30 дней — удаляется"),
]

BT_MUTS = [
    ("            self.orders.insert(i, item)", "            self.orders.append(item)", "опоздавшая сделка — на своё место"),
]


def main() -> int:
    # MUTATE_ONLY="часть имени|другая" — только эти (перепроверить выживших).
    only = [x for x in os.environ.get("MUTATE_ONLY", "").split("|") if x]
    pick = lambda ms: [m for m in ms if not only or any(o in m[2] for o in only)]  # noqa: E731
    # Контроль: на чистом коде тесты обязаны проходить — иначе каждая мутация
    # «убита» падением, которое было и без неё.
    for t in TESTS:
        if subprocess.run([sys.executable, t], cwd=ROOT, env=ENV, capture_output=True, timeout=300, check=False).returncode:
            print(f"тесты падают и без мутаций: {t} — проверка недействительна")
            return 2
    alive: list[str] = []
    total = 0
    for path, muts in ((PATH, pick(MUTS)), (BT_PATH, pick(BT_MUTS))):
        alive += run(path, muts)
        total += len(muts) - len(EQUIV & {m[2] for m in muts})
    print(f"ИТОГ: убито {total - len(alive)} из {total}; выжили: {alive}")
    return 1 if alive else 0


def run(path: str, muts: list[tuple[str, str, str]]) -> list[str]:
    with open(path, encoding="utf-8") as f:
        src = f.read()
    alive: list[str] = []
    try:
        for a, b, name in muts:
            if src.count(a) != 1:
                sys.exit(f"мутация «{name}»: текст найден {src.count(a)} раз — обновите список")
            with open(path, "w", encoding="utf-8") as f:
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
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
    return alive


if __name__ == "__main__":
    sys.exit(main())
