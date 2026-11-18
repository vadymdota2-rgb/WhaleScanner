#!/usr/bin/env bash
# Проверки бота, которым не нужна сеть: транзакции базы биткоина, кнопки по
# типу чата и оплата Премиума. Журнал бота во время проверки — в файл: на
# экран он выходит, только если проверка провалилась.
#
#   bash tests/run.sh                  # быстрые проверки
#   SANITIZE=1 bash tests/run.sh       # под ASan + UBSan (так в CI)
#   FULL=1 bash tests/run.sh           # ещё clang-tidy, фаззинг, покрытие 100 %
#   FUZZ_SECONDS=60 FULL=1 …           # фаззинг дольше (по умолчанию 20 с)
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
CXX="${CXX:-g++}"
FLAGS=(-std=c++20 -O0 -pthread -I. -Wall -Wextra -Werror ${SANITIZE:+-fsanitize=address,undefined -fno-sanitize-recover=all -g})
# Файлы оплаты — строже всего: любое неявное сужение числа, знака или тень
# имени — ошибка сборки, а не предупреждение.
STRICT=(-std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wuseless-cast
        -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wcast-qual -Wduplicated-cond -Wlogical-op -Werror)

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

g++ "${STRICT[@]}" -I. -fsyntax-only premium.cpp
g++ "${STRICT[@]}" -I. -fsyntax-only safe_http.cpp
# И clang со всеми предупреждениями сразу (-Weverything), кроме тех, что про
# совместимость с C++98, выравнивание и макросы curl. json.hpp — системным
# заголовком: его предупреждения — не наши. Для этого файлы модуля — в
# отдельный каталог, где json.hpp нет, а сам он — через -isystem.
if command -v clang++ >/dev/null; then
    mkdir -p "$out/every"
    cp premium.cpp premium.h premium_logic.h safe_http.cpp safe_http.h utils.h telegram.h ru.h lifecycle.h "$out/every/"
    for f in premium.cpp safe_http.cpp; do
        clang++ -std=c++20 -Weverything -Werror -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-padded \
            -Wno-unsafe-buffer-usage -Wno-exit-time-destructors -Wno-global-constructors -Wno-disabled-macro-expansion \
            -isystem . -I"$out/every" -fsyntax-only "$out/every/$f"
    done
fi
echo "premium: строгая сборка — без замечаний"
"$CXX" "${FLAGS[@]}" tests/premium_test.cpp premium.cpp utils.cpp ru.cpp translations.cpp -lsqlite3 -o "$out/premium_test"
run premium_test
python3 tests/premium_policy.py
python3 tests/safe_http_test.py

[ -n "${FULL:-}" ] || exit 0

clang-tidy --quiet premium.cpp safe_http.cpp -- -std=c++20 -I. >"$out/tidy.log" 2>&1 || {
    cat "$out/tidy.log"
    echo "ПРОВАЛ: clang-tidy" >&2
    exit 1
}
echo "premium: clang-tidy — без замечаний"

mkdir -p "$out/corpus"
cp tests/fuzz/corpus/* "$out/corpus/"
clang++ -std=c++20 -g -O1 -I. -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
    tests/fuzz/premium_fuzz.cpp -o "$out/premium_fuzz"
if ! "$out/premium_fuzz" -max_total_time="${FUZZ_SECONDS:-20}" -artifact_prefix="$out/" "$out/corpus" >"$out/fuzz.log" 2>&1; then
    tail -n 40 "$out/fuzz.log"
    echo "ПРОВАЛ: фаззинг (вход для повтора — crash-* выше)" >&2
    exit 1
fi
grep -E "^Done [0-9]+ runs" "$out/fuzz.log" | sed 's/^/premium: фаззинг — /'

bash tools/coverage.sh | tail -n 1
