#!/usr/bin/env python3
# @std/http against a local server: serves HTTP and HTTPS (with the test
# certificate beside this file, which nothing outside these tests trusts) on
# ports of the machine's choosing, runs http.cluau with their addresses, and
# compares its output with http.expected.
#
#   python3 run.py <cluau> [--update]

import http.server
import json
import os
import ssl
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body, content_type="text/plain", headers=None):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def route(self):
        path = self.path.split("?")[0]
        if path == "/hello":
            return self.reply(200, "hello", headers={"X-Test": "yes", "Set-Cookie": "a=1"})
        if path == "/echo":
            received = self.body().decode()
            info = {
                "method": self.command,
                "body": received,
                "type": self.headers.get("Content-Type") or "",
                "agent": (self.headers.get("User-Agent") or "").split("/")[0],
                "custom": self.headers.get("X-Custom") or "",
            }
            return self.reply(200, json.dumps(info, sort_keys=True), "application/json")
        if path.startswith("/status/"):
            return self.reply(int(path.split("/")[2]), "status")
        if path == "/redirect":
            return self.reply(302, "", headers={"Location": "/hello"})
        if path == "/slow":
            time.sleep(3)
            return self.reply(200, "late")
        if path == "/big":
            return self.reply(200, b"x" * (1 << 20), "application/octet-stream")
        return self.reply(404, "not found")

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = route


def serve(tls):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    if tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(os.path.join(HERE, "test-cert.pem"), os.path.join(HERE, "test-key.pem"))
        server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


def main():
    cluau = sys.argv[1]
    update = "--update" in sys.argv
    env = dict(os.environ)
    env["CLUAU_TEST_HTTP"] = f"http://127.0.0.1:{serve(False)}"
    env["CLUAU_TEST_HTTPS"] = f"https://localhost:{serve(True)}"
    env["CLUAU_TEST_CA"] = os.path.join(HERE, "test-cert.pem")
    result = subprocess.run([cluau, "http.cluau"], cwd=HERE, env=env, capture_output=True, text=True, timeout=120)
    out = (result.stdout + result.stderr).replace("\r\n", "\n") + f"exit: {result.returncode}\n"
    expected_path = os.path.join(HERE, "http.expected")
    if update:
        with open(expected_path, "w", newline="\n") as f:
            f.write(out)
        return
    with open(expected_path) as f:
        expected = f.read().replace("\r\n", "\n")
    if out != expected:
        sys.exit(f"FAIL http.cluau: the output differs from http.expected\n--- got:\n{out}--- expected:\n{expected}")
    print("http: every request matched")


main()
