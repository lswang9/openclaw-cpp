#!/usr/bin/env python3
"""Offline fake for GitHub Copilot (device flow, copilot_internal/user, CAPI chat/completions) and OpenAI Codex
(deviceauth usercode/token, oauth/token code+refresh, backend-api/codex/responses). Usage: fake_oauth_llm.py <port> <log>"""
import json, sys, base64, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs
PORT, LOG = int(sys.argv[1]), sys.argv[2]
B = f"http://127.0.0.1:{PORT}"
ST = {"gh_polls": 0, "cx_polls": 0}; LK = threading.Lock()
def jwt(acct, tag):
    enc = lambda o: base64.urlsafe_b64encode(json.dumps(o).encode()).decode().rstrip("=")
    return enc({"alg": "none"}) + "." + enc({"https://api.openai.com/auth": {"chatgpt_account_id": acct}, "tag": tag}) + ".sig"
ACCESS1, ACCESS2 = jwt("acct-1", "a1"), jwt("acct-1", "a2")
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def sse(self, events):
        self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.end_headers()
        for ev, data in events: self.wfile.write(((f"event: {ev}\n" if ev else "") + "data: " + json.dumps(data) + "\n\n").encode()); self.wfile.flush()
    def rec(self, body):
        with open(LOG, "a") as f: f.write(json.dumps({"m": self.command, "path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "body": body}) + "\n")
    def do_HEAD(self):
        self.do_GET()
    def do_GET(self):
        self.rec(None)
        if self.path.startswith("/oauth/authorize"):
            from urllib.parse import urlparse, parse_qs, urlencode
            q=parse_qs(urlparse(self.path).query)
            redir=q.get("redirect_uri",[""])[0]; st=q.get("state",[""])[0]
            if not redir: return self.send(400, {"error": "redirect_uri"})
            loc=redir + (("&" if ("?" in redir) else "?") + urlencode({"code": "pkce-ac-1", "state": st}))
            self.send_response(302); self.send_header("Location", loc); self.send_header("Content-Length", "0"); self.end_headers(); return
        if self.path == "/copilot_internal/user":
            if self.headers.get("Authorization") != "Bearer gho_test": return self.send(401, {"message": "Bad credentials"})
            return self.send(200, {"login": "octo", "endpoints": {"api": B + "/capi"}})
        self.send(404, {})
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)) or 0)
        ct = self.headers.get("Content-Type", "")
        body = {k: v[0] for k, v in parse_qs(raw.decode()).items()} if "urlencoded" in ct else (json.loads(raw or b"{}"))
        self.rec(body); p = self.path
        if p == "/login/device/code":
            return self.send(200, {"device_code": "dc-1", "user_code": "ABCD-1234", "verification_uri": B + "/login/device", "interval": 1, "expires_in": 60})
        if p == "/login/oauth/access_token":
            assert body.get("grant_type") == "urn:ietf:params:oauth:grant-type:device_code" and body.get("device_code") == "dc-1"
            with LK: ST["gh_polls"] += 1; n = ST["gh_polls"]
            return self.send(200, {"error": "authorization_pending"} if n < 2 else {"access_token": "gho_test", "token_type": "bearer"})
        if p == "/capi/chat/completions":
            h = self.headers
            if h.get("Authorization") != "Bearer gho_test" or h.get("Copilot-Integration-Id") != "copilot-developer-cli" or not h.get("Editor-Version"):
                return self.send(401, {"error": {"message": "copilot auth headers missing"}})
            if body.get("stream"):
                return self.sse([(None, {"choices": [{"index": 0, "delta": {"content": "copilot-"}}]}), (None, {"choices": [{"index": 0, "delta": {"content": "ok:" + body["model"]}, "finish_reason": "stop"}]})])
            return self.send(200, {"choices": [{"index": 0, "message": {"role": "assistant", "content": "copilot-ok:" + body["model"]}, "finish_reason": "stop"}]})
        if p == "/api/accounts/deviceauth/usercode":
            if body.get("client_id") != "app_EMoamEEZ73f0CkXaXp7hrann": return self.send(400, {"error": "bad client"})
            return self.send(200, {"device_auth_id": "da-1", "user_code": "CODX-9", "interval": "1"})
        if p == "/api/accounts/deviceauth/token":
            with LK: ST["cx_polls"] += 1; n = ST["cx_polls"]
            if n < 2: return self.send(403, {"error": "pending"})
            return self.send(200, {"authorization_code": "ac-1", "code_verifier": "cv-1"})
        if p == "/oauth/token":
            g = body.get("grant_type")
            if g == "authorization_code" and body.get("code") == "ac-1" and body.get("code_verifier") == "cv-1" and body.get("redirect_uri", "").endswith("/deviceauth/callback"):
                return self.send(200, {"access_token": ACCESS1, "refresh_token": "rt-1", "id_token": ACCESS1, "expires_in": 1})
            if g == "authorization_code" and body.get("code") == "pkce-ac-1" and body.get("code_verifier") and "/auth/callback" in body.get("redirect_uri", ""):
                return self.send(200, {"access_token": ACCESS1, "refresh_token": "rt-pkce", "id_token": ACCESS1, "expires_in": 3600})
            if g == "refresh_token" and body.get("refresh_token") == "rt-1":
                return self.send(200, {"access_token": ACCESS2, "refresh_token": "rt-2", "expires_in": 3600})
            return self.send(400, {"error": "invalid_grant"})
        if p == "/codex/responses":
            h = self.headers
            if h.get("Authorization") != "Bearer " + ACCESS2 or h.get("chatgpt-account-id") != "acct-1" or h.get("originator") != "openclaw":
                return self.send(401, {"error": {"message": "codex auth mismatch"}})
            if body.get("stream"):
                return self.sse([("response.output_text.delta", {"type": "response.output_text.delta", "delta": "codex-"}), ("response.output_text.delta", {"type": "response.output_text.delta", "delta": "ok"}), ("response.completed", {"type": "response.completed"})])
            return self.send(200, {"status": "completed", "output": [{"type": "message", "content": [{"type": "output_text", "text": "codex-ok"}]}]})
        self.send(404, {"error": "no route " + p})
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
