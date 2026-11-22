# WhaleScanner — бот Wallet Tracker

Telegram-бот приложения Wallet Tracker: следит за кошельками в блокчейнах,
шлёт алерты, принимает оплату Премиума. Меню, кнопок и диалогов в чате нет —
всё в приложении (репозиторий Wallet-Tracker); бот отвечает кнопкой
«Открыть приложение», рассылает алерты и отвечает владельцу на служебные
команды.

## Сборка и запуск

```
./build.sh                                  # все .cpp каталога → whale_bot
sudo systemctl restart whalescanner.service
```

На сервере это делает автовыкат: новый коммит в `main` — сборка и
перезапуск. Нужны g++ (C++20), libcurl, SQLite, Boost (заголовки).

## Проверки

```
bash tests/run.sh                  # без сети: несколько минут
SANITIZE=1 bash tests/run.sh       # под ASan и UBSan (так в CI)
CXX=clang++ SANITIZE=1 bash tests/run.sh
FULL=1 bash tests/run.sh           # ещё clang-tidy, фаззинг, покрытие 100 %, миллион человек
python3 tools/mutate.py            # мутационная проверка оплаты (~30 минут)
```

CI (`.github/workflows/tests.yml`) гоняет всё это на каждый push: сборку
всего бота двумя компиляторами с предупреждениями-ошибками, проверки под
санитайзерами, clang-tidy, фаззинг, покрытие и мутации.

## Модули после ревизии

| Модуль | Спецификация |
|---|---|
| Оплата Премиума: звёзды Telegram и USD₮ в сети TON, выдача и окончание срока, ручная выдача `/grant` | [`docs/premium.md`](docs/premium.md) |

Остальные файлы проходят ревизию по очереди, тем же порядком.

## Автокопирование китов (закрытое, только для владельца)

Реальная торговля на OKX — служба [`tools/autotrade_live.py`](tools/autotrade_live.py):
от 2 китов Hyperliquid в одну сторону за 2 часа (каждый от 1000 $), против —
не крупнее по объёму без плеча; маржа 5 % × 10×, одна позиция; тейк +1 %, выход вслед за китом в
убытке, разовое усреднение. Бэктест — повтор истории по тем же правилам:
[`tools/autotrade_replay.py`](tools/autotrade_replay.py) — с вариантами выхода
на тех же сигналах («вместе с китами», тейк или раньше с китом, без
усреднения) для сравнения; общее —
[`tools/autotrade_core.py`](tools/autotrade_core.py). База бота — только
чтение. Устройство — [`docs/autotrade-how-it-works.md`](docs/autotrade-how-it-works.md),
проверка на сервере — [`docs/check-autotrade-server.md`](docs/check-autotrade-server.md).

## Авторские права

Wallet Tracker, все права защищены — [`LICENSE`](LICENSE). Сторонний код —
под своими лицензиями, тексты — в [`LICENSES/`](LICENSES).
