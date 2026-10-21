#!/usr/bin/env python3
"""Offline fake Discord v10 / Slack Web API / Telegram Bot API subset for directory e2e tests. Prints its port."""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
D_USERS = [{"user": {"id": "u1", "username": "alice", "global_name": "Alice A"}}, {"user": {"id": "u2", "username": "bob"}, "nick": "Bobby"},
           {"user": {"id": "b1", "username": "clawbot", "bot": True}}]
S_USERS = [{"id": "U1", "name": "alice", "real_name": "Alice Slack"}, {"id": "U2", "name": "carol", "real_name": "Carol C"}, {"id": "U3", "name": "gone", "deleted": True}]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}; p = u.path; a = self.headers.get("Authorization", "")
        if p.startswith("/discord/"):
            if a != "Bot dtok": return self.send(401, {"message": "401: Unauthorized"})
            p = p[len("/discord"):]
            if p == "/users/@me": return self.send(200, {"id": "b1", "username": "clawbot", "bot": True})
            if p == "/users/@me/guilds": return self.send(200, [{"id": "g1", "name": "Claw Guild"}])
            if p == "/guilds/g1/channels": return self.send(200, [{"id": "c1", "name": "general", "type": 0}, {"id": "v1", "name": "voice", "type": 2}, {"id": "c2", "name": "dev-chat", "type": 0}])
            if p == "/channels/c1": return self.send(200, {"id": "c1", "guild_id": "g1"})
            if p == "/guilds/g1/members": return self.send(200, D_USERS[: int(q.get("limit", "1000"))])
            if p == "/guilds/g1/members/search": return self.send(200, [m for m in D_USERS if q.get("query", "").lower() in m["user"]["username"]])
            return self.send(404, {"message": "Unknown"})
        if p.startswith("/slack/"):
            if a != "Bearer stok": return self.send(200, {"ok": False, "error": "invalid_auth"})
            m = p[len("/slack/"):]
            if m == "auth.test": return self.send(200, {"ok": True, "user_id": "UBOT", "user": "clawbot", "team": "Claw Team", "team_id": "T1"})
            if m == "users.list":
                if q.get("cursor") == "p2": return self.send(200, {"ok": True, "members": S_USERS[2:], "response_metadata": {"next_cursor": ""}})
                return self.send(200, {"ok": True, "members": S_USERS[:2], "response_metadata": {"next_cursor": "p2"}})
            if m == "conversations.list": return self.send(200, {"ok": True, "channels": [{"id": "C1", "name": "general"}, {"id": "C2", "name": "secret", "is_private": True}]})
            if m == "conversations.members": return self.send(200, {"ok": True, "members": ["U1", "U2"]} if q.get("channel") == "C1" else {"ok": False, "error": "channel_not_found"})
            return self.send(200, {"ok": False, "error": "unknown_method"})
        if p == "/telegram/botttok/getMe": return self.send(200, {"ok": True, "result": {"id": 777, "is_bot": True, "first_name": "Claw", "username": "claw_bot"}})
        self.send(404, {"error": "not found"})
s = ThreadingHTTPServer(("127.0.0.1", 0), H); print(s.server_address[1], flush=True); s.serve_forever()
