#!/usr/bin/env python3
"""Защищённый запрос к чужому серверу (safe_http.cpp) — на настоящих
локальных серверах, без интернета (tests/run.sh).

Поднимаются свой удостоверяющий центр, https-сервер с его сертификатом
(имя — localhost), второй — с самоподписанным сертификатом, и обычный http.
Каждая атака — свой адрес на сервере; проверяем, что httpsCapped:

- не ходит по http (сервер не получает ни одного запроса);
- не принимает чужой сертификат и сертификат на другое имя;
- не выполняет перенаправление (цель перенаправления не запрошена);
- обрывает ответ больше предела — и с длиной в заголовке, и без неё;
- не ждёт дольше срока ответ «по байту»;
- не просит сжатие и не распаковывает его сам;
- нормальный ответ и POST с JSON проходят как есть.
"""
import gzip
import http.server
import json
import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from typing import ClassVar

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIG = 2_000_000


class Handler(http.server.BaseHTTPRequestHandler):
    seen: ClassVar[list] = []

    def log_message(self, *a):  # тишина в выводе проверки
        pass

    def _send(self, code: int, body: bytes, headers: dict | None = None, length: bool = True) -> None:
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        if length:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        Handler.seen.append((self.path, dict(self.headers)))
        if self.path == "/ok":
            self._send(200, b'{"ok":true}')
        elif self.path == "/err":
            self._send(400, b'{"ok":false,"description":"Bad Request"}')
        elif self.path == "/big":
            self._send(200, b"[" + b"1," * (BIG // 2) + b"1]")
        elif self.path == "/big-chunked":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            for _ in range(BIG // 65536 + 2):
                self.wfile.write(b"x" * 65536)
        elif self.path == "/redirect":
            self._send(302, b"moved", {"Location": "/ok"})
        elif self.path == "/slow":
            self.send_response(200)
            self.send_header("Content-Length", "1000")
            self.end_headers()
            try:
                for _ in range(1000):
                    self.wfile.write(b"x")
                    self.wfile.flush()
                    time.sleep(0.2)
            except OSError:
                pass
        elif self.path == "/gzip":
            self._send(200, gzip.compress(b"0" * 100_000), {"Content-Encoding": "gzip"})
        else:
            self._send(404, b"")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        Handler.seen.append((self.path, dict(self.headers), body))
        self._send(200, body)


class Server(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):  # оборванные нами ответы — ожидаемы
        pass


def serve(ctx: ssl.SSLContext | None) -> tuple[http.server.ThreadingHTTPServer, int]:
    srv = Server(("127.0.0.1", 0), Handler)
    srv.daemon_threads = True
    if ctx:
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def openssl(*args: str, cwd: str) -> None:
    subprocess.run(["openssl", *args], cwd=cwd, check=True, capture_output=True)


class SafeHttpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        d = cls.dir
        # Свой удостоверяющий центр и сертификат на localhost от него.
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "2",
                "-subj", "/CN=test-ca", cwd=d)
        openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", "srv.key", "-out", "srv.csr", "-subj", "/CN=localhost",
                cwd=d)
        with open(os.path.join(d, "ext.cnf"), "w", encoding="utf-8") as f:
            f.write("subjectAltName=DNS:localhost\n")
        openssl("x509", "-req", "-in", "srv.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", "srv.pem",
                "-days", "2", "-extfile", "ext.cnf", cwd=d)
        # И самоподписанный — «подменённый» сервер.
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "evil.key", "-out", "evil.pem", "-days", "2",
                "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost", cwd=d)
        good = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        good.load_cert_chain(os.path.join(d, "srv.pem"), os.path.join(d, "srv.key"))
        evil = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        evil.load_cert_chain(os.path.join(d, "evil.pem"), os.path.join(d, "evil.key"))
        cls.servers = []
        for ctx in (good, evil, None):
            srv, port = serve(ctx)
            cls.servers.append(srv)
            cls.ports = (*getattr(cls, "ports", ()), port)
        cls.drv = os.path.join(d, "drv")
        ca = os.path.join(d, "ca.pem")
        subprocess.run(["g++", "-std=c++20", "-O1", "-I", ROOT, f'-DSAFE_HTTP_TEST_CAINFO="{ca}"',
                        os.path.join(ROOT, "tests", "safe_http_driver.cpp"), os.path.join(ROOT, "safe_http.cpp"),
                        "-lcurl", "-o", cls.drv], check=True)

    @classmethod
    def tearDownClass(cls):
        for s in cls.servers:
            s.shutdown()
        shutil.rmtree(cls.dir, ignore_errors=True)

    def setUp(self):
        Handler.seen.clear()

    def call(self, url: str, max_bytes: int = 100_000, timeout: int = 5, post: str = "") -> dict:
        env = {**os.environ, "NO_PROXY": "*", "no_proxy": "*"}
        for k in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy", "ALL_PROXY", "all_proxy"):
            env.pop(k, None)
        args = [self.drv, url, str(max_bytes), str(timeout)] + ([post] if post else [])
        out = subprocess.run(args, capture_output=True, text=True, check=True, env=env, timeout=60).stdout
        r = json.loads(out)
        r["body"] = bytes.fromhex(r["hex"])
        return r

    def good(self, path: str) -> str:
        return f"https://localhost:{self.ports[0]}{path}"

    def test_normal_answer_and_error_body_pass(self):
        r = self.call(self.good("/ok"))
        self.assertEqual((r["body"], r["error"]), (b'{"ok":true}', ""))
        r = self.call(self.good("/err"))  # у Bot API в теле 4xx — причина
        self.assertIn(b"Bad Request", r["body"])

    def test_plain_http_is_never_requested(self):
        r = self.call(f"http://127.0.0.1:{self.ports[2]}/ok")
        self.assertEqual((r["len"], r["error"]), (0, "не https"))
        for url in ("ftp://localhost/x", "file:///etc/passwd", "gopher://localhost/", "HTTPS://localhost/ok", ""):
            self.assertEqual(self.call(url)["len"], 0, url)
        self.assertEqual(Handler.seen, [])

    def test_foreign_certificate_and_wrong_name_are_rejected(self):
        r = self.call(f"https://localhost:{self.ports[1]}/ok")
        self.assertEqual(r["len"], 0)
        self.assertTrue(r["error"])
        r = self.call(f"https://127.0.0.1:{self.ports[0]}/ok")  # сертификат на localhost, а не на 127.0.0.1
        self.assertEqual(r["len"], 0)
        self.assertTrue(r["error"])
        self.assertEqual(Handler.seen, [])  # до запроса дело не дошло

    def test_redirect_is_not_followed(self):
        r = self.call(self.good("/redirect"))
        self.assertEqual(r["len"], 0)
        self.assertIn("перенаправление 302", r["error"])
        self.assertEqual([p for p, *_ in Handler.seen], ["/redirect"])

    def test_oversized_answer_is_refused_not_truncated(self):
        for path in ("/big", "/big-chunked"):
            r = self.call(self.good(path), max_bytes=100_000)
            self.assertEqual(r["len"], 0, path)
            self.assertIn("больше 100000 байт", r["error"], path)
        self.assertEqual(self.call(self.good("/big"), max_bytes=BIG + 10)["len"], BIG + 3)

    def test_slow_drip_is_cut_by_total_deadline(self):
        t0 = time.monotonic()
        r = self.call(self.good("/slow"), timeout=2)
        self.assertLess(time.monotonic() - t0, 6)
        self.assertEqual(r["len"], 0)
        self.assertTrue(r["error"])

    def test_no_compression_requested_or_unpacked(self):
        r = self.call(self.good("/gzip"))
        self.assertEqual(r["body"][:2], b"\x1f\x8b")  # как пришло, не распаковано
        self.assertLess(r["len"], 10_000)
        headers = Handler.seen[0][1]
        self.assertNotIn("Accept-Encoding", headers)

    def test_post_sends_json(self):
        r = self.call(self.good("/post"), post='{"a":1}')
        self.assertEqual(r["body"], b'{"a":1}')
        path, headers, body = Handler.seen[0]
        self.assertEqual((path, headers.get("Content-Type"), body), ("/post", "application/json", b'{"a":1}'))


if __name__ == "__main__":
    import io
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    buf = io.StringIO()
    res = unittest.TextTestRunner(stream=buf, verbosity=0).run(suite)
    if not res.wasSuccessful():
        sys.stderr.write(buf.getvalue())
        sys.exit(1)
    print(f"safe_http: {res.testsRun} атак на настоящих серверах — отбиты")
