#!/usr/bin/env bash
# Покрытие файлов оплаты тестами (gcov): каждая строка premium.cpp и
# premium_logic.h должна выполниться хотя бы раз. Ниже 100 % — выход 1 и
# список невыполненных строк. Ветвления печатаются для сведения: у C++
# компилятор добавляет скрытые ветви исключений, и 100 % по ним — шум.
#
#   bash tools/coverage.sh
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -std=c++20 -O0 -g --coverage -pthread -I. tests/premium_test.cpp premium.cpp utils.cpp ru.cpp translations.cpp \
    -lsqlite3 -o "$out/t"
if ! "$out/t" >"$out/log" 2>&1; then
    cat "$out/log"
    echo "ПРОВАЛ: тесты не прошли — покрытие не считаю" >&2
    exit 1
fi
fail=0
report() {  # report <gcda> <исходник>
    local gcda="$1" src="$2"
    (cd "$out" && gcov -b -o "$out" "$gcda" >"$out/gcov.txt" 2>/dev/null) || true
    local file="$out/$(basename "$src").gcov"
    # gcov кладёт отчёт в текущий каталог; исходник ищет от корня.
    (cd "$root" && gcov -b -o "$out" "$out/$gcda" >"$out/gcov.txt" 2>/dev/null && mv -f ./*.gcov "$out/" 2>/dev/null) || true
    local lines total
    total=$(grep -cE '^ +([0-9#]+\*?|=====):' "$file" || true)
    lines=$(grep -E '^ +#####:' "$file" || true)
    echo "$src: строк с кодом $total, не выполнено $(printf '%s' "$lines" | grep -c . || true)"
    grep -A3 "File '.*$(basename "$src")'" "$out/gcov.txt" | grep -E "Branches|Taken" | sed 's/^/  /'
    if [ -n "$lines" ]; then
        printf '%s\n' "$lines" | sed 's/^/  НЕ ВЫПОЛНЕНО /'
        fail=1
    fi
}
report t-premium.gcda premium.cpp
report t-premium_test.gcda premium_logic.h
if [ "$fail" -ne 0 ]; then
    echo "ПРОВАЛ: покрытие строк ниже 100 %" >&2
    exit 1
fi
echo "покрытие строк: 100 %"
