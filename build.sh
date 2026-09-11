#!/usr/bin/env bash
# Сборка бота.
#
# Списка файлов здесь нет намеренно: стоит добавить в проект новый .cpp и
# забыть дописать его в команду — линковщик выдаёт сотню строк «undefined
# reference», по которым не сразу видно, что не хватает всего лишь файла.
# Поэтому берём все .cpp каталога.
set -euo pipefail
cd "$(dirname "$0")"

OUT="${1:-whale_bot}"
mapfile -t SRC < <(ls -1 ./*.cpp | sort)

echo "Собираю $OUT из ${#SRC[@]} файлов (это несколько минут)..."
g++ -std=c++20 -O2 -pthread "${SRC[@]}" -lcurl -lsqlite3 -o "$OUT"
echo "Готово: $OUT"
echo "Перезапуск: sudo systemctl restart whalescanner.service"
