#!/usr/bin/env bash
# Проверки бота, которым не нужна сеть: транзакции базы биткоина.
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -std=c++20 -O0 -pthread -I. tests/btc_tx_test.cpp btc_etf.cpp -lcurl -lsqlite3 -o "$out/btc_tx_test"
"$out/btc_tx_test"
