#!/usr/bin/env python3
"""Offline fake OAuth 2.1 authorization server + protected MCP resource for e2e tests.
RFC 9728 /.well-known/oauth-protected-resource, RFC 8414 metadata, RFC 7591 /register, /authorize (auto-consents, 302 to redirect_uri),
/token (authorization_code with PKCE S256 verification), /mcp (JSON-RPC, requires the issued bearer). Prints its port."""
import base64, hashlib, json, secrets, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, urlencode
CLIENTS, CODES, TOKENS, LOG = {"static-client": {"redirect_uris": None}}, {}, set(), []
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj, hdr=None):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        for k, v in (hdr or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(b)
    def base(self): return f"http://127.0.0.1:{self.server.server_address[1]}"
    def do_GET(self):
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/.well-known/oauth-protected-resource": return self.send(200, {"resource": self.base() + "/mcp", "authorization_servers": [self.base()]})
        if u.path == "/.well-known/oauth-authorization-server":
            return self.send(200, {"issuer": self.base(), "authorization_endpoint": self.base() + "/authorize", "token_endpoint": self.base() + "/token",
                                   "registration_endpoint": self.base() + "/register", "code_challenge_methods_supported": ["S256"]})
        if u.path == "/authorize":
            LOG.append({"authorize": q})
            if q.get("client_id") not in CLIENTS or q.get("code_challenge_method") != "S256" or not q.get("code_challenge"): return self.send(400, {"error": "invalid_request"})
            code = secrets.token_urlsafe(16); CODES[code] = q
            self.send_response(302); self.send_header("Location", q["redirect_uri"] + "?" + urlencode({"code": code, "state": q.get("state", "")})); self.end_headers(); return
        if u.path == "/_log": return self.send(200, {"log": LOG, "clients": list(CLIENTS)})
        self.send(404, {"error": "not_found"})
    def do_POST(self):
        u = urlparse(self.path); raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if u.path == "/register":
            j = json.loads(raw); cid = "dyn-" + secrets.token_hex(4); CLIENTS[cid] = j; return self.send(201, {"client_id": cid, **j})
        if u.path == "/token":
            f = {k: v[0] for k, v in parse_qs(raw.decode()).items()}; LOG.append({"token": {k: v for k, v in f.items() if k != "code_verifier"}})
            c = CODES.pop(f.get("code", ""), None)
            if not c or c["client_id"] != f.get("client_id") or c["redirect_uri"] != f.get("redirect_uri"): return self.send(400, {"error": "invalid_grant"})
            ch = base64.urlsafe_b64encode(hashlib.sha256(f.get("code_verifier", "").encode()).digest()).rstrip(b"=").decode()
            if ch != c["code_challenge"]: return self.send(400, {"error": "invalid_grant", "error_description": "PKCE verification failed"})
            tok = "at-" + secrets.token_hex(8); TOKENS.add(tok)
            return self.send(200, {"access_token": tok, "token_type": "Bearer", "expires_in": 3600, "refresh_token": "rt-" + secrets.token_hex(8), "scope": c.get("scope", "")})
        if u.path == "/mcp":
            a = self.headers.get("Authorization", "")
            if not a.startswith("Bearer ") or a[7:] not in TOKENS:
                return self.send(401, {"error": "unauthorized"}, {"WWW-Authenticate": f'Bearer resource_metadata="{self.base()}/.well-known/oauth-protected-resource"'})
            j = json.loads(raw); return self.send(200, {"jsonrpc": "2.0", "id": j.get("id"), "result": {"protocolVersion": "2025-06-18", "capabilities": {}, "serverInfo": {"name": "fake-protected"}}})
        self.send(404, {"error": "not_found"})
s = ThreadingHTTPServer(("127.0.0.1", 0), H); print(s.server_address[1], flush=True); s.serve_forever()
