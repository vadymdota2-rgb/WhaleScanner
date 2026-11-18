#!/usr/bin/env python3
"""Правила кода оплаты — «что модулю можно» (tests/run.sh).

Защита, которая не зависит от внимательности ревьюера: если кто-то —
человек или ИИ-агент — впишет в оплату опасное (запуск процессов, файлы,
новый адрес в сети, SQL со вставкой данных строкой, разбор чисел через
stoll, глотание исключений), проверка упадёт, и в main это не попадёт.

Код разбирается после удаления комментариев и содержимого строк: правило
про код, а не про пояснения к нему, и обойти его словом в комментарии
нельзя. Строковые литералы для правил про SQL и сеть берутся отдельно.
"""
import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# PREMIUM_CPP — копия с подложенной порчей (tools/mutate.py, план запросов).
CPP = os.environ.get("PREMIUM_CPP") or os.path.join(ROOT, "premium.cpp")
LOGIC = os.path.join(ROOT, "premium_logic.h")
HEADER = os.path.join(ROOT, "premium.h")

# Что можно подключать. У правил (premium_logic.h) — ни базы, ни сети, ни
# часов, ни потоков, ни ввода-вывода: только вычисления.
CPP_INCLUDES = {"premium.h", "premium_logic.h", "lifecycle.h", "safe_http.h", "ru.h", "telegram.h", "utils.h",
                "sqlite3.h", "algorithm", "array", "atomic", "chrono", "cstdint", "cstdlib", "ctime", "iostream", "map", "memory",
                "mutex", "optional", "set", "string", "thread", "vector"}
LOGIC_INCLUDES = {"algorithm", "array", "utility", "climits", "cstddef", "cstdint", "optional", "string", "string_view", "unordered_set",
                  "json.hpp"}
HEADER_INCLUDES = {"cstddef", "set", "string", "vector", "json.hpp"}
SAFE_HTTP = os.path.join(ROOT, "safe_http.cpp")
SAFE_HTTP_INCLUDES = {"safe_http.h", "curl/curl.h", "memory", "string"}
# Разборщик JSON вшит в репозиторий — и разбирает всё, что пришло извне.
# Хеш — релиз nlohmann/json 3.11.3 (сверено побайтно с
# github.com/nlohmann/json/releases/download/v3.11.3/json.hpp): подмена
# файла в репозитории роняет проверку.
JSON_HPP_SHA256 = "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6"

# Вызовы, которых в оплате быть не должно ни под каким видом.
BANNED = [
    r"\bsystem\s*\(", r"\bpopen\s*\(", r"\bexec[lv]p?e?\s*\(", r"\bfork\s*\(", r"\bdlopen\s*\(",
    r"\bfopen\s*\(", r"\bofstream\b", r"\bifstream\b", r"\bfstream\b", r"\bremove\s*\(", r"\brename\s*\(",
    r"\bunlink\s*\(", r"\bstoi\b", r"\bstol\b", r"\bstoll\b", r"\bstoul\b", r"\batoi\b", r"\batol\b", r"\bstrtol\b",
    r"\bsprintf\s*\(", r"\bstrcpy\s*\(", r"\bstrcat\s*\(", r"\bgets\s*\(", r"\brand\s*\(", r"\bsrand\s*\(",
    r"\bmalloc\s*\(", r"\bfree\s*\(", r"\bnew\s+\w", r"\bdelete\b(?!\s*;)", r"\breinterpret_cast\b",
    r"\bconst_cast\b", r"\bgoto\b", r"\bsetenv\b", r"\bputenv\b", r"\.detach\s*\(", r"\bstd::thread\s*\(",
    r"\btry\b", r"\bcatch\b", r"\bthrow\b", r"#\s*define\b",
]
# Правилам — ещё и никакого состояния и внешнего мира.
LOGIC_BANNED = BANNED + [r"\btime\s*\(", r"\bgetenv\b", r"\bstd::cout\b", r"\bstd::cerr\b", r"\bsqlite3",
                         r"\bhttp\s*\(", r"\bstatic\s+(?!inline|constexpr|const|_assert|cast)\w"]


def strip(src: str) -> str:
    """Код без комментариев; содержимое строк и символов — пустое."""
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif src.startswith('R"(', i):
            j = src.find(')"', i + 3)
            out.append('R"()"')
            i = n if j < 0 else j + 2
        elif src[i] in "\"'":
            q, j = src[i], i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == "\\" else 1
            out.append(q + q)
            i = j + 1
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def includes(src: str) -> set:
    return set(re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', src, re.MULTILINE))


class PolicyTest(unittest.TestCase):
    def test_includes_are_on_the_list(self):
        for path, allowed in ((CPP, CPP_INCLUDES), (LOGIC, LOGIC_INCLUDES), (HEADER, HEADER_INCLUDES),
                              (SAFE_HTTP, SAFE_HTTP_INCLUDES)):
            extra = includes(read(path)) - allowed
            self.assertFalse(extra, f"{os.path.basename(path)}: новый #include {sorted(extra)}")

    def test_no_banned_calls(self):
        for path, banned in ((CPP, BANNED), (LOGIC, LOGIC_BANNED), (HEADER, BANNED)):
            code = strip(read(path))
            for pat in banned:
                m = re.search(pat, code)
                self.assertIsNone(m, f"{os.path.basename(path)}: запрещено {pat!r}: …{code[max(0, m.start() - 60):m.end() + 20] if m else ''}…")

    def test_sql_is_parameterized(self):
        """SQL — только литералом: данные идут через bind, а не склейкой строк.
        Единственная собранная строка — список «?» для IN (premiumSubsetOf),
        и в неё добавляются только литералы."""
        src = read(CPP)
        code = strip(src)
        for m in re.finditer(r"\bStmt\s+\w+\s*\(\s*([^;]*?)\)\s*;", code, re.DOTALL):
            arg = m.group(1).strip()
            self.assertTrue(re.fullmatch(r'(""\s*)+', arg) or arg == "sql.c_str()", f"Stmt не из литерала: {arg!r}")
        for m in re.finditer(r"\bsql\s*\+?=\s*([^;]*);", code):
            self.assertTrue(re.fullmatch(r'\s*(""|i\s*!=\s*0\s*\?\s*""\s*:\s*"")\s*', m.group(1)),
                            f"в SQL склеивается не литерал: {m.group(0)!r}")
        for m in re.finditer(r"\bsqlite3_exec\s*\(\s*db\s*,\s*([^,]+),", code):
            self.assertIn(m.group(1).strip(), {'""', "sql", "tablesSql", "indexSql"}, f"sqlite3_exec не из литерала: {m.group(1)!r}")
        # sql в addColumnLocked — только из списка литералов в цикле.
        for m in re.finditer(r"\baddColumnLocked\s*\(\s*([^)]*)\)", code):
            self.assertIn(m.group(1).strip(), {"sql", "const char* sql"}, f"addColumnLocked не из литерала: {m.group(1)!r}")
        loops = re.findall(r"for\s*\(\s*const char\*\s+sql\s*:\s*\{([^}]*)\}\s*\)\s*ok\s*=\s*ok\s*&&\s*addColumnLocked\(sql\)", code)
        self.assertEqual(len(loops), 2, "миграции — два цикла по спискам литералов")
        for body in loops:
            self.assertRegex(body, r'^\s*""(\s*,\s*"")*\s*$', "в списке миграций — не только литералы")

    def test_network_goes_only_to_known_hosts(self):
        """В сеть — только httpsCapped (safe_http): к toncenter (адрес —
        константа) и к Bot API через tgApi с литералом метода, каждый раз с
        пределом ответа. Общий http() без пределов — запрещён."""
        src = read(CPP)
        code = strip(src)
        self.assertIsNone(re.search(r"(?<![\w.])http\s*\(", code), "http() без пределов в оплате")
        calls = re.findall(r"\bhttpsCapped\s*\(\s*([^,]+),[^,]*,[^,]*,\s*([^,]+),", code)
        self.assertEqual(len(calls), 3)
        for arg, cap in calls:
            arg = arg.strip()
            self.assertTrue(arg == "url" or re.fullmatch(r'tgApi\(""\)', arg), f"запрос не к известному адресу: {arg!r}")
            self.assertIn(cap.strip(), {"TONCENTER_MAX_BYTES", "TELEGRAM_MAX_BYTES"}, "запрос без предела ответа")
        literals = re.findall(r'"(https?://[^"]*)"', src)
        self.assertEqual(set(literals), {"https://toncenter.com/api/v3/"}, "новый адрес в сети")
        self.assertEqual(re.findall(r'\bgetenv\s*\(\s*"([^"]+)"', src), ["TON_WALLET_ADDRESS"])

    def test_money_paths_use_transactions(self):
        """Где меняется срок или счёт — там Tx: продление без транзакции
        оставило бы «оплачен, а подписки нет»."""
        code = strip(read(CPP))
        for fn in ("grantDays", "cleanupExpiredPremium", "pollUsdtPayments", "applySuccessfulPayment"):
            m = re.search(r"\b" + fn + r"\s*\([^)]*\)\s*\{", code)
            self.assertIsNotNone(m, fn)
            body, depth, i = [], 1, m.end()
            while depth:
                ch = code[i]
                depth += ch == "{"
                depth -= ch == "}"
                body.append(ch)
                i += 1
            self.assertRegex("".join(body), r"\bTx\s+tx\s*\(", f"{fn}: запись без транзакции")
        # Никаких BEGIN/COMMIT/ROLLBACK вручную — только через Tx.
        raw = read(CPP)
        self.assertEqual(len(re.findall(r'"(BEGIN IMMEDIATE|COMMIT|ROLLBACK)"', raw)), 3)

    def test_logs_take_outside_strings_through_logSafe(self):
        """Строка из ответа Telegram, обозревателя или SQLite — в журнал только
        через logSafe: иначе ответ мог бы дописать в журнал поддельные строки."""
        code = strip(read(CPP))
        for m in re.finditer(r"std::c(err|out)\s*<<(.*?);", code, re.DOTALL):
            stmt = m.group(2)
            for bad in ("sqlite3_errmsg", "payload", "currency", "chargeId", "memo", "chatId", "e.what", "description"):
                for hit in re.finditer(r"<<\s*([^<]*\b" + re.escape(bad) + r"\b[^<]*)", stmt):
                    piece = hit.group(1).strip()
                    self.assertTrue(piece.startswith(("logSafe(", "premium::usdText(")),
                                    f"в журнал без logSafe: {piece!r}")


class SupplyChainTest(unittest.TestCase):
    def test_json_parser_is_the_released_one(self):
        import hashlib
        with open(os.path.join(ROOT, "json.hpp"), "rb") as f:
            self.assertEqual(hashlib.sha256(f.read()).hexdigest(), JSON_HPP_SHA256, "json.hpp изменён")

    def test_safe_http_keeps_its_guards(self):
        """Защиты запроса к чужому серверу не убираются молча."""
        code = strip(read(SAFE_HTTP))
        for must in (r"CURLOPT_FOLLOWLOCATION,\s*0L", r"CURLOPT_SSL_VERIFYPEER,\s*1L", r"CURLOPT_SSL_VERIFYHOST,\s*2L",
                     r"CURLOPT_PROTOCOLS_STR,\s*\"\"", r"CURLOPT_PROTOCOLS,\s*static_cast<long>\(CURLPROTO_HTTPS\)",
                     r"CURLOPT_TIMEOUT,", r"CURLOPT_MAXFILESIZE_LARGE", r"capAppend\("):
            self.assertRegex(code, must)
        for banned in (r"CURLOPT_SSL_VERIFYPEER,\s*0", r"CURLOPT_SSL_VERIFYHOST,\s*0", r"CURLOPT_ACCEPT_ENCODING",
                       r"CURLOPT_FOLLOWLOCATION,\s*1", r"CURLOPT_INSECURE"):
            self.assertNotRegex(code, banned)
        # Свой удостоверяющий центр — только в сборке проверки.
        raw = read(SAFE_HTTP)
        self.assertRegex(raw, r"#ifdef SAFE_HTTP_TEST_CAINFO\s+(//[^\n]*\n\s*)*curl_easy_setopt\(c, CURLOPT_CAINFO")


class LicenseTest(unittest.TestCase):
    """Авторские права: модуль — Wallet Tracker, сторонний код — под своей
    лицензией и с её текстом (MIT требует прикладывать уведомление)."""

    MODULE = ("premium.h", "premium.cpp", "premium_logic.h", "safe_http.h", "safe_http.cpp")

    def test_module_files_carry_copyright(self):
        for name in self.MODULE:
            first = read(os.path.join(ROOT, name)).splitlines()[0]
            self.assertEqual(first, "// © 2026 Wallet Tracker. Все права защищены — см. LICENSE.", name)

    def test_third_party_notices_are_present(self):
        lic = read(os.path.join(ROOT, "LICENSE"))
        self.assertIn("All rights reserved", lic)
        self.assertIn("json.hpp", lic)
        self.assertIn("LICENSES/nlohmann-json.txt", lic)
        mit = read(os.path.join(ROOT, "LICENSES", "nlohmann-json.txt"))
        for must in ("MIT License", "Copyright (c) 2013-2022 Niels Lohmann",
                     "The above copyright notice and this permission notice shall be included"):
            self.assertIn(must, mit)
        head = read(os.path.join(ROOT, "json.hpp"))[:600]
        self.assertIn("SPDX-FileCopyrightText: 2013-2023 Niels Lohmann", head)
        self.assertIn("SPDX-License-Identifier: MIT", head)


class ContractTest(unittest.TestCase):
    """Договор с API (tests/fixtures/premium_contract.json; тот же файл — в
    Wallet-Tracker). Константы правил сверяет tests/premium_test.cpp; здесь —
    то, что живёт в тексте premium.cpp, и одинаковость двух копий."""

    def setUp(self):
        import json
        self.c = json.loads(read(os.path.join(ROOT, "tests", "fixtures", "premium_contract.json")))

    def test_wallet_default_and_env(self):
        src = read(CPP)
        u = self.c["usdt"]
        self.assertIn(f'std::getenv("{u["wallet_env"]}")', src)
        self.assertIn(f'return "{u["wallet_default"]}";', src)

    def test_same_file_as_the_api_when_both_are_here(self):
        api = os.path.join(os.path.dirname(ROOT), "Wallet-Tracker", "tests", "fixtures", "premium_contract.json")
        if not os.path.exists(api):
            self.skipTest("репозитория API рядом нет")
        with open(api, "rb") as a, open(os.path.join(ROOT, "tests", "fixtures", "premium_contract.json"), "rb") as b:
            self.assertEqual(a.read(), b.read(), "договор у бота и у API разный")


class PolicyCatchesTest(unittest.TestCase):
    """Сами правила тоже проверены: в копию кода подкладываем по одному
    нарушению — каждое должно быть поймано. Иначе правило, ослабленное
    правкой регулярного выражения, молча пропускало бы всё."""

    INJECT: tuple = (
        ("const long long now = nowSec();", 'const long long now = nowSec(); system("x");'),
        ('Stmt s("SELECT is_premium, premium_start, premium_expire FROM users WHERE chat_id=?");',
         'Stmt s(("SELECT * FROM users WHERE chat_id=" + chatId).c_str());'),
        ('httpsCapped(url, "", 15, TONCENTER_MAX_BYTES', 'httpsCapped(std::string("https://evil.example/"), "", 15, TONCENTER_MAX_BYTES'),
        ('httpsCapped(url, "", 15, TONCENTER_MAX_BYTES', 'httpsCapped(url, "", 15, 1U << 30U'),
        ('const json j = json::parse(httpsCapped(', 'const json j = json::parse(http(url) + httpsCapped('),
        ("logSafe(payload)", "payload"),
        ('Tx tx("grant");', ";"),
        ('"ALTER TABLE ton_invoices ADD COLUMN kind TEXT NOT NULL DEFAULT \'ton\'",',
         '(std::string("ALTER TABLE ") + g_serviceChatId).c_str(),'),
        ("#include <vector>", "#include <vector>\n#include <fstream>"),
        ('std::getenv("TON_WALLET_ADDRESS")', 'std::getenv("HOME")'),
        ("const json j = json::parse(", "try { } catch (...) { }\n    const json j = json::parse("),
        # Большая база: запрос без своего индекса читает таблицу целиком.
        ("CREATE INDEX IF NOT EXISTS idx_ton_hash ON ton_invoices(tx_hash) WHERE tx_hash!='';", ""),
        ("CREATE INDEX IF NOT EXISTS idx_users_premium ON users(premium_expire) WHERE is_premium=1;", ""),
        ("p.sub_until > ? AND p.sub_until>0 AND", "p.sub_until > ? AND"),
    )

    def test_each_injected_violation_is_caught(self):
        global CPP
        import tempfile
        src = read(CPP)
        orig = CPP
        try:
            for old, new in self.INJECT:
                self.assertTrue(old in src, f"нет места для подлога: {old!r}")
                with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False, encoding="utf-8") as f:
                    f.write(src.replace(old, new, 1))
                CPP = f.name
                try:
                    with open(os.devnull, "w", encoding="utf-8") as sink:
                        load = unittest.defaultTestLoader.loadTestsFromTestCase
                        r = unittest.TextTestRunner(stream=sink, verbosity=0).run(
                            unittest.TestSuite([load(PolicyTest), load(ScaleTest)]))
                    self.assertFalse(r.wasSuccessful(), f"подлог не пойман: {new!r}")
                finally:
                    os.unlink(f.name)
        finally:
            CPP = orig


def cpp_literal(chunk: str) -> str:
    """Текст подряд идущих строковых литералов C++ ("…" "…" или R"(…)")."""
    raw = re.match(r'\s*R"\((.*?)\)"', chunk, re.DOTALL)
    if raw:
        return raw.group(1)
    out, i = [], 0
    while True:
        m = re.match(r'\s*"((?:[^"\\]|\\.)*)"', chunk[i:])
        if not m:
            return "".join(out)
        out.append(m.group(1))
        i += m.end()


class ScaleTest(unittest.TestCase):
    """Миллион человек и миллионы счетов: ни один запрос модуля не читает
    таблицу целиком — каждый идёт по индексу (план запроса SQLite, EXPLAIN
    QUERY PLAN, на той же схеме, что строит initPremium). Новый запрос без
    индекса — проверка падает, а не бот на большой базе."""

    def test_every_query_uses_an_index(self):
        import sqlite3

        src = read(CPP)
        con = sqlite3.connect(":memory:")
        # users и funnel_events заводит бот (main.cpp, initDB) — как там.
        con.execute("CREATE TABLE users(chat_id TEXT PRIMARY KEY, language TEXT NOT NULL DEFAULT 'en', "
                    "threshold_nanos INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL)")
        con.execute("CREATE TABLE funnel_events(chat_id TEXT NOT NULL, ev TEXT NOT NULL, src TEXT NOT NULL DEFAULT '', "
                    "day TEXT NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (chat_id, ev, src, day))")
        alters = re.findall(r'"(ALTER TABLE [^"]+)"', src)
        self.assertEqual(len(alters), 6)
        for sql in alters[:3]:
            con.execute(sql)
        for name in ("tablesSql", "indexSql"):
            m = re.search(r"const char\* " + name + r"\s*=\s*(R\"\(.*?\)\")", src, re.DOTALL)
            self.assertIsNotNone(m, name)
            con.executescript(cpp_literal(m.group(1)))
        queries = [cpp_literal(m.group(1)) for m in re.finditer(r"\bStmt\s+\w+\s*\(\s*((?:R?\"[^;]*?\"\s*)+)\)\s*;", src)]
        subset = re.search(r'std::string sql\s*=\s*("[^;]*?")\s*;', src)
        self.assertIsNotNone(subset)
        queries.append(cpp_literal(subset.group(1)) + "?,?)")
        # Каждый запрос модуля — в проверке (собранный — premiumSubsetOf — тоже).
        self.assertEqual(len(queries), len(re.findall(r"\bStmt\s+\w+\s*\(", strip(src))))
        for sql in queries:
            plan = con.execute("EXPLAIN QUERY PLAN " + sql, [None] * sql.count("?")).fetchall()
            scans = [row[3] for row in plan if row[3].startswith("SCAN") and "json_each" not in row[3]]
            self.assertFalse(scans, f"чтение всей таблицы: {sql!r} → {plan}")
            # Индекс только по статусу — тоже почти вся таблица: оплаченных и
            # истёкших счетов миллионы. Допустим лишь для живых (active):
            # их мало — гасятся через сутки.
            broad = [row[3] for row in plan if re.search(r"\(status=\?(\)| AND created_at)", row[3])]
            self.assertFalse(broad and "status='active'" not in sql, f"поиск по одному статусу: {sql!r} → {plan}")


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    r = unittest.TextTestRunner(stream=sys.stderr, verbosity=0).run(suite) if "-v" in sys.argv else None
    if r is None:
        import io
        buf = io.StringIO()
        r = unittest.TextTestRunner(stream=buf, verbosity=0).run(suite)
        if not r.wasSuccessful():
            sys.stderr.write(buf.getvalue())
    if r.wasSuccessful():
        print(f"premium: правила кода — {r.testsRun} проверок без нарушений")
    sys.exit(0 if r.wasSuccessful() else 1)
