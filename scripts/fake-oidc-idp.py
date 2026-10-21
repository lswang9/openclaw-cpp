#!/usr/bin/env python3
"""Minimal fake OIDC IdP for tests: discovery, auto-approve /authorize, /token with PKCE S256 check, JWKS, RS256 id_token.
Env: IDP_PORT, IDP_EMAIL, IDP_SUB, IDP_CLIENT_ID, IDP_BAD_SIG=1 (sign with a different key)."""
import base64, hashlib, json, os, secrets, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from cryptography.hazmat.primitives.asymmetric import rsa, padding
from cryptography.hazmat.primitives import hashes

PORT = int(os.environ.get("IDP_PORT", "0"))
ISS = f"http://127.0.0.1:{PORT}"
KEY = rsa.generate_private_key(public_exponent=65537, key_size=2048)
OTHER = rsa.generate_private_key(public_exponent=65537, key_size=2048)
CODES = {}
b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
def i2b(n): return n.to_bytes((n.bit_length() + 7) // 8, "big")
def jwt(claims):
    h = b64(json.dumps({"alg": "RS256", "kid": "k1", "typ": "JWT"}).encode()); p = b64(json.dumps(claims).encode())
    k = OTHER if os.environ.get("IDP_BAD_SIG") == "1" else KEY
    return f"{h}.{p}." + b64(k.sign(f"{h}.{p}".encode(), padding.PKCS1v15(), hashes.SHA256()))

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def j(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = dict(urllib.parse.parse_qsl(u.query))
        if u.path == "/.well-known/openid-configuration":
            return self.j(200, {"issuer": ISS, "authorization_endpoint": ISS + "/authorize", "token_endpoint": ISS + "/token", "jwks_uri": ISS + "/jwks"})
        if u.path == "/jwks":
            n = KEY.public_key().public_numbers()
            return self.j(200, {"keys": [{"kty": "RSA", "kid": "k1", "alg": "RS256", "n": b64(i2b(n.n)), "e": b64(i2b(n.e))}]})
        if u.path == "/authorize":
            if q.get("code_challenge_method") != "S256" or q.get("response_type") != "code": return self.j(400, {"error": "bad request"})
            code = secrets.token_urlsafe(16); CODES[code] = q
            loc = q["redirect_uri"] + "?" + urllib.parse.urlencode({"code": code, "state": q["state"]})
            self.send_response(302); self.send_header("Location", loc); self.end_headers(); return
        self.j(404, {})
    def do_POST(self):
        body = dict(urllib.parse.parse_qsl(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()))
        q = CODES.pop(body.get("code", ""), None)
        if not q: return self.j(400, {"error": "invalid_grant"})
        if b64(hashlib.sha256(body.get("code_verifier", "").encode()).digest()) != q["code_challenge"]: return self.j(400, {"error": "pkce"})
        if body.get("redirect_uri") != q["redirect_uri"] or body.get("client_id") != q["client_id"]: return self.j(400, {"error": "mismatch"})
        now = int(time.time())
        claims = {"iss": ISS, "aud": q["client_id"], "sub": os.environ.get("IDP_SUB", "user-1"), "email": os.environ.get("IDP_EMAIL", "a@example.com"),
                  "email_verified": True, "iat": now, "exp": now + 300, "nonce": q["nonce"]}
        self.j(200, {"access_token": "at", "token_type": "Bearer", "id_token": jwt(claims)})

s = ThreadingHTTPServer(("127.0.0.1", PORT), H)
print(s.server_address[1], flush=True)
s.serve_forever()
