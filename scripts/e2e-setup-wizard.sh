#!/usr/bin/env bash
# 1.117: openclaw.setup detect/verify/activate, auth/prepare/activate wizards, wizard.* engine, openclaw.chat/history, changes/approvals.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
trap _reap EXIT
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-setup-wizard] FAIL: $*"; tail -20 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
echo mock-resource > "$T/res.txt"; mkdir -p "$T/home" "$T/ws" "$T/repo"
( cd "$T/repo" && git init -q -b main && echo a > a.txt && git add a.txt && git -c user.name=t -c user.email=t@t commit -qm init )
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENAI_API_KEY=sk-test \
  HOME="$T/home" OPENCLAW_WORKSPACE="$T/ws" GIT_CONFIG_GLOBAL=/dev/null
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T
python3 - <<'P' || fail "setup"
import json, os, urllib.request
URL, T = os.environ["URL"], os.environ["T"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
d = ok("openclaw.setup.detect", {}); assert any(c["provider"] == "openai" and c["source"] == "env" for c in d["candidates"]), d
prov = next(c["provider"] for c in d["candidates"] if c["source"] == "runtime")
assert ok("openclaw.setup.verify", {"provider": prov})["ok"]
assert not ok("openclaw.setup.verify", {"provider": "nope"})["ok"]
assert not rpc("openclaw.setup.activate", {"provider": "nope"}).get("ok")
a = ok("openclaw.setup.activate", {"provider": prov}); assert a["activated"]
assert json.load(open(T + "/cfg.json"))["agents"]["defaults"]["model"] == prov
w = ok("openclaw.setup.auth.start", {"sessionId": "s1", "authChoice": "openai-api-key"}); assert w["step"]["id"] == "apiKey" and w["step"]["sensitive"]
assert not rpc("openclaw.setup.auth.start", {"sessionId": "s1", "authChoice": "openai-api-key"}).get("ok")
assert not rpc("wizard.next", {"sessionId": "s1", "answer": ""}).get("ok")
w = ok("wizard.next", {"sessionId": "s1", "answer": {"stepId": "apiKey", "value": "sk-new"}}); assert w["step"]["id"] == "baseUrl"
w = ok("wizard.next", {"sessionId": "s1", "answer": ""}); assert w["done"] and w["status"] == "done" and w["result"]["saved"], w
assert json.load(open(T + "/cfg.json"))["models"]["providers"]["openai"]["apiKey"] == "sk-new"
w = ok("wizard.start", {}); sid = w["sessionId"]; assert w["step"]["id"] == "workspace"
assert ok("wizard.status", {"sessionId": sid})["status"] == "running"
assert ok("wizard.cancel", {"sessionId": sid})["status"] == "cancelled"
assert not rpc("wizard.next", {"sessionId": sid, "answer": "x"}).get("ok")
act = ok("openclaw.setup.activate.start", {"sessionId": "s2", "provider": prov}); assert act["step"]["type"] == "confirm"
r = ok("openclaw.chat", {"sessionId": "s2", "wizardAnswer": True}); assert r["wizard"]["result"]["activated"], r
ch = ok("openclaw.changes.list", {})["changes"]; assert [c["kind"] for c in ch] == ["model.activate", "provider.auth", "model.activate"], ch
assert ok("openclaw.chat", {"message": "hi"})["reply"]
h = ok("openclaw.chat.history", {})["turns"]; assert len(h) >= 2 and h[-1]["role"] == "assistant", h
assert "approvals" in ok("openclaw.approval.list", {})
ok("openclaw.setup.prepare.start", {"sessionId": "s3", "provider": "deepseek", "model": "deepseek-chat"})
print("[e2e-setup-wizard] ok")
P
