#!/usr/bin/env python3
"""Offline fake Telegram Bot API. Usage: fake_telegram.py <port> <logfile>. POST /__push {update} queues an update
for getUpdates; every Bot API call is appended to the log as JSON."""
import json, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
PORT, LOG = int(sys.argv[1]), sys.argv[2]
Q = []; LOCK = threading.Condition(); NEXT = [1]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, obj, code=200):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def rec(self, method, args):
        with open(LOG, "a") as f: f.write(json.dumps({"method": method, "args": args}) + "\n")
    def handle_api(self, args):
        u = urlparse(self.path); parts = u.path.strip("/").split("/")
        if self.path == "/__push":
            with LOCK:
                upd = args; upd["update_id"] = NEXT[0]; NEXT[0] += 1; Q.append(upd); LOCK.notify_all()
            return self.send({"ok": True})
        if len(parts) != 2 or parts[0] != "botTT-1:abc": return self.send({"ok": False, "error_code": 401, "description": "Unauthorized"}, 401)
        m = parts[1]; self.rec(m, args)
        if m == "getUpdates":
            off = int(args.get("offset", 0)); to = int(args.get("timeout", 0)); end = time.time() + min(to, 2)
            with LOCK:
                while True:
                    res = [x for x in Q if x["update_id"] >= off]
                    if res or time.time() >= end: break
                    LOCK.wait(0.1)
            return self.send({"ok": True, "result": res})
        if m == "sendMessage":
            if len(args.get("text", "")) > 4096: return self.send({"ok": False, "error_code": 400, "description": "message is too long"}, 400)
            return self.send({"ok": True, "result": {"message_id": 100, "chat": {"id": args.get("chat_id")}}})
        return self.send({"ok": True, "result": True})
    def do_GET(self):
        q = {k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()}; self.handle_api(q)
    def do_POST(self):
        b = self.rfile.read(int(self.headers.get("Content-Length", 0))); 
        try: args = json.loads(b or b"{}")
        except Exception: args = {}
        self.handle_api(args)
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
