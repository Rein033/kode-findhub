#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Answers the macless-haystack web page from reports a Mac pushed (findmy_mac.py).

Speaks the endpoint's protocol: POST {"ids": [...], "days": n} returns Apple's
fetch result with only those ids and days, newest first. The reports are
encrypted; this server never sees a private key.

    python3 cache_server.py [--file /opt/findmy-cache/reports.json] [--port 6176]
"""
import argparse
import base64
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

APPLE_EPOCH = 978307200


def make_handler(path):
    class H(BaseHTTPRequestHandler):
        def _cors(self):
            self.send_header("Access-Control-Allow-Origin", self.headers.get("Origin") or "*")
            self.send_header("Access-Control-Allow-Credentials", "true")
            self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")

        def do_OPTIONS(self):
            self.send_response(200)
            self._cors()
            self.end_headers()

        def do_GET(self):
            try:
                with open(path) as f:
                    d = json.load(f)
                msg = f"reports={len(d.get('results', []))} fetched={d.get('fetched')} age_s={int(time.time()) - int(d.get('fetched', 0))}"
            except OSError:
                msg = "no reports pushed yet"
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(msg.encode())

        def do_POST(self):
            try:
                body = json.loads(self.rfile.read(int(self.headers.get("content-length", 0))) or b"{}")
                ids = set(body.get("ids", []))
                days = int(body.get("days", 7))
                with open(path) as f:
                    d = json.load(f)
            except (OSError, ValueError):
                d, ids, days = {"results": []}, set(), 7
            cut = time.time() - days * 86400
            out = []
            for e in d.get("results", []):
                try:
                    ts = int.from_bytes(base64.b64decode(e["payload"])[0:4], "big") + APPLE_EPOCH
                except (KeyError, ValueError):
                    continue
                if e.get("id") in ids and ts > cut:
                    out.append((ts, e))
            out.sort(key=lambda x: x[0], reverse=True)
            resp = json.dumps({"results": [e for _, e in out], "statusCode": "200"}).encode()
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(resp)

        def log_message(self, fmt, *a):
            pass

    return H


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="/opt/findmy-cache/reports.json")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6176)
    a = ap.parse_args()
    HTTPServer((a.host, a.port), make_handler(a.file)).serve_forever()
