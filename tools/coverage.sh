#!/usr/bin/env bash
# © 2026 Wallet Tracker. Все права защищены — см. LICENSE.
# Покрытие файлов оплаты тестами: 100 % строк, ветвлений, функций и областей
# у premium.cpp и premium_logic.h (clang + llvm-cov — ветвления считаются по
# исходнику, без скрытых веток исключений, которые добавляет gcov). Ниже 100 %
# по любой мере — выход 1 и список непокрытого.
#
#   bash tools/coverage.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
clang++ -std=c++20 -O0 -fprofile-instr-generate -fcoverage-mapping -pthread -I. \
    tests/premium_test.cpp premium.cpp utils.cpp ru.cpp translations.cpp -lsqlite3 -o "$out/t"
if ! LLVM_PROFILE_FILE="$out/p.profraw" "$out/t" >"$out/log" 2>&1; then
    cat "$out/log"
    echo "ПРОВАЛ: тесты не прошли — покрытие не считаю" >&2
    exit 1
fi
llvm-profdata merge -sparse "$out/p.profraw" -o "$out/p.profdata"
llvm-cov export "$out/t" -instr-profile="$out/p.profdata" -summary-only premium.cpp premium_logic.h >"$out/sum.json"
status=0
python3 - "$out/sum.json" <<'EOF' || status=$?
import json
import sys

files = json.load(open(sys.argv[1]))["data"][0]["files"]
bad = False
for f in files:
    name = f["filename"].rsplit("/", 1)[-1]
    if name not in ("premium.cpp", "premium_logic.h"):
        continue
    s = f["summary"]
    parts = []
    for k, title in (("lines", "строк"), ("branches", "ветвлений"), ("functions", "функций"), ("regions", "областей")):
        c, n = s[k]["covered"], s[k]["count"]
        parts.append(f"{title} {c}/{n}")
        bad |= c != n
    print(f"{name}: " + ", ".join(parts))
sys.exit(1 if bad else 0)
EOF
if [ "$status" -ne 0 ]; then
    llvm-cov show "$out/t" -instr-profile="$out/p.profdata" premium.cpp premium_logic.h -show-branches=count \
        -use-color=false | grep -E "^ +[0-9]+\| +0\||Branch \([0-9]+:[0-9]+\): \[(True: 0|.*False: 0\])" | head -40
    echo "ПРОВАЛ: покрытие ниже 100 %" >&2
    exit 1
fi
echo "покрытие: 100 % строк, ветвлений, функций и областей"
