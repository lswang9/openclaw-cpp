#!/usr/bin/env bash
# 1.119: Web Push (VAPID + RFC 8291 aes128gcm, decrypted by a fake push service), users.authConnect, tools.github identity (managed PAT, inherit, device flow).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""; PS=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FK" ]] && kill $FK 2>/dev/null; [[ -n "$PS" ]] && kill $PS 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-push-identity] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
python3 "$ROOT/scripts/fake-github.py" >"$T/fk.port" 2>"$T/fk.log" & FK=$!
cat > "$T/ps.py" <<'P'
import json, sys, base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization, hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
import os
priv = ec.generate_private_key(ec.SECP256R1()); auth = os.urandom(16)
pub = priv.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
GOT = []
def hk(salt, ikm, info, n): return HKDF(hashes.SHA256(), n, salt, info).derive(ikm)
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        b = json.dumps({"got": GOT, "p256dh": b64(pub), "auth": b64(auth)}).encode(); self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if self.path == "/gone": self.send_response(410); self.end_headers(); return
        salt, idlen = body[:16], body[20]; aspub = body[21:21 + idlen]; ct = body[21 + idlen:]
        shared = priv.exchange(ec.ECDH(), ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), aspub))
        ikm = hk(auth, shared, b"WebPush: info\0" + pub + aspub, 32)
        cek = hk(salt, ikm, b"Content-Encoding: aes128gcm\0", 16); nonce = hk(salt, ikm, b"Content-Encoding: nonce\0", 12)
        pt = AESGCM(cek).decrypt(nonce, ct, None)
        assert pt.endswith(b"\x02")
        a = self.headers.get("Authorization", "")
        GOT.append({"payload": json.loads(pt[:-1]), "vapid": a.startswith("vapid t="), "enc": self.headers.get("Content-Encoding")})
        self.send_response(201); self.end_headers()
s = ThreadingHTTPServer(("127.0.0.1", 0), H); print(s.server_address[1], flush=True); s.serve_forever()
P
python3 "$T/ps.py" >"$T/ps.port" 2>"$T/ps.log" & PS=$!
for i in $(seq 1 50); do [[ -s "$T/fk.port" && -s "$T/ps.port" ]] && break; sleep 0.1; done
FPORT=$(cat "$T/fk.port"); export PSPORT=$(cat "$T/ps.port")
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home" "$T/ws"
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" HOME="$T/home" \
  OPENCLAW_WORKSPACE="$T/ws" OPENCLAW_GITHUB_API_BASE_URL="http://127.0.0.1:$FPORT" GIT_CONFIG_GLOBAL=/dev/null \
  OPENCLAW_GITHUB_OAUTH_BASE_URL="http://127.0.0.1:$FPORT" OPENCLAW_GITHUB_OAUTH_CLIENT_ID=oc-client GH_SETUP_HANDLE_1=good-token
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T
python3 - <<'P' || fail "push/identity"
import json, os, urllib.request, time
URL, T, PS = os.environ["URL"], os.environ["T"], int(os.environ["PSPORT"])
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
k = ok("push.web.vapidPublicKey", {})["publicKey"]; assert len(k) == 87 and ok("push.web.vapidPublicKey", {})["publicKey"] == k
assert not rpc("push.web.test", {}).get("ok")
info = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/" % PS).read())
ep = "http://127.0.0.1:%d/push/1" % PS
ok("push.web.subscribe", {"subscription": {"endpoint": ep, "keys": {"p256dh": info["p256dh"], "auth": info["auth"]}}})
ok("push.web.subscribe", {"subscription": {"endpoint": "http://127.0.0.1:%d/gone" % PS, "keys": {"p256dh": info["p256dh"], "auth": info["auth"]}}})
assert not rpc("push.web.subscribe", {"subscription": {"endpoint": "http://evil.example/x", "keys": {"p256dh": "a", "auth": "b"}}}).get("ok")
assert ok("push.web.preferences.set", {"endpoint": ep, "preferences": {"chat": False}})["user"] == {"chat": False}
assert ok("push.web.preferences.get", {"endpoint": ep})["user"] == {"chat": False}
r = ok("push.web.test", {"title": "T1", "body": "B1"})["results"]
assert any(x["ok"] for x in r) and any(x.get("expired") for x in r), r
got = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/" % PS).read())["got"]
assert got == [{"payload": {"title": "T1", "body": "B1"}, "vapid": True, "enc": "aes128gcm"}], got
assert len(ok("push.web.test", {})["results"]) == 1
assert ok("push.web.unsubscribe", {"endpoint": ep})["unsubscribed"]
assert not rpc("push.test", {"nodeId": "n1"}).get("ok")
# authConnect
cat = ok("users.authConnect.catalog", {}); assert any(p["provider"] == "openai" for p in cat["providers"])
c = ok("users.authConnect.start", {"provider": "openai"}); cid = c["connectId"]; assert c["step"]["stepId"] == "apiKey"
assert not rpc("users.authConnect.answer", {"connectId": cid, "stepId": "x", "value": "sk-1234567890"}).get("ok")
a = ok("users.authConnect.answer", {"connectId": cid, "stepId": "apiKey", "value": "sk-1234567890"}); assert a["status"] == "connected" and a["authProfileId"].startswith("openai:user-"), a
assert ok("users.authConnect.status", {"connectId": cid})["status"] == "connected"
assert a["authProfileId"] in ok("users.listAuthLinks", {})["links"]
c2 = ok("users.authConnect.start", {"provider": "anthropic"}); assert ok("users.authConnect.cancel", {"connectId": c2["connectId"]})["status"] == "cancelled"
assert not rpc("users.authConnect.start", {"provider": "openai", "method": "oauth"}).get("ok")
# tools.github
s = ok("tools.github.status", {"agentId": "main", "selectedScope": "agent"}); assert not s["selected"]["configured"]
assert not rpc("tools.github.configure", {"agentId": "main", "scope": "agent", "mode": "managed", "secretName": "NOPE"}).get("ok")
c = ok("tools.github.configure", {"agentId": "main", "scope": "agent", "mode": "managed", "secretName": "GH_SETUP_HANDLE_1", "gitAuthor": {"name": "Bot", "email": "b@x"}})
assert c["account"]["login"] == "octo"
s = ok("tools.github.status", {"agentId": "main", "selectedScope": "agent"}); assert s["selected"]["identity"]["account"]["login"] == "octo" and s["effective"]["gitAuthor"]["name"] == "Bot", s
ok("tools.github.configure", {"agentId": "main", "scope": "agent", "mode": "inherit"})
assert not ok("tools.github.status", {"agentId": "main", "selectedScope": "agent"})["selected"]["configured"]
st = ok("tools.github.authorize.start", {"agentId": "ops", "scope": "agent"}); assert st["requestId"].startswith("github-device-") and len(st["requestId"]) == 46, st
res = None
for _ in range(10):
    res = ok("tools.github.authorize.poll", {"requestId": st["requestId"]})
    if res["status"] == "success": break
    time.sleep(1.1)
assert res["status"] == "success", res
assert ok("tools.github.status", {"agentId": "ops", "selectedScope": "agent"})["selected"]["identity"]["account"]["login"] == "alice"
assert ok("users.github.status", {})["personal"]["state"] == "disconnected"
assert not ok("tools.github.authorize.cancel", {"requestId": st["requestId"]})["cancelled"]
print("[e2e-push-identity] ok")
P
