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
# Окружение проверки: кошелёк по умолчанию, образцы ответов — из репозитория.
TEST_ENV = {**os.environ, "TON_WALLET_ADDRESS": "", "FIXTURES": os.path.join(ROOT, "tests", "fixtures")}
FLAGS = ["-std=c++20", "-O0", "-pthread", "-w"]

# Строка (без пробелов по краям) и замена, которые ничего не меняют в
# поведении, — с причиной. Сверяются по файлу, тексту строки и замене.
EQUIV: dict = {
    ('premium.cpp', 'if (!open_) std::cerr << "[PREMIUM] " << what_ << ": BEGIN failed: " << logSafe(sqlite3_errmsg(db)) << std::endl;', 'if (!→if (@0'):
        'только журнал',
    ('premium.cpp', 'if (!open_) return false;', 'return false;→return true;@12'):
        'commit() зовётся только после проверки ok()',
    ('premium.cpp', 'if (!open_) return false; ⏎ if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) { ⏎ open_ = false;', 'false→true@8'):
        'после удачного COMMIT лишний ROLLBACK в деструкторе ничего не делает',
    ('premium.cpp', ' ⏎ struct PremiumRow { ⏎ int flag = 0;', '0→1@11'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'long long start = 0;', '0→1@18'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'long long expire = 0;', '0→1@19'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'return PremiumRow{s.int64(0) != 0 ? 1 : 0, s.int64(1), s.int64(2)};', '1→2@36'):
        'флаг сравнивается с нулём — 1 и 2 одинаковы',
    ('premium.cpp', 'return s.run() && sqlite3_changes(db) == 1;', ' && → || @14'):
        'строка человека прочитана выше — удачный UPDATE меняет ровно её',
    ('premium.cpp', 'if (e.find("duplicate column") != std::string::npos) return true;', 'if (→if (!@0'):
        '«!e.find(…)» истинно ровно когда сообщение начинается с «duplicate column» — так у SQLite и есть',
    ('premium.cpp', 'sub_until INTEGER NOT NULL DEFAULT 0', '0→1@35'):
        'бот и API всегда пишут столбец явно',
    ('premium.cpp', 'paid_at INTEGER NOT NULL DEFAULT 0,', '0→1@33'):
        'бот и API всегда пишут столбец явно',
    ('premium.cpp', 'days INTEGER NOT NULL DEFAULT 30', '30→31@30'):
        'API всегда пишет срок счёта явно (pay_usdt)',
    ('premium.cpp', ' ⏎ g_schemaOk = ok; ⏎ if (ok)', 'if (→if (!@0'):
        'только журнал',
    ('premium.cpp', '<< STAR_PLANS[0].days << " days, " << STAR_PLANS[1].stars << " / " << STAR_PLANS[1].days << ")"', '1→2@49'):
        'только журнал',
    ('premium.cpp', '<< STAR_PLANS[0].days << " days, " << STAR_PLANS[1].stars << " / " << STAR_PLANS[1].days << ")"', '1→2@81'):
        'только журнал',
    ('premium.cpp', 'if (chatIds.empty() || !g_schemaOk) return out;', ' || → && @19'):
        'до initPremium нет столбцов Премиума — запрос не подготовится, ответ тот же пустой',
    ('premium.cpp', 'constexpr size_t CHUNK = 500;', '500→501@25'):
        'размер куска — любой до предела SQLite',
    ('premium.cpp', 'for (size_t from = 0; from < chatIds.size(); from += CHUNK) {', ' < → <= @26'):
        'пустой хвост — запрос IN () без строк',
    ('premium.cpp', 'for (size_t i = 0; i < n; i++) sql += i != 0 ? ",?" : "?";', ' < → <= @20'):
        'лишний «?» остаётся NULL и ни с чем не совпадает',
    ('premium.cpp', 'for (size_t i = 0; i < n; i++) s.bind(static_cast<int>(i + 2), chatIds[from + i]);', ' < → <= @20'):
        'лишний параметр SQLite отклоняет (SQLITE_RANGE); чтение за концом списка ловит ASan в tests/run.sh',
    ('premium.cpp', 'if (chatId.empty() || days <= 0 || days > premium::MAX_GRANT_DAYS) return false;', ' <= → < @26'):
        'второй рубеж: extendTerm отклоняет те же дни',
    ('premium.cpp', 'if (chatId.empty() || days <= 0 || days > premium::MAX_GRANT_DAYS) return false;', ' || → && @31'):
        'второй рубеж: extendTerm отклоняет те же дни',
    ('premium.cpp', 'ensureUser(chatId); ⏎ const long long now = nowSec(); ⏎ bool ok = false;', 'false→true@10'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'bool wasActive = false;', 'false→true@17'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'long long until = 0;', '0→1@18'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'std::array<char, 32> when{};', '32→33@17'):
        'размер буфера с запасом',
    ('premium.cpp', 'if (std::strftime(when.data(), when.size(), "%Y-%m-%d %H:%M UTC", &u) == 0) when[0] = \'\\0\';', '0→1@73'):
        'strftime в буфер с запасом не ошибается',
    ('premium.cpp', 'std::array<char, 16> day{};', '16→17@17'):
        'размер буфера с запасом',
    ('premium.cpp', 'if (std::strftime(day.data(), day.size(), "%Y-%m-%d", &g) == 0) return;', '0→1@61'):
        'strftime в буфер с запасом не ошибается',
    ('premium.cpp', 'if (!s.run()) std::cerr << "[PREMIUM] funnel: " << logSafe(sqlite3_errmsg(db)) << std::endl;', 'if (!→if (@0'):
        'только журнал',
    ('premium.cpp', 'long long need = 0;', '0→1@17'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'int days = 30;  // месяц; год — только если так записано в счёте', '30→31@11'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'long long created = 0;', '0→1@20'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'if (said.first("wallet"))', 'if (→if (!@0'):
        'только журнал',
    ('premium.cpp', 'bool anyActive = false;', 'false→true@17'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'if (!expire.run()) std::cerr << "[USDT] истечение счетов: " << logSafe(sqlite3_errmsg(db)) << std::endl;', 'if (!→if (@0'):
        'только журнал',
    ('premium.cpp', 'const json j = json::parse(httpsCapped(url, "", 15, TONCENTER_MAX_BYTES, &err), nullptr, false);', '15→16@48'):
        'тайм-аут сети',
    ('premium.cpp', 'if (!inv || inv->chatId.empty() || inv->need < premium::USDT_MIN_UNITS) continue;', ' || → && @8'):
        'пустой optional дальше не разыменуется в исходном коде; в мутанте — UB, его ловит UBSan',
    ('premium.cpp', 'if (said.first("old:" + pay->hash))', 'if (→if (!@0'):
        'только журнал',
    ('premium.cpp', 'if (said.first("under:" + pay->hash))', 'if (→if (!@0'):
        'только журнал',
    ('premium.cpp', 'if (said.first("service:" + memo))', 'if (→if (!@0'):
        'только журнал',
    ('premium.cpp', 'bool taken = false;  // счёт успели занять раньше нас — не ошибка', 'false→true@13'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'bool reused = false;  // этот перевод уже оплатил другой счёт', 'false→true@14'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'bool granted = false;', 'false→true@15'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'if (said.first("reused:" + pay->hash))', 'if (→if (!@0'):
        'только журнал',
    ('premium.cpp', 'alertOwner("пришло " + premium::usdText(pay->units) + " USD₮ по счёту " + forOwner(memo, 64) + " (chat " +', '64→65@89'):
        'длина строки в сообщении владельцу',
    ('premium.cpp', 'forOwner(inv->chatId, 32) +', '32→33@22'):
        'длина строки в сообщении владельцу',
    ('premium.cpp', 'if (!sp.is_object()) return {Applied::Rejected, 0};', '0→1@48'):
        'срок у отказа не читается',
    ('premium.cpp', 'if (badString(sp, "invoice_payload") || badString(sp, "currency") ||', ' || → && @36'):
        'битая нагрузка или валюта и так отклоняются ниже: пустая строка не совпадёт с тарифом',
    ('premium.cpp', 'return {Applied::Rejected, 0};', '0→1@27'):
        'срок у отказа не читается',
    ('premium.cpp', 'if (rc != SQLITE_DONE && !duplicate)', ' != → == @6'):
        'только журнал',
    ('premium.cpp', 'if (rc != SQLITE_DONE && !duplicate)', ' && → || @21'):
        'только журнал',
    ('premium.cpp', 'if (rc != SQLITE_DONE && !duplicate)', '&& !→&&@22'):
        'только журнал',
    ('premium.cpp', 'if (rc != SQLITE_DONE && !duplicate)', 'if (→if (!@0'):
        'только журнал',
    ('premium.cpp', 'const bool parseOk = !badString(q, "invoice_payload") && !badString(q, "currency");', ' && → || @53'):
        'битое поле становится пустым — тариф и валюта не совпадут',
    ('premium.cpp', 'if (!tgAnswerOk(httpsCapped(tgApi("answerPreCheckoutQuery"), a.dump(), 8, TELEGRAM_MAX_BYTES, &why), why))', '8→9@71'):
        'тайм-аут сети',
    ('premium.cpp', 'const std::string charge = forOwner(premium::strField(sp, "telegram_payment_charge_id"), 80);', '80→81@89'):
        'длина строки в сообщении владельцу',
    ('premium.cpp', 'alertOwner("звёзды списаны (" + std::to_string(amount) + " XTR, chat " + forOwner(chatId, 32) + ", charge " +', '32→33@90'):
        'длина строки в сообщении владельцу',
    ('premium.cpp', 'if (!sendMsg(chatId, text, openAppKeyboard(lang)).ok)', 'if (!→if (@0'):
        'только журнал',
    ('premium.cpp', 'long long userId = 0;', '0→1@19'):
        'начальное значение перезаписывается до чтения',
    ('premium.cpp', 'if (!tgAnswerOk(httpsCapped(tgApi("editUserStarSubscription"), req.dump(), 15, TELEGRAM_MAX_BYTES, &why), why)) {', '15→16@75'):
        'тайм-аут сети',
    ('premium.cpp', 'if (!s.run()) std::cerr << "[PREMIUM] cancel: " << logSafe(sqlite3_errmsg(db)) << std::endl;', 'if (!→if (@0'):
        'только журнал',
    ('premium.cpp', 'std::this_thread::sleep_for(std::chrono::milliseconds(60));', '60→61@54'):
        'пауза между вызовами Telegram',
    ('premium_logic.h', 'if (a == std::string_view::npos) return false;', 'if (→if (!@0'):
        '«!a == npos» всегда ложно; без двоеточия строка не пройдёт strictInt',
    ('premium_logic.h', "const size_t b = payload.find(':', a + 1);", '1→2@39'):
        'пустой номер чата отклоняет strictInt',
    ('premium_logic.h', 'if (b == std::string_view::npos) return false;', 'if (→if (!@0'):
        '«!b == npos» всегда ложно; без второго двоеточия строка не пройдёт strictInt',
    ('premium_logic.h', 'long long id = 0;', '0→1@15'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', 'long long until = 0;', '0→1@18'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', 'if (!introBinding(payload, chat, until) || buyer.empty() || chat != buyer || now > until + INTRO_GRACE_SEC)', ' || → && @39'):
        'пустой покупатель и так отклонён; номер чата в нагрузке не бывает пустым',
    ('premium_logic.h', 'if (units < 0) units = 0;', ' < → <= @9'):
        'ноль и единица дают одинаковые «0.00» — сумма до цента',
    ('premium_logic.h', 'if (units < 0) units = 0;', '0→1@12'):
        'ноль и единица дают одинаковые «0.00» — сумма до цента',
    ('premium_logic.h', 'if (units < 0) units = 0;', '0→1@23'):
        'ноль и единица дают одинаковые «0.00» — сумма до цента',
    ('premium_logic.h', 'long long wc = 0;', '0→1@15'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', 'std::array<uint8_t, 36> b{};', '36→37@20'):
        'размер массива с запасом',
    ('premium_logic.h', 'uint32_t acc = 0;', '0→1@15'):
        'начальная единица уходит за пределы байта (& 0xFF)',
    ('premium_logic.h', 'size_t n = 0; ⏎ for (const char c : addr) { ⏎ uint32_t v = 0;', '0→1@13'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', 'long long units = 0;', '0→1@18'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', "while (i < args.size() && args[i] == ' ') ++i;", ' < → <= @8'):
        'за концом строки — нулевой знак, не пробел: пропуск остановится там же',
    ('premium_logic.h', 'if (a == a_end || b == b_end || skip(b_end) != args.size()) return std::nullopt;', ' || → && @14'):
        'пустые части отклоняет strictInt; «!a» относится только к первому сравнению',
    ('premium_logic.h', 'if (a == a_end || b == b_end || skip(b_end) != args.size()) return std::nullopt;', 'if (→if (!@0'):
        'пустые части отклоняет strictInt; «!a» относится только к первому сравнению',
    ('premium_logic.h', 'long long chat = 0;', '0→1@17'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', 'long long days = 0;', '0→1@17'):
        'начальное значение перезаписывается до чтения',
    ('premium_logic.h', 'out.reserve(s.size() < max ? s.size() : max);', ' < → <= @20'):
        'запас памяти, не результат',
    ('premium_logic.h', "out += c < 0x20U || c == 0x7FU ? ' ' : ch;", ' < → <= @8'):
        'пробел и так остаётся пробелом',
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
    with open(os.path.join(root, path), encoding="utf-8") as f:
        lines = f.read().split("\n")
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
        # Короткая строка («return false;») повторяется в файле много раз —
        # ключ EQUIV для неё дополняют две строки над ней, иначе одна запись
        # молча пропускала бы все одинаковые строки.
        key = s
        if len(s) <= 16:
            key = " ⏎ ".join([x.strip() for x in lines[max(0, n - 2): n]] + [s])
        indent = len(line) - len(line.lstrip())

        def add(a: int, b: int, rep: str, label: str, *, line: str = line, seen: set = seen, n: int = n,
                indent: int = indent, key: str = key) -> None:
            new = line[:a] + rep + line[b:]
            if new != line and new not in seen:
                seen.add(new)
                out.append((n, new, f"{label}@{a - indent}", key))

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
                           capture_output=True, text=True, check=False)
        if r.returncode:
            log.append(r.stderr[-500:])
            return False
    r = subprocess.run([CXX, "-pthread", os.path.join(work, "premium_test.cpp.o"), os.path.join(work, "premium.cpp.o"),
                        *objs.values(), "-lsqlite3", "-o", os.path.join(work, "t")], capture_output=True, text=True,
                       check=False)
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
    path, n, new, _label, objs, test_o, base = args
    with tempfile.TemporaryDirectory() as work:
        setup(work, base)
        p = os.path.join(work, path)
        with open(p, encoding="utf-8") as f:
            lines = f.read().split("\n")
        lines[n] = new
        with open(p, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        log: list = []
        if not build(work, objs, log, test_o if path == "premium.cpp" else ""):
            return "build", ""
        try:
            r = subprocess.run([os.path.join(work, "t")], capture_output=True, text=True, timeout=120, check=False,
                               env=TEST_ENV)
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
        if not build(base, objs, []) or subprocess.run([os.path.join(base, "t")], capture_output=True, check=False,
                                                         env=TEST_ENV).returncode:
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
