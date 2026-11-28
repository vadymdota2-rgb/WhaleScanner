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
# Рейтинг повтора — только по сделкам, закрытым к моменту решения: пока сделка
# открыта, позиция занята и вход всё равно «own_open» — наблюдаемо то же самое
# (проверка — защита от взгляда в будущее).
EQUIV: set[str] = {"киты против — те же 2 часа", "тейка нет в записи — его надо поставить"}

MUTS = [
    ("TP = 0.015 ", "TP = 0.01 ", "тейк +1,5 %"),
    ("        s = {**s, \"stop\": core.fixed_stop(last, s[\"side\"])}", "        s = {**s, \"stop\": 0.0}", "стоп — на входе"),
    # Правила владельца
    ("MIN_WHALES = 2", "MIN_WHALES = 1", "сигнал — от 2 китов"),
    ("MAX_OPEN = 3", "MAX_OPEN = 2", "своих позиций — до 3"),
    ("    if bts.proc is not None:\n        bts.restart_for_update()", "    if False:\n        bts.restart_for_update()", "код обновился — пересчёт заново новым кодом"),
    ("    if mtimes(me) == stamp:\n        return False", "    if True:\n        return False", "код обновился — служба перезапускается"),
    ("        self._done(None, \"код обновился — пересчёт начат заново новой версией\")\n        with open(", "        self._done(None, \"код обновился — пересчёт начат заново новой версией\")\n        if False:\n          with open(", "код обновился — пересчёт заказан заново"),
    ("            if 0 < done < total and spent > 60:", "            if False:", "ход пересчёта — сколько осталось"),
    ("max(1, round(spent * (total - done) / done / 60))", "max(1, round(spent * total / done / 60))", "осталось — по оставшимся сделкам"),
    ("        if self.keep:\n            if self._kept is None:", "        if False:\n            if self._kept is None:", "бэктест — одно соединение с базой"),
    ("        if len(mine) >= MAX_OPEN:", "        if len(mine) > MAX_OPEN:", "больше 3 своих — не открываем"),
    ("        if inst in mine:\n            return self._skip(\"own_same\"", "        if False:\n            return self._skip(\"own_same\"",
     "своя на бумаге — вторую не открываем"),
    ("                continue   # вошли — следующие", "                break   # вошли — следующие", "вошли — и в следующие сигналы опроса"),
    ("            if not inst or inst in mine or inst == waiting or time.time()", "            if not inst or inst == waiting or time.time()", "сверка: свои записанные — не принимать заново"),
    ("waiting = str(e.get(\"inst\")) if isinstance(e, dict) and e.get(\"limit\") else \"\"", "waiting = \"\"", "сверка: висящий лимит входа — его позицию не принимать"),
    ("                    or any(o[\"inst\"] == op[\"inst\"] for o in opens):", "                    or False:", "состояние: повтор бумаги — одна запись"),
    ("        st[\"opens\"] = opens[:MAX_OPEN]", "        st[\"opens\"] = opens", "состояние: не больше 3 позиций"),
    ("else [got_open] if got_open is not None else []", "else []", "состояние прежней версии (open) — подхватывается"),
    ("        self.st[\"opens\"] = [o for o in self.st[\"opens\"] if (o[\"inst\"], o[\"side\"]) != (op[\"inst\"], op[\"side\"])]\n", "",
     "закрытая — из своих позиций"),
    ("        self.st[\"open\"] = self.st[\"opens\"][0] if self.st[\"opens\"] else None\n        self.st.update(", "        self.st.update(",
     "open — первая позиция для прежней вкладки"),
    ("MIN_WHALE_USD = 1000 ", "MIN_WHALE_USD = 100 ", "кит — от 1000 $"),
    ("MIN_WHALE_USD = 1000 ", "MIN_WHALE_USD = 1001 ", "кит — от 1000 $ включительно"),
    ("AGAINST = True", "AGAINST = False", "против китов — по умолчанию"),
    ("            s = {**s, \"side\": -s[\"side\"], \"fade\": True}", "            s = {**s, \"fade\": True}", "против китов — обратная сторона"),
    ("        if AGAINST:\n            # Против", "        if False:\n            # Против", "против китов — включается AGAINST"),
    ("**({\"fade\": True} if s.get(\"fade\") else {})}", "}", "против китов — пометка в записи «входим»"),
    ("**({\"fade\": True} if e.get(\"fade\") else {})}", "}", "против китов — пометка в позиции"),
    ("**({\"fade\": True} if info.get(\"fade\") else {})}", "}", "против китов — пометка при подхвате"),
    ("            if not why and not op.get(\"fade\") and isinstance(", "            if not why and isinstance(", "против китов — без выхода вслед за китом"),
    ("and wallets and not e.get(\"fade\") and self.sig.whale_loss(", "and wallets and self.sig.whale_loss(", "против китов — ордер входа не снимаем из-за кита"),
    ("        if mark > 0 and op[\"side\"] * (mark - stop) > 0:", "        if mark > 0 and op[\"side\"] * (mark - stop) >= 0:",
     "стоп — дошла до уровня стопа включительно"),
    ("        if last <= 0 or op[\"side\"] * (last - stop) > 0:",
     "        if mark <= 0 and (last <= 0 or op[\"side\"] * (last - stop) > 0):", "стоп — цену позиции подтверждает тикер"),
    ("        if last <= 0 or op[\"side\"] * (last - stop) > 0:", "        if last <= 0 or op[\"side\"] * (last - stop) >= 0:",
     "стоп — тикер на стопе включительно"),
    ("            if self.skip_code == \"candles\":\n                self.retry.append(s)\n", "",
     "сбой свечей — сигнал повторяется, пока свеж"),
    ("        sigs, self.retry = [*self.retry, *new], []", "        sigs, self.retry = [*new, *self.retry], []",
     "повтор сигнала — новый той же бумаги важнее"),
    ("abs(out_px / op[\"tp\"] - 1) < 0.0005", "abs(out_px / op[\"tp\"] - 1) < 0.002", "сделка: «тейк» — только по цене тейка"),
    ("        if not live or stop <= 0:", "        if not live:", "стопа нет — уровней на входе не было"),
    ("\"stop\": s.get(\"stop\", 0.0), \"limit\": fmt_dec(lim)", "\"limit\": fmt_dec(lim)", "стоп — и в записи «входим» (сбой между ордером и записью)"),
    ("        mark = mark if mark > 0 else last\n", "", "стоп — нет цены позиции — по тикеру"),
    ("            why = self._stop_hit(op)\n", "            why = \"\"\n", "стоп проверяется на сверке"),
    ("else (\"стоп\" if op[\"exit\"].get(\"kind\") == \"stop\" else \"кит вышел в убыток\")",
     "else \"кит вышел в убыток\"", "сделка: причина «стоп»"),
    ("WINDOW_MS = 2 * 3_600_000", "WINDOW_MS = 4 * 3_600_000", "окно китов — 2 часа"),
    ("CHASE = -0.01", "CHASE = 0.0", "против китов: вход от 1 % лучше цены китов"),
    ("CHASE = -0.01", "CHASE = -0.02", "против китов: 1 %, не 2 %"),
    ("    if CHASE < 0:\n", "    if CHASE <= 0:\n", "текст правила цены: ноль — «не хуже»"),
    ("    if CHASE > 0:\n", "    if CHASE >= 0:\n", "текст правила цены: ноль — «не хуже» без процента"),
    ("PX_EPS = 1e-9", "PX_EPS = 0.0", "допуск на погрешность средней цены китов"),
    ("        if s[\"side\"] * (hl_px / s[\"ref_px\"] - 1) > CHASE + PX_EPS:", "        if s[\"side\"] * (hl_px / s[\"ref_px\"] - 1) > CHASE:",
     "не хуже китов — с допуском на погрешность дроби"),
    ("MATCH_TOL = 0.02", "MATCH_TOL = 0.2", "другой актив — расхождение цен больше 2 %"),
    ("MARGIN_FRAC = 0.05", "MARGIN_FRAC = 0.10", "маржа — всегда 5 % капитала"),
    ("LEVERAGE = 10\n", "LEVERAGE = 20\n", "плечо 10×"),
    ("COIN_PAUSE_MS = 3 * 3_600_000", "COIN_PAUSE_MS = 3_600_000", "в бумагу — раз в 3 часа"),
    ("if key in seen or now - s[\"ts\"] > FRESH_MS:", "if key in seen:", "сигнал старше 15 минут — не входим"),
    ("        if len(mine) >= MAX_OPEN:\n            return self._skip(\"own_open\"", "        if False:\n            return self._skip(\"own_open\"",
     "своих позиций — не больше 3"),
    ("        if inst in foreign:", "        if False:", "на бумагу с ручной позицией — не входим"),
    ("        if unknown:\n", "        if False:\n", "своя непринятая позиция — новых входов нет"),
    ("min(size_of(p.get(\"pos\")), size_of(op.get(\"size\")))", "size_of(p.get(\"pos\"))", "тейк заново — только на объём бота"),
    ("\"ordType\": \"ioc\", \"px\": fmt_dec(lim)", "\"ordType\": \"market\", \"px\": fmt_dec(lim)", "вход — IOC с границей, не голый рынок"),
    ("\"ordType\": \"limit\", \"px\": op[\"tp_s\"]", "\"ordType\": \"market\", \"px\": op[\"tp_s\"]", "выход — лимитный"),
    ("            body[\"reduceOnly\"] = True\n", "", "тейк в режиме net — только на закрытие"),
    ("{\"posSide\": \"long\" if side > 0 else \"short\"}", "{\"posSide\": \"short\" if side > 0 else \"long\"}", "сторона позиции в long/short"),
    ("\n    sz = (qty / step).to_integral_value(rounding=ROUND_DOWN) * step",
     "\n    sz = (qty / step).to_integral_value(rounding=ROUND_CEILING) * step", "размер — вниз, не больше 5 %×10"),
    ("    return sz if sz >= min_sz else Decimal(0)", "    return sz", "размер — не меньше минимального ордера"),
    ("        tp = round_step(fill_px * (1 + side * TP), ins.get(\"tickSz\", \"0.0001\"), ROUND_CEILING if side > 0 else ROUND_FLOOR)", "        tp = round_step(fill_px * (1 + side * TP), ins.get(\"tickSz\", \"0.0001\"), ROUND_FLOOR if side > 0 else ROUND_CEILING)", "тейк на шаге — не ближе +1 %"),
    ("in_usd = str(ins.get(\"ctValCcy\", \"\")).upper() in (\"USD\", \"USDC\", \"USDT\")\n    qty",
     "in_usd = False\n    qty", "контракт в долларах"),
    # Ревизия: ни второго рыночного ордера, ни чужой позиции за свою
    ("        if self.st.get(\"entering\"):\n            return self._skip(\"entering\"", "        if False:\n            return self._skip(\"entering\"",
     "вход не подтверждён — новых нет"),
    ("if self.st.get(\"entering\") and not (e.code.isdigit() and len(e.code) == 5):", "if False:",
     "ошибка шлюза на ордер — не отказ, а сверка"),
    ("        if state in (\"canceled\", \"mmp_canceled\") or (not state and", "        if (not state and", "лимит снят без исполнения — входа нет"),
    ("                if self.clock() >= num(e.get(\"until\")) or lost:", "                if lost:", "лимит: 5 минут прошли — снять остаток"),
    ("                lost = self.clock() < num(e.get(\"until\")) and wallets and not e.get(\"fade\") and self.sig.whale_loss(",
     "                lost = False and self.sig.whale_loss(", "лимит: кит закрылся в убытке, пока лимит ждал, — снять"),
    ("        got = min(size_of(o.get(\"accFillSz\")), ordered) if ordered > 0 else", "        got = size_of(o.get(\"accFillSz\")) if ordered > 0 else", "лимит: не больше заказанного"),
    ("\"until\": now + ENTRY_WAIT_MS,", "\"until\": now + 10 * ENTRY_WAIT_MS,", "ордер входа ждёт минуту"),
    ("s[\"ref_px\"] / scale * (1 + s[\"side\"] * (CHASE + PX_EPS)), tick, ROUND_FLOOR if s[\"side\"] > 0 else ROUND_CEILING)",
     "s[\"ref_px\"] / scale * (1 + s[\"side\"] * (CHASE + PX_EPS)), tick, ROUND_CEILING if s[\"side\"] > 0 else ROUND_FLOOR)", "граница входа — на шаге внутрь"),
    ("s[\"ref_px\"] / scale * (1 + s[\"side\"] * (CHASE + PX_EPS)), tick,", "last * (1 + s[\"side\"] * (CHASE + PX_EPS)), tick,", "граница входа — от средней китов, не от рынка"),
    ("s[\"ref_px\"] / scale * (1 + s[\"side\"] * (CHASE + PX_EPS)), tick,", "s[\"ref_px\"] / scale * (1 - s[\"side\"] * (CHASE + PX_EPS)), tick,", "граница входа — в сторону сделки"),
    ("        if self._resolve_entry(self.st[\"entering\"]) == \"miss\":", "        if False:", "вход не исполнился — пропуск в том же шаге"),
    ("            if err.code != \"51603\":", "            if True:", "лимит: «ордера нет» (51603) — как не найден"),
    ("and size_of(o.get(\"accFillSz\")) > 0:\n            self._open_from_fill(e, o)", "and size_of(o.get(\"accFillSz\")) >= 0:\n            self._open_from_fill(e, o)", "лимит: позиция — только из исполненного"),
    ("(not state and self.clock() - num(e.get(\"at\")) > 600_000)", "(not state)", "лимит: ордера не найти — снять через 10 минут"),
    ("or self.clock() - num(e.get(\"at\")) > 600_000:", ":", "неподтверждённый вход снимается через 10 минут"),
    ("        if self.unverified:\n", "        if False:\n", "позиции не проверены — входов нет"),
    ("                self.unverified.add(inst)\n", "", "сбой истории — позиция не проверена"),
    ("        live = [p for p in self.positions if p.get(\"instId\") == op[\"inst\"] and self._side_of(p) == op[\"side\"]]\n        if live:",
     "        live = [p for p in self.positions if p.get(\"instId\") == op[\"inst\"]]\n        if live:", "своя позиция — та же сторона (long/short)"),
    ("            if tp.get(\"state\") == \"filled\":", "            if False:", "тейк исполнен — сделка закрыта"),
    ("            if not (isinstance(op, dict) and all(k in op for k in OPEN_KEYS)", "            if False and not (isinstance(op, dict) and all(k in op for k in OPEN_KEYS)",
     "испорченная запись позиции — забыть"),
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
    ("\"wallets\": info.get(\"wallets\") if isinstance(info.get(\"wallets\"), dict) else {},\n", "\"wallets\": {},\n",
     "принятая позиция помнит китов"),
    ("                    and str(o.get(\"ordId\") or \"\") not in self.st[\"closed_ids\"]]", "]",
     "закрытая сделка — её вход не повод принять позицию"),
    ("        return {str(w): int(t) for w, t, n in rows if int(n or 0) >= self.limit}",
     "        return {str(w): int(t) for w, t, n in rows}", "восстановленные киты — только от 1000 $"),
    ("        if \"wallets\" not in op and num(op.get(\"signal_ts\")) > 0:", "        if False:",
     "позиция прежней версии — киты по базе"),
    # Входим с крупными
    ("        if against > 0 and against >= usd and not prio:", "        if False:", "против сигнала киты крупнее — не входим"),
    ("        if against > 0 and against >= usd and not prio:", "        if against > 0 and against > usd and not prio:", "поровну — не входим"),
    ("                out.append(self._signal(coin, -side, opp, w, newest))", "                pass", "крупных от 2 — входим с ними"),
    ("                opp.expire(newest - WINDOW_MS)", "                pass", "киты против — те же 2 часа"),
    ("            if w is not None:\n                w.expire(now - WINDOW_MS)", "            if False:\n                w.expire(now - WINDOW_MS)",
     "решение — по китам за 2 часа до «сейчас»"),
    ("        usd, against, count = self.sig.balance(s[\"coin\"], s[\"side\"], now)",
     "        usd, against, count = num(s.get(\"usd\")), num(s.get(\"usd_against\")), 2", "решение — по текущему перевесу, а не по строке сигнала"),
    # Ревизия 2
    ("            f\"AND (dir_code != {core.FLIP} OR dir LIKE ?)\",\n            (coin, *since,", "            \"\",\n            (coin, *since,", "разворот в нашу сторону — не выход"),
    ("            if v is None or v[0] < self.limit:\n                continue\n", "", "мелкая сделка сигнал не освежает"),
    ("        if (s[\"coin\"], s[\"side\"]) in self.sig.windows and count < MIN_WHALES and not prio:", "        if False:",
     "на момент входа китов от 2"),
    ("            remaining = size_of(op.get(\"size\")) - got[0]", "            remaining = size_of(op.get(\"size\"))",
     "повтор выхода — только остаток своего объёма"),
    ("            op[\"exit\"].setdefault(\"ids\", []).append(cl)\n", "            op[\"exit\"].setdefault(\"ids\", [])\n",
     "метка ордера выхода — в запись до отправки"),
    # Тейк после сбоя
    ("            if op.get(\"tp_id\") and any(o.get(\"ordId\") == op[\"tp_id\"]", "            if any(o.get(\"ordId\") == op.get(\"tp_id\")",
     "тейка нет в записи — его надо поставить"),
    # Уровни часового графика (24 ч): лонг — у поддержки, шорт — у сопротивления
    # MA30 15-минутного графика: лонг — цена выше MA30, шорт — ниже
    ("        why = \"\" if prio else self.ma_check(inst, last, s[\"side\"])\n", "        why = \"\"\n",
     "MA30 проверяется перед входом"),
    ("self.ma_check(inst, last, s[\"side\"])", "self.ma_check(inst, last, -s[\"side\"])", "MA30 — по стороне сделки"),
    ("{\"instId\": inst, \"bar\": \"15m\", \"limit\": str(core.MA_BARS)}", "{\"instId\": inst, \"bar\": \"5m\", \"limit\": str(core.MA_BARS)}",
     "MA30 — 15-минутный график"),
    ("        return (sum(closes[1:core.MA_BARS]) + last) / core.MA_BARS", "        return sum(closes[:core.MA_BARS]) / core.MA_BARS",
     "MA30: идущая свеча — по цене"),
    ("        if len(closes) < core.MA_BARS or not all(", "        if not all(", "MA30: свечей меньше 30 — нет MA"),
    ("or not all(math.isfinite(c) and c > 0 for c in closes) or last <= 0:", "or last <= 0:", "MA30: мусор в свечах — нет MA"),
    ("            ma = self.okx.ma30_15m(inst, last)\n        except (OkxError, OSError) as e:",
     "            ma = self.okx.ma30_15m(inst, last)\n        except OkxError as e:", "MA30: обрыв сети — пропуск, не падение"),
    ("— повтор на следующем опросе\")\n", "— повтор на следующем опросе\") and \"\"\n", "MA30: сбой свечей — не входим"),
    ("        if core.ma_ok(ma, last, side) is not False:", "        if core.ma_ok(ma, last, -side) is not False:", "MA30 — по стороне сделки"),
    # Рейтинг китов: выше наш винрейт — раньше; > 90 % (от 5) — приоритетный
    ("            if (w.count >= MIN_WHALES or wallet in self.priority) and w.big_u > 0:",
     "            if w.count >= MIN_WHALES and w.big_u > 0:", "приоритетный кит — сигнал и в одиночку"),
    ("                     and v[0] >= self.limit), \"\")", "                     ), \"\")", "приоритетный кит — в окне от 1000 $"),
    ("        if against > 0 and against >= usd and not prio:", "        if against > 0 and against >= usd:", "приоритетный — без «против крупнее»"),
    ("count < MIN_WHALES and not prio:", "count < MIN_WHALES:", "приоритетный — и один в окне"),
    ("        why = \"\" if prio else self.ma_check(", "        why = self.ma_check(",
     "приоритетный — без MA30"),
    ("        out.sort(key=lambda s: -self.rating.best(s.get(\"wallets\")))\n", "", "кандидаты — по нашему винрейту с китами"),
    ("                        self.rating.add(t.get(\"wallets\"), num(t.get(\"pnl\")) > 0)",
     "                        self.rating.add(t.get(\"wallets\"), num(t.get(\"pnl\")) >= 0)", "рейтинг: плюсовая — pnl > 0"),
    ("        self.rating.add(trade[\"wallets\"], pnl > 0)\n        self.sig.priority = self.rating.priority()\n", "",
     "рейтинг — после каждой закрытой сделки"),
    ("                 \"wallets\": sorted(w for w in op.get(\"wallets\") or {} if isinstance(w, str))}", "                 }",
     "сделка — с китами сигнала (для рейтинга)"),
    # Защита
    ("fill_px = avg if avg > 0 and abs(avg / ref - 1) <= FILL_TOL else ref", "fill_px = avg if avg > 0 else ref", "цена исполнения — рядом с рынком"),
    ("        ref = num(e.get(\"last\")) or num(e.get(\"limit\"))", "        ref = num(e.get(\"limit\"))", "мусорная цена исполнения — заменяется ценой рынка"),
    ("        if sz <= 0 or not (op.get(\"px\", 0) > 0 and op.get(\"tp\", 0) > 0\n                           and op[\"side\"] * (op[\"tp\"] / op[\"px\"] - 1) >= TP * 0.999):",
     "        if sz <= 0:", "тейк — только по ту сторону, не ближе +1 %"),
    ("        if lim <= 0 or tp_try <= 0 or not TP * 0.999 <=", "        if lim <= 0 or False and not TP * 0.999 <=", "шаг цены слишком крупный — не входим"),
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
    ("            \"WHERE dir_code IN (1, 2) AND ts >= ? AND ts <= ? ORDER BY ts, tid\", (since, now))", "            \"WHERE dir_code IN (1, 2) AND ts >= ? ORDER BY ts, tid\", (since,))", "сделки китов — только до «сейчас»"),
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
    ("        side = -s[\"side\"] if lv.AGAINST else s[\"side\"]", "        side = s[\"side\"] if lv.AGAINST else -s[\"side\"]", "повтор: против китов — обратная сторона"),
    ("        exit_at = None if lv.AGAINST else self.loss_exit_time(", "        exit_at = self.loss_exit_time(", "повтор: против китов — без выхода вслед за китом"),
    ("        if len(self.busy) >= lv.MAX_OPEN:\n            return", "        if False:\n            return", "повтор: позиций не больше 3"),
    ("        if len(self.busy) >= lv.MAX_OPEN:", "        if len(self.busy) > lv.MAX_OPEN:", "повтор: позиций до 3"),
    ("        if now - self.entered.get(s[\"coin\"], -10**15) < lv.COIN_PAUSE_MS:", "        if False:", "повтор: в бумагу — раз в 3 часа"),
    ("        self.settle(10**17)   # закрытые", "        pass   # закрытые", "повтор: в конце — закрытые в капитал"),
    ("        start, entry = now, last * (1 + side * slip)", "        start, entry = now, last", "повтор: по рынку — проскальзывание"),
    ("        fees = qty * entry * a.fee_taker / 100", "        fees = qty * entry * a.fee_maker / 100", "повтор: по рынку — тейкер"),
    ("        if self.sig.whale_loss(s[\"coin\"], s[\"side\"], s.get(\"wallets\") or {}, now):",
     "        if False:", "повтор: кит уже вышел в убыток"),
    ("        if side * (hl_px / s[\"ref_px\"] - 1) > lv.CHASE + lv.PX_EPS:", "        if False:", "повтор: не догоняем"),
    ("        if side * (hl_px / s[\"ref_px\"] - 1) > lv.CHASE + lv.PX_EPS:", "        if side * (hl_px / s[\"ref_px\"] - 1) > lv.CHASE:",
     "повтор: не хуже китов — с допуском на погрешность дроби"),
    ("        bound = s[\"ref_px\"] / core.okx_candidate(s[\"coin\"])[1] * (1 + side * lv.CHASE)",
     "        bound = s[\"ref_px\"] / core.okx_candidate(s[\"coin\"])[1] * (1 - side * lv.CHASE)", "повтор: граница входа — в сторону сделки"),
    ("core.fixed_stop(last, side), side)", "0.0, side)", "повтор: стоп входа"),
    ("        if not prio and core.ma_ok(", "        if False and core.ma_ok(", "повтор: MA30"),
    ("        if not prio and core.ma_ok(", "        if core.ma_ok(", "повтор: приоритетный — без MA30"),
    ("self.ma30_15m(inst, now, last), last, side) is False:", "self.ma30_15m(inst, now, last), last, s[\"side\"]) is False:",
     "повтор: против китов — MA30 по стороне сделки"),
    ("            self.sig.priority = self.rating.priority()", "            pass", "повтор: одиночный кит — только приоритетный"),
    ("        \"halves\": halves(trades, since, now),", "", "повтор: итог по половинам — в итоге"),
    ("        for attempt in range(3):", "        for attempt in range(1):", "повтор: свечи — до 3 попыток"),
    ("        return idx.get(t // BAR_MS * BAR_MS)", "        return idx.get(t)", "повтор: свеча — по началу"),
    ("        if against > 0 and against >= usd and not prio:", "        if False:", "повтор: против — киты крупнее"),
    ("        if count < lv.MIN_WHALES and not prio:", "        if False:", "повтор: на момент входа китов от 2"),
    ("        if count < lv.MIN_WHALES and not prio:", "        if count < lv.MIN_WHALES:", "повтор: приоритетный — и один в окне"),
    ("        while self.closing and self.closing[0][0] <= now:", "        while self.closing:",
     "повтор: капитал и рейтинг — только закрытые к моменту решения"),
    ("            heapq.heappush(self.closing, (t_out, len(self.trades)))\n", "", "повтор: закрытая — в капитал и рейтинг"),
    ("        self.busy = [b for b in self.busy if b[0] > now]\n", "", "повтор: закрытая освобождает место"),
    ("        if any(b[1] == inst for b in self.busy):\n            return \"own_same\"", "        if False:\n            return \"own_same\"",
     "повтор: своя на бумаге — вторую не открываем"),
    ("        if side * (entry - bound) > 0:\n            entry = bound\n", "", "повтор: по рынку — не хуже средней китов ±0,3 %"),
    ("if isinstance(w, str))), fees))", "if isinstance(w, str)))))", "повтор: комиссии — в сделке"),
    ("\"fees\": round(sum(t.fees for t in trades), 2),", "\"fees\": round(sum(t.pnl for t in trades), 2),", "повтор: комиссии — в итогах"),
    ("for t in trades if t.entry_t < mid), 0.0), 2),", "for t in trades if t.entry_t <= mid), 0.0), 2),", "повтор: половины — середина во второй"),
    ("for t in trades if t.entry_t >= mid), 0.0), 2)]", "for t in trades if t.entry_t > mid), 0.0), 2)]", "повтор: половины — середина не теряется"),
    ("    mid = since + (now - since) // 2", "    mid = since + (now - since) // 3", "повтор: половины — пополам"),
    ("self.equity * lv.MARGIN_FRAC * lv.LEVERAGE", "self.equity * 0.1 * lv.LEVERAGE", "повтор: маржа 5 %"),
    ("            if side * (best - tp) >= 0:", "            if False:", "повтор: тейк"),
    ("            if stop > 0 and side * (worst - stop) <= 0:", "            if False:", "повтор: стоп"),
    ("                px_out = (o if side * (o - stop) <= 0 else stop) * (1 - side * slip)",
     "                px_out = stop * (1 - side * slip)", "повтор: стоп — свеча открылась за ним — по ней"),
    ("            if exit_at is not None and exit_at + SYNC_MS < end:", "            if False:", "повтор: выход вслед за китом"),
    ("            c = self.px.candle(inst, cur - (k - 1) * core.MA_BAR_MS - BAR_MS)", "            c = self.px.candle(inst, cur - (k - 1) * core.MA_BAR_MS)",
     "повтор: MA30 — закрытия закрытых свечей (без будущего)"),
    ("        return (sum(closes) + last) / core.MA_BARS", "        return (sum(closes) + closes[0]) / core.MA_BARS",
     "повтор: MA30 — идущая свеча по цене"),
    ("\"params\": {\"instId\": inst, \"bar\": \"5m\", \"after\":", "\"params\": {\"instId\": inst, \"bar\": \"1m\", \"after\":",
     "повтор: свечи — запросом 5m"),
    ("        if isinstance(rows, list) and end <= self.now_ms:", "        if end <= self.now_ms:", "повтор: сбой сети не ложится в кеш"),
    ("                            got[int(k)] = [c for c in v if candle_ok(c)]", "                            got[int(k)] = v",
     "повтор: испорченный кеш — без мусора"),
]

CORE_MUTS = [
    ("            self.orders.insert(i, item)", "            self.orders.append(item)", "опоздавшая сделка — на своё место"),
    ("    if not os.path.isfile(path):\n        raise OSError", "    if False:\n        raise OSError", "нет базы — ошибка шага, не выход"),
    ("        was, now = k0 > 0 and n0 >= self.limit, k1 > 0 and n1 >= self.limit", "        was, now = k0 > 0 and n0 > self.limit, k1 > 0 and n1 > self.limit", "кит — от порога включительно"),
    ("        self.whale_n += (n1 if now else 0) - (n0 if was else 0)", "        self.whale_n += (n1 if now else 0)", "объём китов окна — по ходу (вышел — вычесть)"),
    ("    return bool(coin) and not coin.startswith(\"@\") and \"/\" not in coin", "    return bool(coin)", "спот — не сигнал"),
    ("    con.execute(\"PRAGMA query_only=1\")\n", "", "база бота — только чтение"),
    ("STOP_PCT = 0.015", "STOP_PCT = 0.02", "стоп — 1,5 % от входа"),
    ("    return px * (1 - side * pct)", "    return px * (1 + side * pct)", "стоп — против сделки"),
    ("    if not math.isfinite(px) or px <= 0:\n        return 0.0\n    return px * (1", "    return px * (1", "стоп: цена — мусор — нет стопа"),
    ("MA_BARS = 30", "MA_BARS = 20", "MA — 30 свечей"),
    ("RATING_MIN_TRADES = 5", "RATING_MIN_TRADES = 4", "рейтинг — от 5 сделок"),
    ("PRIORITY_WINRATE = 0.9", "PRIORITY_WINRATE = 0.95", "приоритетный — выше 90 %"),
    ("if (r := self.rate(w)) is not None and r > PRIORITY_WINRATE}", "if (r := self.rate(w)) is not None and r >= PRIORITY_WINRATE}",
     "приоритетный — строго выше 90 %"),
    ("        return max(rates, default=-1.0)", "        return min(rates, default=-1.0)", "рейтинг сигнала — лучший кит"),
    ("                v[0] += int(win)", "                v[0] += 1", "рейтинг — только плюсовые в числитель"),
    ("MA_BAR_MS = 15 * 60_000", "MA_BAR_MS = 5 * 60_000", "MA — 15-минутные свечи"),
    ("    return px > ma if side > 0 else px < ma", "    return px < ma if side > 0 else px > ma", "MA: лонг — цена выше"),
    ("or ma <= 0 or px <= 0 or abs(px / ma - 1) < 1e-9:", "or ma <= 0 or px <= 0:", "MA: цена на MA — как без неё"),
    ("or ma <= 0 or px <= 0 or abs(px / ma - 1) < 1e-9:", "or ma <= 0 or px <= 0 or abs(px / ma - 1) < 1e-4:", "MA: на MA — только до округления"),
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
