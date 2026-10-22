#!/usr/bin/env python3
"""Offline fake for signal-cli REST, Matrix C-S, Bot Framework (token + activities), Google Chat and Mattermost APIs.
Every request is appended to LOG as {"method","path","auth","body"}."""
import json, os, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs
LOG = sys.argv[2]
Q = {"matrix": [], "matrix_invite": [], "signal": [], "mattermost": []}
LOCK = threading.Lock()
BATCH = [0]
def pop(kind, wait_s=0.0):
    end = time.time() + wait_s
    while True:
        with LOCK:
            if Q[kind]: items = Q[kind][:]; Q[kind].clear(); return items
        if time.time() >= end: return []
        time.sleep(0.05)

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def handle_any(self, method):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0)).decode()
        try: body = json.loads(raw) if raw else {}
        except Exception: body = {k: v[0] for k, v in parse_qs(raw).items()}
        with open(LOG, "a") as f: f.write(json.dumps({"method": method, "path": self.path, "auth": self.headers.get("Authorization", ""), "body": body}) + "\n")
        p = self.path
        if p == "/__push":
            with LOCK: Q[body["kind"]].append(body["item"])
            return self.send(200, {"ok": True})
        if p.endswith("/oauth2/v2.0/token"):
            ok = body.get("client_id") == "app-1" and body.get("client_secret") == "pw-1"
            return self.send(200 if ok else 401, {"access_token": "teams-tok", "expires_in": 3600} if ok else {"error": "invalid_client"})
        if p == "/v2/send": return self.send(201, {"timestamp": "1"})
        if "/_matrix/client/v3/rooms/" in p:
            return self.send(200 if self.headers.get("Authorization") == "Bearer mx-tok" else 401, {"event_id": "$e1"})
        if "/v3/conversations/" in p:
            return self.send(200 if self.headers.get("Authorization") == "Bearer teams-tok" else 401, {"id": "a1"})
        if p.startswith("/v1/spaces/"): return self.send(200, {"name": "spaces/x/messages/1"})
        if p == "/api/v4/commands":
            return self.send(201, {"id": "cmd-1", "trigger": (body.get("trigger") if isinstance(body, dict) else ""), "url": body.get("url") if isinstance(body, dict) else ""})
        if p == "/api/v4/posts":
            return self.send(201 if self.headers.get("Authorization") == "Bearer mm-tok" else 401, {"id": "p1"})
        self.send(200, {"ok": True})
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        p = self.path
        if p.startswith("/openid"):
            return self.send(200, {"jwks_uri": "http://127.0.0.1:%d/jwks" % self.server.server_port})
        if p == "/jwks": return self.send(200, json.load(open(os.environ["FAKE_JWKS"])))
        if p.startswith("/_matrix/client/v3/sync"):
            if "since=" not in p:
                return self.send(200, {"next_batch": "s0", "rooms": {"join": {"!old:x": {"timeline": {"events": [{"type": "m.room.message", "sender": "@ann:x", "content": {"body": "backlog"}}]}}}}})
            evs = pop("matrix", 1.0); inv = pop("matrix_invite")
            BATCH[0] += 1
            rooms = {"join": {}, "invite": {r: {} for r in inv}}
            for e in evs: rooms["join"].setdefault(e.pop("room_id"), {"timeline": {"events": []}})["timeline"]["events"].append(e)
            return self.send(200, {"next_batch": "s%d" % BATCH[0], "rooms": rooms})
        if p.startswith("/v1/receive/"): return self.send(200, pop("signal", 0.5))
        if p == "/api/v4/users/me": return self.send(200, {"id": "botid"})
        if p.startswith("/api/v4/channels/") and "/posts" in p:
            now = int(time.time() * 1000)
            posts = {}
            for i, it in enumerate(pop("mattermost")): it.setdefault("create_at", now + i); posts[it["id"]] = it
            return self.send(200, {"order": list(posts), "posts": posts})
        if p.startswith("/api/v4/channels/"): return self.send(200, {"id": p.rsplit("/", 1)[1], "type": "O"})
        self.handle_any("GET")
    def do_POST(self): self.handle_any("POST")
    def do_PUT(self): self.handle_any("PUT")

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
