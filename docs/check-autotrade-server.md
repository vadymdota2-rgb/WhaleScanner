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

В модуле сейчас — **этап 1, бэктест**, и закрытая вкладка владельца.
Бумажной и реальной торговли (этапы 2–3) в коде нет: они строятся только
после «да» владельца по отчёту.

Части модуля:
- `~/WhaleScanner/tools/autotrade_backtest.py` — бэктест;
- `~/WhaleScanner/tools/autotrade_live.py` — служба `whale-autotrade`:
  реальная торговля на OKX и пересчёт бэктеста (ночью и по кнопке) — со
  своим пределом памяти, не в API;
- `~/WalletTracker/wt_autotrade.py` — в API только кнопки «Старт», «Стоп»,
  «Пересчитать» (файлы-запросы) и показ состояния; процессов не запускает;
- `~/WalletTracker/whale_api.py` — маршруты `/api/owner/autotrade…`;
- вкладка «Автоторговля» в приложении (`~/WalletTracker/html/`).

Устройство по строкам — `docs/autotrade-how-it-works.md`.

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
`sudo bash ~/WhaleScanner/tools/autotrade-install.sh` (ключ не спросит —
он уже есть).

## 3. Тесты — во временной копии

Бэктест (без сети, база — синтетическая):

```bash
mkdir -p "$T/ws/tools" "$T/ws/tests"
cp ~/WhaleScanner/tools/autotrade_backtest.py "$T/ws/tools/"
cp ~/WhaleScanner/tests/autotrade_backtest_test.py ~/WhaleScanner/tests/autotrade_scale_test.py "$T/ws/tests/"
(cd "$T/ws" && python3 tests/autotrade_backtest_test.py)              # «36 проверок без сети и без записи в базу»
(cd "$T/ws" && nice -n 19 python3 tests/autotrade_scale_test.py)      # 300 000 ордеров, пик < 700 МБ; ~20–60 с; с пределом 150 МБ — код 3
```

Модуль API (без сети, без настоящих баз и ключей — всё подменено в тесте):

```bash
mkdir -p "$T/wt"
cp ~/WalletTracker/*.py ~/WalletTracker/whale-api.service "$T/wt/"
cp -r ~/WalletTracker/tests "$T/wt/"
(cd "$T/wt" && env -u WHALE_API_KEY python3 tests/test_autotrade.py)   # «Ran 16 tests … OK»
```

## 4. Правила владельца — в коде по умолчанию

```bash
(cd "$T/ws/tools" && python3 - <<'EOF'
import autotrade_backtest as bt
a = bt.parser().parse_args([])
want = {"cluster": "2,3", "cluster_hours": 2, "chase": "0.3", "tp": "1", "sl": "0", "hold": "0", "max_hold": 0,
        "margin": "cross", "leverage": 10, "margin_pct": 10, "max_open": 1, "cooldown": 180, "rest": 0, "markets": "all"}
bad = {k: (getattr(a, k), v) for k, v in want.items() if getattr(a, k) != v}
print("правила — как у владельца" if not bad else f"РАСХОЖДЕНИЕ: {bad}")
print("кит от, $ (сетка):", a.min_whale)
EOF
)
```

Ожидается «правила — как у владельца»: 2 кита за 2 часа, у каждого от
1000 $; не дороже средней китов больше чем на 0,3 %; кросс, плечо 10×, маржа
10 %; **одна открытая позиция**; в одну бумагу — раз в 3 часа; выход —
только тейк 1 %, без стопа и срока.

## 5. Сколько сделок в день — без сети, несколько минут

Путь к базе Hyperliquid — `WHALE_HL_DB` в `systemctl cat whale-api` (вывести
только этот путь, не другие значения).

```bash
DB=~/WhaleScanner/hyperliquid.db
time nice -n 19 python3 "$T/ws/tools/autotrade_backtest.py" --db "$DB" --out "$T/cnt" --cache "$T/cache" --count-only
```

Только чтение базы. Считает **все рынки** — крипту, акции, металлы: это
правило владельца (`--markets all` по умолчанию); только акции — добавить
`--markets stocks`. На базе в ~1,2 млн ордеров — несколько минут (с nice 19
дольше, если сервер занят); дольше 15 минут — доложить. Строку «в среднем …
в день» и таблицу по правилам — владельцу. Это верхняя граница: позиция
одна — пока она не дошла до тейка, новые не открываются.

Полный бэктест руками **не запускать**: его считает API (шаг 6), второй
прогон рядом — лишняя память и запросы к Hyperliquid.

## 6. Ночной пересчёт, кнопки и итог во вкладке

```bash
D=~/WalletTracker/autotrade_data
ls -la "$D" "$D/latest" 2>/dev/null
cat "$D/switch.json" 2>/dev/null || echo "switch.json нет — модуль выключен («Старт» не нажимали)"
tail -n 20 "$D/last_run.log" 2>/dev/null
cat "$D/backtest_status.json" 2>/dev/null           # running, started, finished, error, next
journalctl -u whale-api --since "-2 day" --no-pager | grep '\[AUTO\]' | tail -n 20
journalctl -u whale-autotrade --since "-1 day" --no-pager | grep -v -iE 'key|secret|pass' | tail -n 30
journalctl -k --since "-1 day" --no-pager | grep -iE 'oom|killed process' | tail -n 5   # ожидается пусто
pgrep -af autotrade_backtest || echo "пересчёт сейчас не идёт"
for p in $(pgrep -f autotrade_backtest); do ps -o pid,ni,rss,etime,cmd -p "$p"; done   # NI 19, RSS < 1 ГБ
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
f, z = s.get("full") or {}, s["sizing"]
print("посчитан:", time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(s["generated_at"])))
print("вердикт:", "можно дальше" if s["verdict"] else "нет")
print("размер:", z["deposit"], "$, маржа", z["margin_pct"], "% ×", z["leverage"], ", позиций сразу", z["max_open"],
      ", кросс" if z["cross"] else ", изолированная")
print("сигналов:", s["data"]["signals"], "| свечи:", s["data"]["candles"], "| запросов:", s["data"]["requests"],
      "| сбоев:", s["data"]["errors"])
print("общая: сделок", f.get("trades"), "| винрейт", f.get("win_pct"), "% | итог", f.get("net"), "$ | PF", f.get("pf"),
      "| просадка", f.get("max_dd"), "$ | дольше всех", f.get("longest_h"), "ч | сделок в день", s.get("per_day"))
best = s["top"][0] if s["top"] else None
if best:
    print("лучший набор:", best["params"])
    print("на отложенных:", best["holdout"], "— проходит" if best["passes"] else "— не проходит")
print("против китов:", s.get("fade"))
print("активные позиции:", s.get("active") if s.get("active") else "нет")
EOF
```

Проверить: `max_open` = 1; у лучшего набора `cooldown` 180, `chase` 0,3,
`tp` 1, `sl` 0. Если нет `per_day` и `active` — итог посчитан старой
версией: нужен «Пересчитать сейчас» во вкладке (это делает владелец).

Отчёт целиком — `cat "$D/latest/report.txt"`.

## 7. Закрытость вкладки

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8090/api/owner/autotrade               # без подписи
curl -s -o /dev/null -w '%{http_code}\n' -X POST http://127.0.0.1:8090/api/owner/autotrade/start   # без подписи
```

Ожидается **не 200** (401, 403 или 404). Ключ API не подставлять и не
читать. Что владельцу отвечает 200, а чужому с верной подписью — 404 и
строка `[AUTO] чужой запрос`, проверяют тесты шага 3.

## 8. Торговли нет, ключей нет, сеть — только публичная

```bash
cd ~/WhaleScanner
grep -nE '"/auto(trade)?[" ]' *.cpp || echo "команд /auto в боте нет — этапы 2–3 не построены"   # /autorenew, /autobase — не то
grep -rnE 'whale-autotrade\.env|\bAUTOTRADE_|agent_?wallet|api_?secret|passphrase' --include=*.cpp --include=*.h --include=*.py . ~/WalletTracker/wt_autotrade.py || echo "ключей и их загрузки в коде нет"   # WT_AUTOTRADE_DIR — путь, не ключ
grep -noE 'https://[a-z0-9./-]+' tools/autotrade_backtest.py | sort -u   # только api.hyperliquid.xyz и www.okx.com
grep -nE '/api/v5/(trade|account|asset)' tools/autotrade_backtest.py || echo "торговых и счётных адресов OKX нет"
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
3. Тесты: бэктест (34), объём (пик памяти), модуль API (16) — прошли ли.
4. Правила в коде — «как у владельца» или расхождения.
5. `--count-only`: сделок в день в среднем и за последние 14 дней.
6. Ночной пересчёт: включён ли модуль, когда был последний, идёт ли сейчас
   (память, приоритет), сбои из `last_run.log` и строки `[AUTO]`.
7. Итог во вкладке: вердикт; общая статистика (сделок, винрейт, итог, PF,
   просадка, самая долгая сделка, сделок в день); лучший набор и
   отложенные; «против китов»; активные позиции.
8. Вкладка закрыта (коды ответа без подписи); чужие запросы — есть или нет.
9. Торговых команд, ключей и торговых адресов нет; сеть — только публичные
   Hyperliquid и OKX; в каталоге бота файлов бэктеста нет.
10. Что не так — отдельным списком, с точной строкой из журнала.
