#!/usr/bin/env python3
"""Мутационная проверка тестов оплаты: портим код по одной мелочи и смотрим,
заметит ли tests/premium_test.cpp.

Каждая мутация — одна замена в одной строке premium_logic.h или premium.cpp:
сравнение (<, <=, ==, !=…), логика (&&, ||, !), истина/ложь, число на
единицу больше, «return false» вместо «return true». Если с порчей тесты
всё равно проходят — мутант «выжил»: этой строки тесты на деле не
проверяют. Выжившие печатаются; заведомо равносильные замены (поведение не
меняется — например, строка журнала) перечислены в EQUIV с причиной.

    python3 tools/mutate.py            # все мутанты (~20 минут на 4 ядрах)
    python3 tools/mutate.py --list     # только перечислить
    python3 tools/mutate.py -k extendTerm  # только строки с этим текстом

Исходники не трогаются: мутант собирается из копии во временном каталоге.
"""
import argparse
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGETS = ["premium_logic.h", "premium.cpp"]
STATIC = ["utils.cpp", "ru.cpp", "translations.cpp"]
CXX = os.environ.get("CXX", "g++")
FLAGS = ["-std=c++20", "-O0", "-pthread", "-w"]

# Строка (без пробелов по краям) и замена, которые ничего не меняют в
# поведении, — с причиной. Сверяются по файлу, тексту строки и замене.
EQUIV: dict = {
    ('premium.cpp', 'return false;', 'return false;→return true;@0'):
        'сбой подготовки запроса к своей же таблице не воспроизводится',
    ('premium.cpp', 'bool found = false;', 'false→true@13'):
        'строки нет — UPDATE ниже меняет 0 строк, и продление всё равно отказ (changes == 1)',
    ('premium.cpp', 'if (err) sqlite3_free(err);', 'if (→if (!@0'):
        'освобождение строки ошибки',
    ('premium.cpp', 'sub_until INTEGER NOT NULL DEFAULT 0', '0→1@35'):
        'бот и API всегда пишут столбец явно',
    ('premium.cpp', 'paid_at INTEGER NOT NULL DEFAULT 0,', '0→1@33'):
        'бот и API всегда пишут столбец явно',
    ('premium.cpp', 'days INTEGER NOT NULL DEFAULT 30', '30→31@30'):
        'API всегда пишет срок счёта явно (pay_usdt)',
    ('premium.cpp', 'if (perr) sqlite3_free(perr);', 'if (→if (!@0'):
        'освобождение строки ошибки',
    ('premium.cpp', 'if (sqlite3_exec(db, "ALTER TABLE premium_payments ADD COLUMN sub_until INTEGER NOT NULL DEFAULT 0",', 'if (→if (!@0'):
        'только строка журнала',
    ('premium.cpp', 'nullptr, nullptr, &aerr) == SQLITE_OK)', '==→!=@24'):
        'только строка журнала',
    ('premium.cpp', 'if (aerr) sqlite3_free(aerr);', 'if (→if (!@0'):
        'освобождение строки ошибки',
    ('premium.cpp', 'if (sqlite3_exec(db, "ALTER TABLE ton_invoices ADD COLUMN days INTEGER NOT NULL DEFAULT 30",', 'if (→if (!@0'):
        'только строка журнала',
    ('premium.cpp', 'nullptr, nullptr, &aerr) == SQLITE_OK)', '==→!=@24'):
        'только строка журнала',
    ('premium.cpp', 'if (aerr) sqlite3_free(aerr);', 'if (→if (!@0'):
        'освобождение строки ошибки',
    ('premium.cpp', 'if (ok && !g_serviceChatId.empty()) {', ' && → || @6'):
        'при неудачной миграции UPDATE снятия отметки всё равно ничего не меняет',
    ('premium.cpp', 'if (ok) {', 'if (→if (!@0'):
        'только строка журнала',
    ('premium.cpp', '<< STAR_PLANS[0].days << " days, " << STAR_PLANS[1].stars << " / " << STAR_PLANS[1].days', '1→2@49'):
        'только строка журнала',
    ('premium.cpp', '<< STAR_PLANS[0].days << " days, " << STAR_PLANS[1].stars << " / " << STAR_PLANS[1].days', '1→2@81'):
        'только строка журнала',
    ('premium.cpp', 'if (chatIds.empty() || !g_premiumSchemaOk) return out;', ' || → && @19'):
        'пустой список и так даёт пустой ответ: цикл по кускам не начнётся',
    ('premium.cpp', 'constexpr size_t CHUNK = 500;', '500→501@25'):
        'размер куска — любой до предела SQLite',
    ('premium.cpp', 'for (size_t from = 0; from < chatIds.size(); from += CHUNK) {', ' < → <= @26'):
        'пустой хвост — запрос IN () без строк',
    ('premium.cpp', 'for (size_t i = 0; i < n; i++) sql += (i ? ",?" : "?");', ' < → <= @20'):
        'лишний «?» остаётся NULL и ни с чем не совпадает',
    ('premium.cpp', 'for (size_t i = 0; i < n; i++)', ' < → <= @20'):
        'лишний параметр SQLite отклоняет (SQLITE_RANGE); чтение за концом списка ловит ASan в tests/run.sh',
    ('premium.cpp', 'int flag = 0; long long start = 0, expire = 0;', '0→1@32'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'int flag = 0; long long start = 0, expire = 0;', '0→1@11'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'int flag = 0; long long start = 0, expire = 0;', '0→1@44'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'return flag != 0 && expire > now;', ' > → >= @26'):
        'граница в одну секунду: срок истекает «сейчас» — тест на часах ненадёжен',
    ('premium.cpp', 'int changed = 0;', '0→1@14'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'if (changed > 0) {', ' > → >= @11'):
        'ноль истёкших — те же пустые циклы',
    ('premium.cpp', 'int flag = 0; long long start = 0, expire = 0;', '0→1@11'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'int flag = 0; long long start = 0, expire = 0;', '0→1@32'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'int flag = 0; long long start = 0, expire = 0;', '0→1@44'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'return false;', 'return false;→return true;@0'):
        'сбой подготовки запроса к своей же таблице не воспроизводится',
    ('premium.cpp', 'return rc == SQLITE_DONE && sqlite3_changes(db) == 1;', ' && → || @24'):
        'шаг UPDATE без ошибки — changes() решает сам',
    ('premium.cpp', 'return false;', 'return false;→return true;@0'):
        'сбой COMMIT после удачного BEGIN IMMEDIATE не воспроизводится в памяти',
    ('premium.cpp', 'static bool said = false;', 'false→true@19'):
        'только строка журнала',
    ('premium.cpp', 'if (!said) std::cerr << "[USDT] адрес кошелька приёма не разобран — оплату USD₮ не проверяю" << std::endl;', 'if (!→if (@0'):
        'только строка журнала',
    ('premium.cpp', 'said = true;', 'true→false@7'):
        'только строка журнала',
    ('premium.cpp', 'bool anyActive = false;', 'false→true@17'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'sqlite3_bind_int64(s, 1, now - 86400);', '86400→86401@31'):
        'граница в одну секунду',
    ('premium.cpp', 'sqlite3_bind_int64(s, 1, now - 86400);', '86400→86401@31'):
        'граница в одну секунду',
    ('premium.cpp', 'const std::string resp = http(url, "", 15);', '15→16@39'):
        'тайм-аут сети',
    ('premium.cpp', 'if (!j.is_object() || !j.contains("jetton_transfers") || !j["jetton_transfers"].is_array())', ' || → && @18'):
        'не-массив обходится как один элемент, и usdtTransfer его отклоняет',
    ('premium.cpp', 'if (!j.is_object() || !j.contains("jetton_transfers") || !j["jetton_transfers"].is_array())', ' || → && @53'):
        'не-массив обходится как один элемент, и usdtTransfer его отклоняет',
    ('premium.cpp', 'long long need = 0;', '0→1@17'):
        'без строки счёта chatId пуст — проверка ниже отсекает',
    ('premium.cpp', 'if (d == 30 || d == 365) days = d;', 'if (→if (!@0'):
        '«!d == 30» всегда ложно: 30 и так по умолчанию, остальное — тоже 30',
    ('premium.cpp', 'if (underpaidSeen.size() > 1000) underpaidSeen.clear();', ' > → >= @24'):
        'только строка журнала',
    ('premium.cpp', 'if (underpaidSeen.size() > 1000) underpaidSeen.clear();', 'if (→if (!@0'):
        'только строка журнала',
    ('premium.cpp', 'if (underpaidSeen.size() > 1000) underpaidSeen.clear();', '1000→1001@27'):
        'только строка журнала',
    ('premium.cpp', 'if (underpaidSeen.insert(hash).second)', 'if (→if (!@0'):
        'только строка журнала',
    ('premium.cpp', 'bool granted = false;', 'false→true@15'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', '"WHERE memo=? AND status=\'active\' AND kind=\'usdt\'")) return false;', 'return false;→return true;@53'):
        'сбой подготовки запроса к своей же таблице не воспроизводится',
    ('premium.cpp', 'if (rc != SQLITE_DONE) return false;', 'return false;→return true;@23'):
        'сбой шага UPDATE не воспроизводится',
    ('premium.cpp', 'if (sqlite3_changes(db) != 1) { taken = true; return false; }', 'return false;→return true;@46'):
        'счёт заняли между SELECT и UPDATE — гонка двух опросов, опрос идёт из одного потока',
    ('premium.cpp', 'if (sqlite3_changes(db) != 1) { taken = true; return false; }', 'true→false@40'):
        'счёт заняли между SELECT и UPDATE — гонка двух опросов, опрос идёт из одного потока',
    ('premium.cpp', 'bool wasActive = false;', 'false→true@17'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'char day[16];', '16→17@9'):
        'размер буфера с запасом',
    ('premium.cpp', 'if (chatId.empty() || days <= 0 || days > premium::MAX_GRANT_DAYS) return false;', ' <= → < @26'):
        'второй рубеж: extendTerm отклоняет те же дни',
    ('premium.cpp', 'if (chatId.empty() || days <= 0 || days > premium::MAX_GRANT_DAYS) return false;', ' || → && @31'):
        'второй рубеж: extendTerm отклоняет те же дни',
    ('premium.cpp', 'bool ok = false;', 'false→true@10'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'bool wasActive = false;', 'false→true@17'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'long long amount = 0;', '0→1@19'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'long long subUntil = 0;', '0→1@21'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'if (rc == SQLITE_CONSTRAINT && extendedRc == SQLITE_CONSTRAINT_UNIQUE) {', ' && → || @27'):
        'при успешном шаге расширенный код — не UNIQUE',
    ('premium.cpp', 'bool wasAlreadyActive = false;', 'false→true@24'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'long long amount = 0;', '0→1@19'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'parseOk = false;', 'false→true@10'):
        'при битом поле нагрузка пуста — тариф не найдётся',
    ('premium.cpp', 'auto r = http(apiUrl("answerPreCheckoutQuery"), a.dump(), 8);', '8→9@58'):
        'тайм-аут сети',
    ('premium.cpp', 'if (!p.value("ok", false)) {', 'if (!→if (@0'):
        'только строка журнала',
    ('premium.cpp', 'if (!p.value("ok", false)) {', 'false→true@19'):
        'только строка журнала',
    ('premium.cpp', '<< " amount=" << (sp.is_object() ? sp.value("total_amount", 0LL) : 0LL)', '0→1@60'):
        'только строка журнала',
    ('premium.cpp', '<< " amount=" << (sp.is_object() ? sp.value("total_amount", 0LL) : 0LL)', '0→1@67'):
        'только строка журнала',
    ('premium.cpp', '? withDays(tr(lang, "payment_renewed"), plan ? plan->days : 30)', '30→31@60'):
        'тариф после applySuccessfulPayment известен всегда',
    ('premium.cpp', 'withDays(tr(lang, "payment_success_duration"), plan ? plan->days : 30);', '30→31@67'):
        'тариф после applySuccessfulPayment известен всегда',
    ('premium.cpp', 'if (!sendMsg(chatId, text, openAppKeyboard(lang)).ok)', 'if (!→if (@0'):
        'только строка журнала',
    ('premium.cpp', 'auto r = nlohmann::json::parse(http(tgApi("editUserStarSubscription"), j.dump(), 15));', '15→16@81'):
        'тайм-аут сети',
    ('premium.cpp', 'if (!done) std::cerr << "[PREMIUM] cancel subscription " << sub.chat << ": "', 'if (!→if (@0'):
        'только строка журнала',
    ('premium.cpp', 'std::this_thread::sleep_for(std::chrono::milliseconds(60));', '60→61@54'):
        'пауза между вызовами Telegram',
    ('premium_logic.h', 'if (a == std::string::npos) return false;', 'if (→if (!@0'):
        '«!a == npos» всегда ложно; без двоеточия строка дальше не пройдёт strictInt',
    ('premium_logic.h', "const size_t b = payload.find(':', a + 1);", '1→2@39'):
        'пустой номер чата отклоняет strictInt',
    ('premium_logic.h', 'if (b == std::string::npos) return false;', 'if (→if (!@0'):
        '«!b == npos» всегда ложно; без второго двоеточия строка не пройдёт strictInt',
    ('premium_logic.h', 'long long id = 0;', '0→1@15'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', 'long long until = 0;', '0→1@18'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', 'if (!introBinding(payload, chat, until) || buyer.empty() || chat != buyer ||', ' || → && @39'):
        'пустой покупатель и так отклонён; номер чата в нагрузке не бывает пустым',
    ('premium_logic.h', 'long long wc = 0;', '0→1@15'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', 'std::array<uint8_t, 36> b{};', '36→37@20'):
        'размер массива с запасом',
    ('premium_logic.h', 'uint32_t acc = 0;', '0→1@15'):
        'начальная единица уходит за пределы байта (& 0xFF)',
    ('premium_logic.h', 'long long units = 0;', '0→1@18'):
        'начальное значение перезаписывается до чтения',
}


def code_spans(line: str) -> list[tuple[int, int]]:
    """Части строки вне строковых литералов и вне комментария «//»."""
    spans, start, i, q = [], 0, 0, ""
    while i < len(line):
        c = line[i]
        if q:
            if c == "\\":
                i += 2
                continue
            if c == q:
                q, start = "", i + 1
        elif c in "\"'":
            spans.append((start, i))
            q = c
        elif line.startswith("//", i):
            spans.append((start, i))
            return spans
        i += 1
    if not q:
        spans.append((start, len(line)))
    return spans


OPS = [
    (" <= ", " < "), (" >= ", " > "), (" < ", " <= "), (" > ", " >= "),
    (" == ", " != "), (" != ", " == "), (" && ", " || "), (" || ", " && "),
    ("return true;", "return false;"), ("return false;", "return true;"),
]


def mutants(path: str, root: str) -> list[tuple[int, str, str, str]]:
    """(номер строки, новая строка, ярлык, исходная строка без пробелов)."""
    out = []
    in_block = False
    lines = open(os.path.join(root, path), encoding="utf-8").read().split("\n")
    for n, line in enumerate(lines):
        s = line.strip()
        if in_block:
            in_block = "*/" not in s
            continue
        if s.startswith("/*"):
            in_block = "*/" not in s
            continue
        if not s or s.startswith(("//", "#", "*")) or "std::cout" in s or "std::cerr <<" in s and not s.startswith("if"):
            continue
        seen = set()

        indent = len(line) - len(line.lstrip())

        def add(a: int, b: int, rep: str, label: str) -> None:
            new = line[:a] + rep + line[b:]
            if new != line and new not in seen:
                seen.add(new)
                out.append((n, new, f"{label}@{a - indent}", s))

        for a, b in code_spans(line):
            seg = line[a:b]
            for old, new in OPS:
                for m in re.finditer(re.escape(old), seg):
                    add(a + m.start(), a + m.end(), new, f"{old}→{new}".replace("  ", " ") if old.strip() in "<=>=!=&&||" else f"{old.strip()}→{new.strip()}")
            for m in re.finditer(r"\b(true|false)\b", seg):
                if "return" in seg[: m.start()]:
                    continue  # «return true/false» уже есть выше
                add(a + m.start(), a + m.end(), "false" if m.group(1) == "true" else "true", f"{m.group(1)}→{'false' if m.group(1) == 'true' else 'true'}")
            for m in re.finditer(r"(?<![\w.\-])(\d+)(?:LL|ULL)?\b(?!\.)", seg):
                if m.group(1) == "0" and "[" in seg[max(0, m.start() - 1): m.start()]:
                    continue
                v = int(m.group(1))
                add(a + m.start(1), a + m.end(1), str(v + 1), f"{v}→{v + 1}")
            for m in re.finditer(r"(if \(|while \(|&& |\|\| |return )!(?=[\w(])", seg):
                add(a + m.start(), a + m.end(), m.group(1), f"{m.group(1)}!→{m.group(1)}".strip())
            for m in re.finditer(r"if \((?!!)", seg):
                add(a + m.start(), a + m.end(), "if (!", "if (→if (!")
            # «!x» на «x» в начале условия, когда «!» стоит у имени.
            for m in re.finditer(r"!(\w+)", seg):
                if seg[max(0, m.start() - 4): m.start()].endswith(("if (", "&& ", "|| ", "rn ")):
                    continue
                add(a + m.start(), a + m.end(), m.group(1), f"!{m.group(1)}→{m.group(1)}")
    return out


def build(work: str, objs: dict, log: list, test_o: str = "") -> bool:
    """Собрать проверку в work; test_o — готовый объект теста (мутант в
    premium.cpp его не меняет, и пересобирать тяжёлый тест незачем)."""
    if test_o:
        shutil.copy(test_o, os.path.join(work, "premium_test.cpp.o"))
    for src in (["premium.cpp"] if test_o else ["tests/premium_test.cpp", "premium.cpp"]):
        o = os.path.join(work, os.path.basename(src) + ".o")
        r = subprocess.run([CXX, *FLAGS, "-I", work, "-c", os.path.join(work, src), "-o", o],
                           capture_output=True, text=True)
        if r.returncode:
            log.append(r.stderr[-500:])
            return False
    r = subprocess.run([CXX, "-pthread", os.path.join(work, "premium_test.cpp.o"), os.path.join(work, "premium.cpp.o"),
                        *objs.values(), "-lsqlite3", "-o", os.path.join(work, "t")], capture_output=True, text=True)
    return r.returncode == 0


def setup(dst: str, src: str = ROOT) -> None:
    """Копия исходников из src. Мутанты берут её из снимка, сделанного при
    запуске: правка рабочих файлов во время прогона его не портит."""
    os.makedirs(os.path.join(dst, "tests"), exist_ok=True)
    for f in os.listdir(src):
        if f.endswith((".h", ".hpp", ".cpp")):
            shutil.copy(os.path.join(src, f), dst)
    shutil.copy(os.path.join(src, "tests", "premium_test.cpp"), os.path.join(dst, "tests"))


def run_one(args) -> tuple[str, str]:
    path, n, new, label, objs, test_o, base = args
    with tempfile.TemporaryDirectory() as work:
        setup(work, base)
        p = os.path.join(work, path)
        lines = open(p, encoding="utf-8").read().split("\n")
        lines[n] = new
        open(p, "w", encoding="utf-8").write("\n".join(lines))
        log: list = []
        if not build(work, objs, log, test_o if path == "premium.cpp" else ""):
            return "build", ""
        try:
            r = subprocess.run([os.path.join(work, "t")], capture_output=True, text=True, timeout=120,
                               env={**os.environ, "TON_WALLET_ADDRESS": ""})
        except subprocess.TimeoutExpired:
            return "killed", "timeout"
        return ("killed" if r.returncode else "survived"), ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-k", default="")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
    a = ap.parse_args()
    def collect(root: str) -> list:
        todo = []
        for path in TARGETS:
            for n, new, label, src in mutants(path, root):
                if a.k and a.k not in src:
                    continue
                if (path, src, label) in EQUIV:
                    continue
                todo.append((path, n, new, label, src))
        return todo

    todo = collect(ROOT)
    # Запись в EQUIV, которой больше нет в коде, — устаревшая: строку
    # переписали, и мутант снова надо проверять, а не молча пропускать.
    known = {(p, src, label) for p in TARGETS for _, _, label, src in mutants(p, ROOT)}
    stale = [k for k in EQUIV if k not in known]
    for k in stale:
        print("УСТАРЕЛО в EQUIV:", k, file=sys.stderr)
    if stale:
        return 2
    if a.list:
        for path, n, _, label, src in todo:
            print(f"{path}:{n + 1}: {label}  | {src[:90]}")
        print(len(todo), "мутантов")
        return 0
    with tempfile.TemporaryDirectory() as base:
        setup(base)
        todo = collect(base)
        objs = {}
        for src in STATIC:
            o = os.path.join(base, src + ".o")
            subprocess.run([CXX, *FLAGS, "-c", os.path.join(base, src), "-o", o], check=True)
            objs[src] = o
        if not build(base, objs, []) or subprocess.run([os.path.join(base, "t")], capture_output=True,
                                                         env={**os.environ, "TON_WALLET_ADDRESS": ""}).returncode:
            print("исходный код не проходит тесты — мутировать нечего", file=sys.stderr)
            return 2
        res = {"killed": 0, "survived": 0, "build": 0}
        survivors = []
        with cf.ThreadPoolExecutor(a.j) as ex:
            futs = {ex.submit(run_one, (p, n, new, label, objs, os.path.join(base, "premium_test.cpp.o"), base)): (p, n, label, src) for p, n, new, label, src in todo}
            for i, f in enumerate(cf.as_completed(futs), 1):
                st, _ = f.result()
                res[st] += 1
                if st == "survived":
                    p, n, label, src = futs[f]
                    survivors.append(f"{p}:{n + 1}: {label}  | {src}")
                if i % 20 == 0:
                    print(f"… {i}/{len(todo)}", file=sys.stderr, flush=True)
    for s in sorted(survivors, key=lambda x: (x.split(":")[0], int(x.split(":")[1]))):
        print("ВЫЖИЛ", s)
    print(f"ИТОГ: убито {res['killed']}, выжило {res['survived']}, не собралось {res['build']}, из {len(todo)}")
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
