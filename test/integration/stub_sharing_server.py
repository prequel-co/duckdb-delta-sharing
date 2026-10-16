"""Minimal Delta Sharing server for offline integration tests.

Tables:
  empty -> protocol + metadata, no files (query and changes)
  data  -> protocol + metadata + one local parquet file (query only)

Usage: stub_sharing_server.py <parquet_path>
Prints the bound port on the first line of stdout, then serves until killed.
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PARQUET = sys.argv[1]
SCHEMA = json.dumps({"type": "struct", "fields": [
    {"name": "id", "type": "long", "nullable": True, "metadata": {}}]})


def table_lines(table, with_files):
    lines = [
        {"protocol": {"minReaderVersion": 1}},
        {"metaData": {"id": table, "format": {"provider": "parquet"},
                      "schemaString": SCHEMA, "partitionColumns": []}},
    ]
    if with_files and table == "data":
        lines.append({"file": {"url": PARQUET, "id": "f1", "partitionValues": {},
                               "size": os.path.getsize(PARQUET)}})
    return lines


class SharingServer(BaseHTTPRequestHandler):
    def _reply(self, lines):
        body = "\n".join(json.dumps(line) for line in lines).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _table(self):
        return self.path.split("/tables/")[1].split("/")[0]

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self._reply(table_lines(self._table(), with_files=True))

    def do_GET(self):
        self._reply(table_lines(self._table(), with_files=False))

    def log_message(self, *args):
        pass


server = HTTPServer(("127.0.0.1", 0), SharingServer)
print(server.server_port, flush=True)
server.serve_forever()
