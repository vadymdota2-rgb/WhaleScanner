# Автокопирование китов — полная проверка модуля на сервере (для Claude на сервере)

Правило для этого сервера — **только чтение**:
- без sudo, без правок файлов бота и API, без перезапуска служб;
- `git pull` вручную **не делать**: код обновляет только автодеплой (иначе
  бот не пересобирается);
- боевые базы на запись не открываются;
- ключи (`/etc/whale-api.env`, `/etc/whale-autotrade.env`) не печатаются ни
  целиком, ни частями; значения переменных окружения служб не выводятся;
- всё, что создаётся, — только во временном каталоге (`mktemp -d`), в конце
  он удаляется.

В модуле — реальная торговля на OKX (служба `whale-autotrade`, ключи — только
в `/etc/whale-autotrade.env`, root 600), бэктест-повтор по тем же правилам и
закрытая вкладка владельца.

Части модуля:
- `~/WhaleScanner/tools/autotrade_replay.py` — бэктест-повтор по актуальным правилам
  живого бота (его запускает служба ночью и по кнопке);
- `~/WhaleScanner/tools/autotrade_core.py` — общее торговли и бэктеста (база только на чтение, окно китов);
- `~/WhaleScanner/tools/autotrade_live.py` — служба `whale-autotrade`:
  реальная торговля на OKX и пересчёт бэктеста (ночью и по кнопке) — со
  своим пределом памяти, не в API;
- `~/WalletTracker/wt_autotrade.py` — в API только кнопки «Старт», «Стоп»,
  «Пересчитать» (файлы-запросы) и показ состояния; процессов не запускает;
- `~/WalletTracker/whale_api.py` — маршруты `/api/owner/autotrade…`;
- вкладка «Автоторговля» в приложении (`~/WalletTracker/html/`).

Устройство — `docs/autotrade-how-it-works.md`.

---

## 0. Подготовка

```bash
T=$(mktemp -d)
export PYTHONDONTWRITEBYTECODE=1        # никакого __pycache__ в каталогах бота и API
```

## 1. Версии — дошёл ли автодеплой

```bash
git -C ~/WhaleScanner log --oneline -1     # ожидается коммит «быстрый подсчёт…» (после 4c0d139) или новее
git -C ~/WalletTracker log --oneline -1    # ожидается e850157 или новее
git -C ~/WhaleScanner status --short | head; git -C ~/WalletTracker status --short | head   # изменённых файлов нет (?? — данные бота, это нормально)
systemctl is-active whale-api; systemctl show whale-api -p ActiveEnterTimestamp
git -C ~/WalletTracker log -1 --format=%ci
```

Если версии старее — ничего не тянуть руками, доложить и подождать
автодеплой. API перезапускается только когда менялся его Python-код (`*.py`
в корне Wallet-Tracker); правки одного приложения (`html/`, `src/`, тесты)
перезапуска не требуют. Проверить: `git -C ~/WalletTracker log --format='%h %ci' -1 -- '*.py'`
— это время должно быть раньше запуска службы.

## 2. Ограничения службы API — хватает ли модулю прав и памяти

```bash
systemctl show whale-api -p User -p ProtectHome -p ReadWritePaths -p MemoryMax -p MemoryCurrent
grep -c 'subprocess\|threading' ~/WalletTracker/wt_autotrade.py   # ожидается 0 — API ничего не запускает
systemctl show whale-autotrade -p ActiveState -p MemoryMax -p MemoryCurrent -p NRestarts
```

Ожидается: у `whale-api` — `ProtectHome=read-only`, в `ReadWritePaths` есть
`/home/<пользователь>/WalletTracker`, `MemoryMax` ≈ 1,5 ГБ, и бэктест в ней
больше не идёт. У `whale-autotrade` — `MemoryMax` ≈ 1,75 ГБ (1 879 048 192);
если 384 МБ — служба поставлена старой установкой, бэктест в ней не
запустится (вкладка скажет «переустановите»), владельцу —
`sudo bash /home/vadymdota2/WhaleScanner/tools/autotrade-install.sh` (ключ не спросит —
он уже есть).

## 3. Тесты — во временной копии

Торговля и бэктест-повтор (без сети, ключей и денег; база — синтетическая):

```bash
mkdir -p "$T/ws/tools" "$T/ws/tests"
cp ~/WhaleScanner/tools/autotrade_live.py ~/WhaleScanner/tools/autotrade_core.py ~/WhaleScanner/tools/autotrade_replay.py "$T/ws/tools/"
cp ~/WhaleScanner/tests/autotrade_live_test.py ~/WhaleScanner/tests/autotrade_live_security_test.py \
   ~/WhaleScanner/tests/autotrade_live_props_test.py ~/WhaleScanner/tests/autotrade_replay_test.py \
   ~/WhaleScanner/tests/autotrade_core_test.py "$T/ws/tests/"
cp -r ~/WhaleScanner/deploy "$T/ws/"
(cd "$T/ws" && python3 tests/autotrade_live_test.py)                  # «… проверок на поддельной OKX — без сети, ключей и денег»
(cd "$T/ws" && python3 tests/autotrade_live_security_test.py)         # «12 атак и порч — закрыты»
(cd "$T/ws" && nice -n 19 python3 tests/autotrade_live_props_test.py) # «1500 случаев свойств и 1500 — фаззинга»
(cd "$T/ws" && python3 tests/autotrade_replay_test.py)                # «… проверок повтора по правилам живого бота — без сети»
(cd "$T/ws" && nice -n 19 python3 tests/autotrade_core_test.py)       # 300 000 сделок китов — секунды, пик < 400 МБ
```

Модуль API (без сети, без настоящих баз и ключей — всё подменено в тесте):

```bash
mkdir -p "$T/wt"
cp ~/WalletTracker/*.py ~/WalletTracker/whale-api.service "$T/wt/"
cp -r ~/WalletTracker/tests "$T/wt/"
(cd "$T/wt" && env -u WHALE_API_KEY python3 tests/test_autotrade.py)   # «Ran … tests … OK»
```

## 4. Правила владельца — в коде

```bash
(cd "$T/ws/tools" && python3 - <<'EOF'
import autotrade_live as lv
want = {"MIN_WHALES": 2, "MIN_WHALE_USD": 1000, "MATCH_TOL": 0.10, "AGAINST": True, "WINDOW_MS": 7_200_000, "CHASE": -0.01, "MARGIN_FRAC": 0.05,
        "LEVERAGE": 10, "TP": 0.015, "COIN_PAUSE_MS": 10_800_000,
        "MAX_OPEN": 3, "ENTRY_WAIT_MS": 60_000, "LOSS_STREAK": 2, "LOSS_PAUSE_MS": 7_200_000}
assert not hasattr(lv, "AVG_LOSS"), "усреднения быть не должно"
assert not hasattr(lv.core, "level_ok") and not hasattr(lv.Okx, "range_levels"), "уровней 4ч быть не должно"
bad = {k: (getattr(lv, k), v) for k, v in want.items() if getattr(lv, k) != v}
c = lv.core
rest = (c.STOP_PCT, c.MA_BARS, c.MA_BAR_MS, c.RATING_MIN_TRADES, c.PRIORITY_WINRATE)
if rest != (0.015, 30, 900_000, 5, 0.9):
    bad["стоп, MA30, рейтинг"] = (rest, (0.015, 30, 900_000, 5, 0.9))
print("правила — как у владельца" if not bad else f"РАСХОЖДЕНИЕ: {bad}")
EOF
)
```

Ожидается «правила — как у владельца»: от 2 китов за 2 часа, у каждого от
1000 $; сделка — против китов (обратная сторона, выхода вслед за китом нет); не дороже средней цены китов; маржа 5 % × 10×, кросс;
до 3 своих позиций, на разных бумагах; вход — по рынку, но не хуже средней цены
китов (IOC с границей: остаток за ней снимается, за ценой не гонимся); уровней
4ч нет; в бумагу — раз в 3 часа; тейк +1,5 %; выхода вслед за китом нет; стоп — 1,5 % от цены входа; MA30 15м — лонг выше, шорт
ниже; рейтинг китов по нашим сделкам — выше винрейт раньше,
выше 90 % от 5 сделок — с ним и в одиночку, без MA30 и «против
крупнее»; усреднения нет.

## 5. Бэктест руками — не запускать

Его считает служба ночью и по кнопке «Пересчитать сейчас» (шаг 6); второй
прогон рядом — лишняя память и запросы к OKX.

## 6. Ночной пересчёт, кнопки и итог во вкладке

```bash
D=~/WalletTracker/autotrade_data
ls -la "$D" "$D/latest" 2>/dev/null
cat "$D/switch.json" 2>/dev/null || echo "switch.json нет — модуль выключен («Старт» не нажимали)"
tail -n 20 "$D/last_run.log" 2>/dev/null
cat "$D/backtest_status.json" 2>/dev/null           # running, started, finished, error, next, progress
cat "$D/run/progress.json" 2>/dev/null              # ход идущего пересчёта: stage, done/total, trades, requests
journalctl -u whale-api --since "-2 day" --no-pager | grep '\[AUTO\]' | tail -n 20
journalctl -u whale-autotrade --since "-1 day" --no-pager | grep -v -iE 'key|secret|pass' | tail -n 30
journalctl -k --since "-1 day" --no-pager | grep -iE 'oom|killed process' | tail -n 5   # ожидается пусто
pgrep -af 'autotrade_(replay|backtest)' || echo "пересчёт сейчас не идёт"
for p in $(pgrep -f 'autotrade_(replay|backtest)'); do ps -o pid,ni,rss,etime,cmd -p "$p"; done   # NI 19, RSS < 1 ГБ
```

Что искать в журнале:
- после «Старт» — `[AUTO] старт (владелец)` и `switch.json` с `"on": true`;
- `[AUTO] старт: не записать …` — каталог закрыт на запись — **доложить сразу**;
- `[AUTO] бэктест: …` — сбой прогона, причина — в строке и в `last_run.log`;
- `[AUTO] чужой запрос` — кто-то, кроме владельца, стучался во вкладку
  (номер чата — доложить).

Итог, который видит вкладка:

```bash
python3 - <<'EOF'
import json, os, time
p = os.path.expanduser("~/WalletTracker/autotrade_data/latest/summary.json")
if not os.path.exists(p):
    raise SystemExit("итога ещё нет")
s = json.load(open(p, encoding="utf-8"))
if "last" not in s:
    raise SystemExit("итог прежней сетки — нужен «Пересчитать сейчас» во вкладке (это делает владелец)")
f, z = s["full"], s["sizing"]
print("посчитан:", time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(s["generated_at"])))
print("вердикт:", "правила окупаются" if s["verdict"] else "пока не окупаются")
print("капитал:", z["deposit"], "→", z["deposit_end"], "$, маржа", z["margin_pct"], "% ×", z["leverage"])
print("сигналы:", s["skips"], "| запросов к OKX:", s["data"]["requests"],
      "| сбоев:", s["data"]["errors"])
print("весь период: сделок", f["trades"], "| винрейт", f["win_pct"], "% | итог", f["net"], "$ | PF", f["pf"],
      "| просадка", f["max_dd"], "$ | сделок в день", s["per_day"])
print("последняя четверть:", s["last"])
print("открытая позиция модели:", s["active"] or "нет")
EOF
```

Отчёт целиком — `cat "$D/latest/report.txt"`.

## 7. Закрытость вкладки

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8090/api/owner/autotrade               # без подписи
curl -s -o /dev/null -w '%{http_code}\n' -X POST http://127.0.0.1:8090/api/owner/autotrade/start   # без подписи
```

Ожидается **не 200** (401, 403 или 404). Ключ API не подставлять и не
читать. Что владельцу отвечает 200, а чужому с верной подписью — 404 и
строка `[AUTO] чужой запрос`, проверяют тесты шага 3.

## 8. Ключи — только у службы, торговые адреса — только в торговле

```bash
cd ~/WhaleScanner
grep -rnE 'OKX_(API_KEY|SECRET|PASSPHRASE)' --include=*.cpp --include=*.h --include=*.py . ~/WalletTracker/wt_autotrade.py | grep -v 'tools/autotrade_live.py' || echo "ключи читает только служба торговли (из окружения)"
grep -nE '/api/v5/(trade|account)' tools/autotrade_live.py | head -3   # торговые адреса OKX — только здесь
grep -noE 'https://[a-z0-9./-]+' tools/autotrade_core.py tools/autotrade_replay.py | sort -u   # только www.okx.com (публичное)
grep -nE '/api/v5/(trade|account|asset)' tools/autotrade_core.py tools/autotrade_replay.py || echo "у бэктеста торговых и счётных адресов OKX нет"
stat -c '%U %a' /etc/whale-autotrade.env 2>/dev/null || echo "/etc/whale-autotrade.env нет"   # если есть — root 600, не открывать
```

## 9. Отчёт и кеш — не в каталоге бота

```bash
ls -la ~/WhaleScanner | grep -iE 'autotrade|report\.txt|summary\.json|candles_' || echo "в каталоге бота файлов бэктеста нет"
```

## 10. Уборка

```bash
rm -rf "$T"
```

---

## Что доложить владельцу

1. Версии обоих репозиториев и время перезапуска API; дошёл ли автодеплой.
2. Ограничения службы (ProtectHome, ReadWritePaths, MemoryMax) — хватает ли
   модулю прав на запись и памяти.
3. Тесты: торговля, атаки, свойства, повтор, общее и объём (время и пик
   памяти), модуль API — прошли ли.
4. Правила в коде — «как у владельца» или расхождения.
5. Служба торговли: жива ли (сердце в `live/state.json`), ошибка, ключ OKX.
6. Ночной пересчёт: включён ли модуль, когда был последний, идёт ли сейчас
   (память, приоритет), сбои из `last_run.log` и строки `[AUTO]`.
7. Итог во вкладке: вердикт; капитал на начало и конец; общая статистика
   (сделок, винрейт, итог, PF, просадка, сделок в день); последняя четверть;
   пропуски сигналов; выходы по причинам (тейк, стоп, кит в убытке); открытая
   позиция модели.
8. Вкладка закрыта (коды ответа без подписи); чужие запросы — есть или нет.
9. Ключи — только у службы торговли (root 600), в коде их нет; у бэктеста —
   только публичное API OKX; в каталоге бота файлов бэктеста нет.
10. Что не так — отдельным списком, с точной строкой из журнала.
