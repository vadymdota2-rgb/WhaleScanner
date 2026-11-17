#!/usr/bin/env bash
# Проверки бота, которым не нужна сеть: транзакции базы биткоина и кнопки по типу чата.
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -std=c++20 -O0 -pthread -I. tests/btc_tx_test.cpp btc_etf.cpp -lcurl -lsqlite3 -o "$out/btc_tx_test"
"$out/btc_tx_test"
g++ -std=c++20 -O0 -I. tests/markup_test.cpp -o "$out/markup_test"
"$out/markup_test"
