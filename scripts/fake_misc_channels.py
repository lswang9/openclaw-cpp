#!/usr/bin/env python3
"""Offline fake HTTP backends for the 16 misc channels. Usage: fake_misc_channels.py <port> <logfile>"""
import base64, json, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs
PORT, LOG = int(sys.argv[1]), sys.argv[2]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)) or 0)
        ctype = self.headers.get("Content-Type", "")
        auth = self.headers.get("Authorization", "")
        if "application/x-www-form-urlencoded" in ctype:
            form = {k: v[0] for k, v in parse_qs(raw.decode()).items()}
            with open(LOG, "a") as f: f.write(json.dumps({"path": self.path, "auth": auth, "form": form, "headers": {k.lower(): v for k, v in self.headers.items()}}) + "\n")
            if "/Messages.json" in self.path:
                if not auth.startswith("Basic "): return self.send(401, {"error": "auth"})
                return self.send(201, {"sid": "SM1", "status": "queued", "to": form.get("To"), "body": form.get("Body")})
            return self.send(404, {"error": "form"})
        body = {}
        try: body = json.loads(raw or b"{}")
        except Exception: pass
        with open(LOG, "a") as f: f.write(json.dumps({"path": self.path, "auth": auth, "body": body, "headers": {k.lower(): v for k, v in self.headers.items()}}) + "\n")
        p = self.path
        if p.endswith("/v2/bot/message/reply") or p.endswith("/v2/bot/message/push"):
            if auth != "Bearer line-tok": return self.send(401, {"message": "Unauthorized"})
            return self.send(200, {"ok": True})
        if "/bot" in p and p.endswith("/sendMessage"):
            return self.send(200, {"ok": True, "result": {"message_id": 1}})
        if p.endswith("/helix/chat/messages"):
            if auth != "Bearer tw-tok" or self.headers.get("Client-Id") != "tw-cid": return self.send(401, {"error": "auth"})
            return self.send(200, {"data": [{"message_id": "m1"}]})
        if "/2/dm_conversations/with/" in p and p.endswith("/messages"):
            if auth != "Bearer x-tok": return self.send(401, {"error": "auth"})
            return self.send(201, {"data": {"dm_event_id": "d1"}})
        if "/ocs/v2.php/apps/spreed/api/v1/chat/" in p:
            if not auth.startswith("Basic ") or self.headers.get("OCS-APIRequest") != "true": return self.send(401, {"error": "auth"})
            return self.send(201, {"ocs": {"meta": {"statuscode": 201}}})
        if p.endswith("/incoming"):  # synology
            return self.send(200, {"ok": True})
        # generic bridge paths
        for suf in ("/privmsg", "/dm", "/v1/messages", "/message", "/send"):
            if p.endswith(suf):
                if auth and auth != "Bearer bridge-tok": return self.send(401, {"error": "auth"})
                return self.send(200, {"ok": True})
        self.send(404, {"error": "no route " + p})
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
