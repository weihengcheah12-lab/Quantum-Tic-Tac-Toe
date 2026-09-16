#!/usr/bin/env python3
"""Local web server for the QT3 perfect-AI UI.

Launches qt3_perfect_ai.exe in `serve` mode ONCE (it loads the ~1.9GB solved
table on startup, ~40s) and keeps it resident as a subprocess for the life of
this server, forwarding one line-based command per HTTP request and reading
back the one JSON line it replies with. This is what avoids re-loading the
table on every move.

Usage: python server.py [port]   (default port 8765)
Then open http://localhost:8765/ in a browser.
"""

import sys
import subprocess
import http.server
import pathlib

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
HERE = pathlib.Path(__file__).resolve().parent
EXE = HERE / "qt3_perfect_ai.exe"
TT_FILE = HERE / "qt3_3x3_tt.bin"
INDEX = HERE / "index.html"

if not EXE.exists():
    print(f"FATAL: {EXE} not found. Build it with g++ first.", file=sys.stderr)
    sys.exit(1)
if not TT_FILE.exists():
    print(f"FATAL: {TT_FILE} not found. Run `qt3_solver save` first.", file=sys.stderr)
    sys.exit(1)

print("Starting engine subprocess (loading solved game data, ~40s)...", file=sys.stderr)
engine = subprocess.Popen(
    [str(EXE), "serve", str(TT_FILE)],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr,
    text=True, bufsize=1,
)
ready_line = engine.stdout.readline()  # initial NEW-equivalent state, also confirms it's alive
print("Engine ready.", file=sys.stderr)


def send_command(cmd: str) -> str:
    engine.stdin.write(cmd + "\n")
    engine.stdin.flush()
    line = engine.stdout.readline()
    if not line:
        raise RuntimeError("engine subprocess died")
    return line.rstrip("\n")


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # keep stderr quiet; comment out to debug HTTP traffic

    def _send(self, code, body: bytes, content_type: str):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self._send(200, INDEX.read_bytes(), "text/html; charset=utf-8")
        elif self.path == "/api/state":
            self._send(200, send_command("STATE").encode(), "application/json")
        else:
            self._send(404, b'{"error":"not found"}', "application/json")

    def do_POST(self):
        if self.path == "/api/cmd":
            length = int(self.headers.get("Content-Length", 0))
            cmd = self.rfile.read(length).decode().strip()
            try:
                result = send_command(cmd)
                self._send(200, result.encode(), "application/json")
            except Exception as e:
                self._send(500, f'{{"error":"{e}"}}'.encode(), "application/json")
        else:
            self._send(404, b'{"error":"not found"}', "application/json")


if __name__ == "__main__":
    httpd = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    print(f"Serving at http://localhost:{PORT}/ -- open this in a browser.", file=sys.stderr)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            send_command("QUIT")
        except Exception:
            pass
        engine.terminate()
