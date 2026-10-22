#!/usr/bin/env bash
# 1.121: mcp.authLogin / models.authLogin OAuth 2.1 (discovery, dynamic registration, PKCE S256, /oauth/callback, paste-back) against a fake auth server.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FO=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FO" ]] && kill $FO 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true; _reap' EXIT
fail() { echo "[e2e-oauth-login] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
python3 -u "$ROOT/scripts/fake-oauth.py" >"$T/fo.port" 2>"$T/fo.log" & FO=$!
for i in $(seq 1 50); do [[ -s "$T/fo.port" ]] && break; sleep 0.1; done
OP=$(cat "$T/fo.port"); PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
cat > "$T/cfg.json" <<C
{"mcp": {"servers": {"demo": {"url": "http://127.0.0.1:$OP/mcp"}, "local": {"command": "true"}}},
 "models": {"providers": {"acme": {"baseUrl": "http://127.0.0.1:$OP/v1", "oauth": {"authorizationUrl": "http://127.0.0.1:$OP/authorize", "tokenUrl": "http://127.0.0.1:$OP/token", "clientId": "static-client", "scopes": ["models.read", "offline"]}}}}}
C
mkdir -p "$T/home"
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/cfg.json" HOME="$T/home"
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T OP
python3 - <<'P' || fail "oauth login"
import json, os, urllib.request
URL, T, OP = os.environ["URL"], os.environ["T"], os.environ["OP"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id": 1, "method": m, "params": p}).encode(), {"Content-Type": "application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
def err(m, p, s):
    r = rpc(m, p); assert not r.get("ok") and s in json.dumps(r), (m, r)
class NoRedir(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a): return None
raw = urllib.request.build_opener(NoRedir)
def authorize(url):  # browser: hit the auth server, get the 302 Location (the gateway callback URL)
    try: raw.open(url, timeout=10); raise AssertionError("expected redirect")
    except urllib.error.HTTPError as e: assert e.code == 302, e.code; return e.headers["Location"]
# ---- MCP: protected-resource discovery + dynamic client registration; the browser follows the redirect to the gateway's /oauth/callback
err("mcp.authLogin", {"sessionId": "s0", "serverName": "local"}, "cannot use operator browser sign-in")
err("mcp.authLogin", {"sessionId": "s0", "serverName": "ghost"}, "cannot use operator browser sign-in")
st = ok("mcp.authLogin", {"sessionId": "mcp1", "serverName": "demo"})
assert st["step"]["type"] == "openUrl" and "code_challenge_method=S256" in st["step"]["url"] and "client_id=dyn-" in st["step"]["url"], st
err("mcp.authLogin", {"sessionId": "mcp1", "serverName": "demo"}, "already running")
cb = authorize(st["step"]["url"]); assert "/oauth/callback?code=" in cb, cb
html = urllib.request.urlopen(cb, timeout=30).read().decode(); assert "Signed in" in html, html
w = ok("wizard.status", {"sessionId": "mcp1"}); assert w["status"] == "done" and w["result"]["verified"] is True and w["result"]["authorized"], w
tok = json.load(open(T + "/data/meta/mcp-oauth-tokens.json")) if os.path.exists(T + "/data/meta/mcp-oauth-tokens.json") else None
# replayed callback: session finished → expired page, no crash
assert "expired" in urllib.request.urlopen(cb, timeout=30).read().decode()
# ---- models: explicit endpoints + static client; paste-back of the redirected URL through wizard.next; state is checked
st = ok("models.authLogin", {"sessionId": "mod1", "authChoice": "acme:oauth", "agentId": "main"})
u = st["step"]["url"]; assert "scope=models.read%20offline" in u and "client_id=static-client" in u, u
cb = authorize(u)
w = ok("wizard.next", {"sessionId": "mod1", "answer": cb.replace("state=", "state=x")})  # state mismatch → wizard error
assert ok("wizard.status", {"sessionId": "mod1"}) == w and w["status"] == "error" and "state mismatch" in w["error"], w
st = ok("models.authLogin", {"sessionId": "mod2", "authChoice": "acme:oauth"})
w = ok("wizard.next", {"sessionId": "mod2", "answer": authorize(st["step"]["url"])})
assert w["status"] == "done" and w["result"]["profileId"] == "acme:oauth" and w["result"]["hasRefreshToken"], w
cfg = ok("config.get", {}); s = json.dumps(cfg); assert '"at-' in s or "apiKey" in s, "token saved to provider config"
err("models.authLogin", {"sessionId": "m3", "authChoice": "nope:oauth"}, "no longer available")
# ---- users.authConnect method=oauth: catalog advertises it, paste-back stores an oauth auth profile
cat = ok("users.authConnect.catalog", {})["providers"]; assert any(p["provider"] == "acme" and any(m["id"] == "oauth" for m in p["methods"]) for p in cat), cat
err("users.authConnect.start", {"provider": "openai", "method": "oauth"}, "not available")
c = ok("users.authConnect.start", {"provider": "acme", "method": "oauth"}); assert c["step"]["kind"] == "redirect" and "oauth" not in c, c
r = ok("users.authConnect.answer", {"connectId": c["connectId"], "stepId": "authorize", "value": authorize(c["step"]["url"])})
assert r["status"] == "connected" and r["authProfileId"].startswith("acme:user-"), r
lg = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{OP}/_log").read())
assert sum(1 for x in lg["log"] if "token" in x) == 3 and any(c.startswith("dyn-") for c in lg["clients"]), lg
print("ok")
P
echo "[e2e-oauth-login] mcp.authLogin (RFC 9728/8414 discovery, DCR, PKCE, /oauth/callback, bearer verify) + models.authLogin (paste-back, state check): PASS"
