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
CORE_PATH = os.path.join(ROOT, "tools", "autotrade_core.py")  # окно китов и база — общие с бэктестом-повтором
RP_PATH = os.path.join(ROOT, "tools", "autotrade_replay.py")    # бэктест-повтор по правилам живого бота
TESTS = ["tests/autotrade_live_test.py", "tests/autotrade_live_security_test.py", "tests/autotrade_live_props_test.py",
         "tests/autotrade_replay_test.py", "tests/autotrade_core_test.py"]
ENV = {**os.environ, "AUTOTRADE_PROPS": "400", "AUTOTRADE_FUZZ": "400", "AUTOTRADE_SCALE_ROWS": "20000",
       "PYTHONDONTWRITEBYTECODE": "1"}
# Выброс встречного окна в опросе — для снимка в сигнале; решение всё равно
# перепроверяет перевес на окнах за 2 часа до «сейчас» (balance) — наблюдаемо
# то же самое.
# Проверка «тейк в записи есть» перед поиском в ожидающих — защита от ответа
# OKX без ordId; у настоящей OKX ordId есть всегда — наблюдаемо то же самое.
EQUIV: set[str] = {"киты против — те же 2 часа", "тейка нет в записи — его надо поставить"}

MUTS = [
    # Правила владельца
    ("MIN_WHALES = 2", "MIN_WHALES = 1", "сигнал — от 2 китов"),
    ("MIN_WHALE_USD = 1000", "MIN_WHALE_USD = 100", "кит — от 1000 $"),
    ("        if mark <= 0 or op[\"side\"] * (mark - stop) > 0:", "        if mark <= 0 or op[\"side\"] * (mark - stop) >= 0:",
     "стоп — дошла до уровня стопа включительно"),
    ("        if not live or stop <= 0:", "        if not live:", "стопа нет — уровней на входе не было"),
    ("        s = {**s, \"stop\": stop}\n", "", "стоп — из уровней на входе"),
    ("                                   \"stop\": s.get(\"stop\", 0.0)}", "                                   }",
     "стоп — и в записи «входим» (сбой между ордером и записью)"),
    ("        mark = num(live[0].get(\"markPx\")) or self.okx.ticker(op[\"inst\"])", "        mark = num(live[0].get(\"markPx\"))",
     "стоп — нет цены позиции — по тикеру"),
    ("            why = self._stop_hit(op)\n", "            why = \"\"\n", "стоп проверяется на сверке"),
    ("else (\"стоп\" if op[\"exit\"].get(\"kind\") == \"stop\" else \"кит вышел в убыток\")",
     "else \"кит вышел в убыток\"", "сделка: причина «стоп»"),
    ("WINDOW_MS = 2 * 3_600_000", "WINDOW_MS = 4 * 3_600_000", "окно китов — 2 часа"),
    ("CHASE = 0.003", "CHASE = 0.03", "не догоняем дальше 0,3 %"),
    ("MATCH_TOL = 0.02", "MATCH_TOL = 0.2", "другой актив — расхождение цен больше 2 %"),
    ("MARGIN_FRAC = 0.05", "MARGIN_FRAC = 0.10", "маржа — всегда 5 % капитала"),
    ("LEVERAGE = 10\n", "LEVERAGE = 20\n", "плечо 10×"),
    ("TP = 0.005 ", "TP = 0.01 ", "тейк +0,5 %"),
    ("COIN_PAUSE_MS = 3 * 3_600_000", "COIN_PAUSE_MS = 3_600_000", "в бумагу — раз в 3 часа"),
    ("if key in seen or now - s[\"ts\"] > FRESH_MS:", "if key in seen:", "сигнал старше 15 минут — не входим"),
    ("        if self.st.get(\"open\"):\n            return self._skip(\"own_open\"", "        if False:\n            return self._skip(\"own_open\"",
     "своя позиция у бота — одна"),
    ("        if inst in foreign:", "        if False:", "на бумагу с ручной позицией — не входим"),
    ("        if unknown:\n", "        if False:\n", "своя непринятая позиция — новых входов нет"),
    ("min(size_of(p.get(\"pos\")), size_of(op.get(\"size\")))", "size_of(p.get(\"pos\"))", "тейк заново — только на объём бота"),
    ("\"ordType\": \"market\", \"sz\": fmt_dec(sz), \"clOrdId\": cl, **self._ps(s[", "\"ordType\": \"limit\", \"sz\": fmt_dec(sz), \"clOrdId\": cl, **self._ps(s[", "вход — рыночный"),
    ("\"ordType\": \"limit\", \"px\": op[\"tp_s\"]", "\"ordType\": \"market\", \"px\": op[\"tp_s\"]", "выход — лимитный"),
    ("            body[\"reduceOnly\"] = True\n", "", "тейк в режиме net — только на закрытие"),
    ("{\"posSide\": \"long\" if side > 0 else \"short\"}", "{\"posSide\": \"short\" if side > 0 else \"long\"}", "сторона позиции в long/short"),
    ("\n    sz = (qty / step).to_integral_value(rounding=ROUND_DOWN) * step",
     "\n    sz = (qty / step).to_integral_value(rounding=ROUND_CEILING) * step", "размер — вниз, не больше 5 %×10"),
    ("    return sz if sz >= min_sz else Decimal(0)", "    return sz", "размер — не меньше минимального ордера"),
    ("tp = round_step(fill_px * (1 + s[\"side\"] * TP), fmt_dec(tick), ROUND_CEILING if s[\"side\"] > 0 else ROUND_FLOOR)",
     "tp = round_step(fill_px * (1 + s[\"side\"] * TP), fmt_dec(tick), ROUND_FLOOR if s[\"side\"] > 0 else ROUND_CEILING)",
     "тейк на шаге — не ближе +1 %"),
    ("in_usd = str(ins.get(\"ctValCcy\", \"\")).upper() in (\"USD\", \"USDC\", \"USDT\")\n    qty",
     "in_usd = False\n    qty", "контракт в долларах"),
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
    # Выход вслед за китом в убытке
    (" for wl, p in sorted(pnl.items()) if p < 0]", " for wl, p in sorted(pnl.items()) if p != 0]", "выход — только когда кит закрыл в убыток"),
    ("            if ts >= since.get(wl, now + 1):", "            if True:", "убыток — только после входа кита в окно"),
    ("        codes = core.CLOSES_FOR[1 if side > 0 else -1]", "        codes = core.CLOSES_FOR[-1 if side > 0 else 1]",
     "закрытие кита — той же стороны"),
    ("        if lost:\n            return self._skip(\"whale_exit\"", "        if False:\n            return self._skip(\"whale_exit\"",
     "кит уже вышел в убыток — не входим"),
    ("        self.follow_whales()\n", "\n", "сверка следит за китами"),
    ("            if op.get(\"exit\"):\n                return  # выходим", "            if False:\n                return  # выходим",
     "выходим — тейк заново не ставится"),
    ("        if now - num(op[\"exit\"].get(\"tried\")) < 60_000:", "        if False:", "повтор закрытия — раз в минуту"),
    ("                body[\"reduceOnly\"] = True  # только закрытие", "                pass  # только закрытие",
     "закрытие вслед за китом — reduceOnly"),
    ("min(size_of(live[0].get(\"pos\")), remaining)", "size_of(live[0].get(\"pos\"))",
     "закрываем только свой объём"),
    ("\"wallets\": info.get(\"wallets\") if isinstance(info.get(\"wallets\"), dict) else {}}", "\"wallets\": {}}",
     "принятая позиция помнит китов"),
    ("                    and str(o.get(\"ordId\") or \"\") not in self.st[\"closed_ids\"]]", "]",
     "закрытая сделка — её вход не повод принять позицию"),
    ("        return {str(w): int(t) for w, t, n in rows if int(n or 0) >= self.limit}",
     "        return {str(w): int(t) for w, t, n in rows}", "восстановленные киты — только от 1000 $"),
    ("        if op and \"wallets\" not in op and num(op.get(\"signal_ts\")) > 0:", "        if False:",
     "позиция прежней версии — киты по базе"),
    # Входим с крупными
    ("        if against > 0 and against >= usd:", "        if False:", "против сигнала киты крупнее — не входим"),
    ("        if against > 0 and against >= usd:", "        if against > 0 and against > usd:", "поровну — не входим"),
    ("                out.append(self._signal(coin, -side, opp, w, newest))", "                pass", "крупных от 2 — входим с ними"),
    ("                opp.expire(newest - WINDOW_MS)", "                pass", "киты против — те же 2 часа"),
    ("            if w is not None:\n                w.expire(now - WINDOW_MS)", "            if False:\n                w.expire(now - WINDOW_MS)",
     "решение — по китам за 2 часа до «сейчас»"),
    ("        usd, against, count = self.sig.balance(s[\"coin\"], s[\"side\"], now)",
     "        usd, against, count = num(s.get(\"usd\")), num(s.get(\"usd_against\")), 2", "решение — по текущему перевесу, а не по строке сигнала"),
    ("        return float(sum(v[0] for v in w.per.values() if v[2] > 0 and v[0] >= self.limit)) / NANOS",
     "        return float(sum(v[0] for v in w.per.values() if v[2] > 0)) / NANOS", "против — только кошельки от 1000 $"),
    # Ревизия 2
    ("                f\"AND (dir_code != {core.FLIP} OR dir LIKE ?)\",", "                \"\",", "разворот в нашу сторону — не выход"),
    ("            if v is None or v[0] < self.limit:\n                continue\n", "", "мелкая сделка сигнал не освежает"),
    ("        if (s[\"coin\"], s[\"side\"]) in self.sig.windows and count < MIN_WHALES:", "        if False:",
     "на момент входа китов от 2"),
    ("            remaining = size_of(op.get(\"size\")) - got[0]", "            remaining = size_of(op.get(\"size\"))",
     "повтор выхода — только остаток своего объёма"),
    ("            op[\"exit\"].setdefault(\"ids\", []).append(cl)\n", "            op[\"exit\"].setdefault(\"ids\", [])\n",
     "метка ордера выхода — в запись до отправки"),
    # Частичный рыночный вход
    ("            if op.get(\"in_done\") is False:\n                self._entry_fill(op)", "            if False:\n                self._entry_fill(op)",
     "доисполнение входа — запись и тейк на весь объём"),
    ("            got = min(got, size_of(op.get(\"ordered\") or op.get(\"size\")) or got)", "            pass",
     "доисполнение — не больше заказанного"),
    ("            if op.get(\"tp_id\") and not (tp_o.get(\"state\") == \"live\" and size_of(tp_o.get(\"accFillSz\")) <= 0):\n                self.event(f\"{op['inst']}: вход доисполнился",
     "            if False:\n                self.event(f\"{op['inst']}: вход доисполнился", "тейк уже исполняется — не трогать"),
    ("        if o.get(\"state\") in (\"filled\", \"canceled\", \"mmp_canceled\"):\n            op[\"in_done\"] = True",
     "        if False:\n            op[\"in_done\"] = True", "вход завершён — больше не проверять"),
    ("                if avg > 0 and abs(avg / op[\"px\"] - 1) <= FILL_TOL:", "                if False:", "тейк — от новой средней цены входа"),
    ("            if op.get(\"tp_id\") and any(o.get(\"ordId\") == op[\"tp_id\"]", "            if any(o.get(\"ordId\") == op.get(\"tp_id\")",
     "тейка нет в записи — его надо поставить"),
    # Уровни часового графика (24 ч): лонг — у поддержки, шорт — у сопротивления
    ("        why, stop = self.level_check(inst, last, s[\"side\"])\n        if why:\n            return why\n",
     "        stop = 0.0\n", "уровни проверяются перед входом"),
    ("{\"instId\": inst, \"bar\": \"1H\", \"limit\": \"24\"}", "{\"instId\": inst, \"bar\": \"1H\", \"limit\": \"12\"}",
     "уровни — за 24 часовые свечи"),
    ("{\"instId\": inst, \"bar\": \"1H\", \"limit\": \"24\"}", "{\"instId\": inst, \"bar\": \"4H\", \"limit\": \"24\"}",
     "уровни — часовой график"),
    ("                if math.isfinite(h) and math.isfinite(lo) and 0 < lo <= h:", "                if True:",
     "свечи: мусор — не в счёт"),
    ("        if core.level_ok(lo, hi, last, side) is not False:", "        if core.level_ok(lo, hi, last, -side) is not False:",
     "уровни — по стороне сделки"),
    ("        except (OkxError, OSError):   # нет свечей (отказ", "        except OkxError:   # нет свечей (отказ", "свечи: обрыв сети — как без уровней"),
    # MA30 15-минутного графика: лонг — цена выше MA30, шорт — ниже (после уровней)
    ("        why = self.ma_check(inst, last, s[\"side\"])\n        if why:\n            return why\n", "",
     "MA30 проверяется перед входом"),
    ("{\"instId\": inst, \"bar\": \"15m\", \"limit\": str(core.MA_BARS)}", "{\"instId\": inst, \"bar\": \"5m\", \"limit\": str(core.MA_BARS)}",
     "MA30 — 15-минутный график"),
    ("        return (sum(closes[1:core.MA_BARS]) + last) / core.MA_BARS", "        return sum(closes[:core.MA_BARS]) / core.MA_BARS",
     "MA30: идущая свеча — по цене"),
    ("        if len(closes) < core.MA_BARS or not all(", "        if not all(", "MA30: свечей меньше 30 — нет MA"),
    ("or not all(math.isfinite(c) and c > 0 for c in closes) or last <= 0:", "or last <= 0:", "MA30: мусор в свечах — нет MA"),
    ("        except (OkxError, OSError):   # нет свечей — как без MA", "        except OkxError:   # нет свечей — как без MA",
     "MA30: обрыв сети — как без MA"),
    ("        if core.ma_ok(ma, last, side) is not False:", "        if core.ma_ok(ma, last, -side) is not False:", "MA30 — по стороне сделки"),
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
    ("        return [x for x in self.call_raw(method, path, params, body, private) if isinstance(x, dict)]",
     "        return self.call_raw(method, path, params, body, private)", "в ответе — только объекты"),
    ("abs(px / mark - 1) <= ADOPT_TOL", "True", "подхват — цена входа по рынку"),
    ("BLOCK_CODES = {\"51001\",", "BLOCK_CODES = {\"50000\",", "отказ по бумаге — бумага исключается"),
    ("            if e.code in MARKET_CODES:", "            if False:", "50124 — запрет ключу: рынок на паузе, бумаги не исключаются"),
    ("        if isinstance(mb, dict) and now - num(mb.get(\"at\")) < MARKET_PAUSE_MS:", "        if False:",
     "рынок на паузе — без ордера на каждый сигнал"),
    ("            self.st.pop(\"key_problem\", None)", "            pass", "ордер прошёл — «проблема ключа» снимается"),
    ("MARKET_PAUSE_MS = 30 * 60_000", "MARKET_PAUSE_MS = 300 * 60_000", "пауза рынка — 30 минут"),
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

RP_MUTS = [
    ("        if now < self.busy_until:", "        if False:", "повтор: позиция одна"),
    ("        if now - self.entered.get(s[\"coin\"], -10**15) < lv.COIN_PAUSE_MS:", "        if False:", "повтор: в бумагу — раз в 3 часа"),
    ("        if against > 0 and against >= usd:", "        if False:", "повтор: против — киты крупнее"),
    ("        if count < lv.MIN_WHALES:", "        if False:", "повтор: на момент входа китов от 2"),
    ("        if self.sig.whale_loss(", "        if False and self.sig.whale_loss(", "повтор: кит уже вышел в убыток"),
    ("        if s[\"side\"] * (hl_px / s[\"ref_px\"] - 1) > lv.CHASE:", "        if False:", "повтор: не догоняем"),
    ("self.equity * lv.MARGIN_FRAC * lv.LEVERAGE", "self.equity * 0.1 * lv.LEVERAGE", "повтор: маржа 5 %"),
    ("            if side * (best - tp) >= 0:", "            if False:", "повтор: тейк"),
    ("            if stop > 0 and side * (worst - stop) <= 0:", "            if False:", "повтор: стоп"),
    ("        self.open_trade(s, inst, ins, sz, last, now, core.level_stop(lo, hi, s[\"side\"]))",
     "        self.open_trade(s, inst, ins, sz, last, now, 0.0)", "повтор: стоп — из уровней на входе"),
    ("                px_out = (o if side * (o - stop) <= 0 else stop) * (1 - side * slip)",
     "                px_out = stop * (1 - side * slip)", "повтор: стоп — свеча открылась за ним — по ней"),
    ("            if exit_at is not None and exit_at + SYNC_MS < end:", "            if False:", "повтор: выход вслед за китом"),
    ("        if core.level_ok(lo, hi, last, s[\"side\"]) is False:", "        if False:", "повтор: уровни"),
    ("        if core.ma_ok(self.ma30_15m(inst, now, last), last, s[\"side\"]) is False:", "        if False:", "повтор: MA30"),
    ("            c = self.px.candle(inst, cur - (k - 1) * core.MA_BAR_MS - BAR_MS)", "            c = self.px.candle(inst, cur - (k - 1) * core.MA_BAR_MS)",
     "повтор: MA30 — закрытия закрытых свечей (без будущего)"),
    ("        return (sum(closes) + last) / core.MA_BARS", "        return (sum(closes) + closes[0]) / core.MA_BARS",
     "повтор: MA30 — идущая свеча по цене"),
    ("        got += [c for c in self.px.candles(inst, hour, now) if c[0] + BAR_MS <= now]",
     "        got += list(self.px.candles(inst, hour, now))", "повтор: уровни — без идущей 5-минутки"),
    ("        got += [c for c in self.px.candles(inst, hour, now) if c[0] + BAR_MS <= now]\n", "",
     "повтор: уровни — идущий час по 5-минуткам"),
    ("core.HOUR_MS, hour) if c[0] + core.HOUR_MS <= hour]", "core.HOUR_MS, hour + core.HOUR_MS)]",
     "повтор: уровни — без идущей часовой свечи"),
    ("                               \"params\": {\"instId\": inst, \"bar\": self.bar,", "                               \"params\": {\"instId\": inst, \"bar\": \"5m\",",
     "повтор: часовые свечи — запросом 1H"),
    ("        if isinstance(rows, list) and end <= self.now_ms:", "        if end <= self.now_ms:", "повтор: сбой сети не ложится в кеш"),
    ("                            got[int(k)] = [c for c in v if candle_ok(c)]", "                            got[int(k)] = v",
     "повтор: испорченный кеш — без мусора"),
]

CORE_MUTS = [
    ("            self.orders.insert(i, item)", "            self.orders.append(item)", "опоздавшая сделка — на своё место"),
    ("    if not os.path.isfile(path):\n        raise OSError", "    if False:\n        raise OSError", "нет базы — ошибка шага, не выход"),
    ("        self.count += (k1 > 0 and n1 >= self.limit) - (k0 > 0 and n0 >= self.limit)",
     "        self.count += (k1 > 0 and n1 > self.limit) - (k0 > 0 and n0 > self.limit)", "кит — от порога включительно"),
    ("    return bool(coin) and not coin.startswith(\"@\") and \"/\" not in coin", "    return bool(coin)", "спот — не сигнал"),
    ("    con.execute(\"PRAGMA query_only=1\")\n", "", "база бота — только чтение"),
    ("LEVEL_STOP_BEYOND = 0.005 ", "LEVEL_STOP_BEYOND = 0.05 ", "стоп — 0,5 % за уровнем"),
    ("MA_BARS = 30", "MA_BARS = 20", "MA — 30 свечей"),
    ("MA_BAR_MS = 15 * 60_000", "MA_BAR_MS = 5 * 60_000", "MA — 15-минутные свечи"),
    ("    return px > ma if side > 0 else px < ma", "    return px < ma if side > 0 else px > ma", "MA: лонг — цена выше"),
    ("or ma <= 0 or px <= 0 or abs(px / ma - 1) < 1e-9:", "or ma <= 0 or px <= 0:", "MA: цена на MA — как без неё"),
    ("or ma <= 0 or px <= 0 or abs(px / ma - 1) < 1e-9:", "or ma <= 0 or px <= 0 or abs(px / ma - 1) < 1e-4:", "MA: на MA — только до округления"),
    ("    return lo * (1 - LEVEL_STOP_BEYOND) if side > 0 else hi * (1 + LEVEL_STOP_BEYOND)",
     "    return hi * (1 + LEVEL_STOP_BEYOND) if side > 0 else lo * (1 - LEVEL_STOP_BEYOND)", "стоп: лонг — за поддержкой"),
    ("LEVEL_LONG_BELOW = 0.4", "LEVEL_LONG_BELOW = 0.5", "уровни: лонг — ниже 40 %"),
    ("LEVEL_SHORT_ABOVE = 0.6", "LEVEL_SHORT_ABOVE = 0.5", "уровни: шорт — выше 60 %"),
    ("    return pos < LEVEL_LONG_BELOW if side > 0 else pos > LEVEL_SHORT_ABOVE",
     "    return pos <= LEVEL_LONG_BELOW if side > 0 else pos >= LEVEL_SHORT_ABOVE", "уровни: граница зоны — нет"),
    ("    return pos < LEVEL_LONG_BELOW if side > 0 else pos > LEVEL_SHORT_ABOVE",
     "    return pos > LEVEL_SHORT_ABOVE if side > 0 else pos < LEVEL_LONG_BELOW", "уровни: лонг — у поддержки"),
    ("or not 0 < lo < hi or px <= 0:", "or px <= 0:", "уровни: пустой диапазон — нет уровней"),
    ("LEVEL_MS = 24 * HOUR_MS", "LEVEL_MS = 8 * HOUR_MS", "уровни — за 24 часа"),
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
    for path, muts in ((PATH, pick(MUTS)), (CORE_PATH, pick(CORE_MUTS)), (RP_PATH, pick(RP_MUTS))):
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
