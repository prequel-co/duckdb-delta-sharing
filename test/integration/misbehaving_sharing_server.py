"""Delta Sharing server that misbehaves, for testing the extension's HTTP limits.

The first path segment picks the behaviour; the rest is a normal sharing API path,
so a secret with ENDPOINT 'http://127.0.0.1:<port>/<mode>' selects a mode:
  echo     listings return the page token they were sent (first page: "t0")
  cycle    listings alternate page tokens: a, b, a, ...
  endless  listings always return a new page token
  pages3   listings take three pages, then finish
  link     table queries and change feeds name themselves as the next page
  stall    accepts the request and never answers
  trickle  answers a listing at about 2 KB/s for about 4 seconds
GET /requests returns how many requests the server has answered (not counting itself).

Usage: misbehaving_sharing_server.py
Prints the bound port on the first line of stdout, then serves until killed.
"""
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

SCHEMA = json.dumps({"type": "struct", "fields": [
    {"name": "id", "type": "long", "nullable": True, "metadata": {}}]})
TABLE_LINES = [
    {"protocol": {"minReaderVersion": 1}},
    {"metaData": {"id": "t", "format": {"provider": "parquet"},
                  "schemaString": SCHEMA, "partitionColumns": []}},
]
NEXT_TOKEN = {
    "echo": lambda token: token or "t0",
    "cycle": lambda token: "b" if token == "a" else "a",
    "endless": lambda token: str(int(token or "0") + 1),
    "pages3": lambda token: {"": "p2", "p2": "p3"}.get(token, ""),
}

answered = 0
lock = threading.Lock()


class MisbehavingServer(BaseHTTPRequestHandler):
    # keep-alive, so a client that loops reuses one connection instead of
    # running the machine out of ports
    protocol_version = "HTTP/1.1"

    def _send(self, body, content_type, headers=()):
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(data)

    def _trickle(self, body):
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        for start in range(0, len(data), 512):
            self.wfile.write(data[start:start + 512])
            self.wfile.flush()
            time.sleep(0.25)

    def _handle(self):
        global answered
        url = urlsplit(self.path)
        if url.path == "/requests":
            self._send(str(answered), "text/plain")
            return
        with lock:
            answered += 1
        mode, _, rest = url.path.lstrip("/").partition("/")
        if mode == "stall":
            time.sleep(600)
            return
        if mode == "trickle":
            self._trickle(json.dumps({"items": [{"name": "s", "id": "1"}]}).ljust(8192))
            return
        if rest.endswith("/query") or rest.endswith("/changes"):
            link = [("Link", f'</{rest}>; rel="next"')] if mode == "link" else []
            self._send("\n".join(json.dumps(line) for line in TABLE_LINES),
                       "application/x-ndjson", link)
            return
        token = parse_qs(url.query).get("pageToken", [""])[0]
        item = {"name": f"item{answered}", "schema": "sc", "share": "s", "id": str(answered)}
        body = {"items": [item]}
        next_token = NEXT_TOKEN.get(mode, lambda _: "")(token)
        if next_token:
            body["nextPageToken"] = next_token
        self._send(json.dumps(body), "application/json")

    def do_GET(self):
        self._handle()

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self._handle()

    def log_message(self, *args):
        pass


server = ThreadingHTTPServer(("127.0.0.1", 0), MisbehavingServer)
server.daemon_threads = True
print(server.server_port, flush=True)
server.serve_forever()
