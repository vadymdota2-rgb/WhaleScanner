#!/usr/bin/env bash
# Проверки бота, которым не нужна сеть: транзакции базы биткоина, кнопки по
# типу чата и оплата Премиума. Журнал бота во время проверки — в файл: на
# экран он выходит, только если проверка провалилась.
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
CXX="${CXX:-g++}"
FLAGS=(-std=c++20 -O0 -pthread -I. -Wall -Wextra -Werror ${SANITIZE:+-fsanitize=address,undefined -fno-sanitize-recover=all -g})

run() {
    local name="$1"
    if ! "$out/$name" >"$out/$name.log" 2>&1; then
        cat "$out/$name.log"
        echo "ПРОВАЛ: $name" >&2
        exit 1
    fi
    tail -n 1 "$out/$name.log"
}

"$CXX" "${FLAGS[@]}" tests/btc_tx_test.cpp btc_etf.cpp -lcurl -lsqlite3 -o "$out/btc_tx_test"
run btc_tx_test
"$CXX" "${FLAGS[@]}" tests/markup_test.cpp -o "$out/markup_test"
run markup_test
"$CXX" "${FLAGS[@]}" tests/premium_test.cpp premium.cpp utils.cpp ru.cpp translations.cpp -lsqlite3 -o "$out/premium_test"
run premium_test
