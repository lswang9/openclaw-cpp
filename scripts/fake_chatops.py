#!/usr/bin/env python3
"""Offline fake for signal-cli REST, Matrix C-S, Bot Framework (token + activities), Google Chat and Mattermost APIs.
Every request is appended to LOG as {"method","path","auth","body"}."""
import json, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs
LOG = sys.argv[2]

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def handle_any(self, method):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0)).decode()
        try: body = json.loads(raw) if raw else {}
        except Exception: body = {k: v[0] for k, v in parse_qs(raw).items()}
        with open(LOG, "a") as f: f.write(json.dumps({"method": method, "path": self.path, "auth": self.headers.get("Authorization", ""), "body": body}) + "\n")
        p = self.path
        if p.endswith("/oauth2/v2.0/token"):
            ok = body.get("client_id") == "app-1" and body.get("client_secret") == "pw-1"
            return self.send(200 if ok else 401, {"access_token": "teams-tok", "expires_in": 3600} if ok else {"error": "invalid_client"})
        if p == "/v2/send": return self.send(201, {"timestamp": "1"})
        if "/_matrix/client/v3/rooms/" in p:
            return self.send(200 if self.headers.get("Authorization") == "Bearer mx-tok" else 401, {"event_id": "$e1"})
        if "/v3/conversations/" in p:
            return self.send(200 if self.headers.get("Authorization") == "Bearer teams-tok" else 401, {"id": "a1"})
        if p.startswith("/v1/spaces/"): return self.send(200, {"name": "spaces/x/messages/1"})
        if p == "/api/v4/posts":
            return self.send(201 if self.headers.get("Authorization") == "Bearer mm-tok" else 401, {"id": "p1"})
        self.send(200, {"ok": True})
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self): self.handle_any("GET")
    def do_POST(self): self.handle_any("POST")
    def do_PUT(self): self.handle_any("PUT")

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
