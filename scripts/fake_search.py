#!/usr/bin/env python3
"""Offline fake for Brave (/res/v1/web/search) and Tavily (/search) web search APIs."""
import json, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/res/v1/web/search":
            if self.headers.get("X-Subscription-Token") != "brave-key": return self.send(401, {"error": "bad token"})
            q = parse_qs(u.query).get("q", [""])[0]; n = int(parse_qs(u.query).get("count", ["5"])[0])
            return self.send(200, {"web": {"results": [{"title": f"Brave {i} {q}", "url": f"https://b.example/{i}", "description": f"snippet {i}"} for i in range(n)]}})
        self.send(200, {"ok": True})
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        if self.path == "/search":
            if body.get("api_key") != "tav-key": return self.send(401, {"error": "bad key"})
            return self.send(200, {"results": [{"title": "Tavily " + body["query"], "url": "https://t.example/1", "content": "tav snippet"}]})
        self.send(404, {})

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
