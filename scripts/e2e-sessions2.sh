#!/usr/bin/env bash
# 1.117: sessions archive catalog, import/continue, startTerminal, files.reveal, github options/status, pluginPatch, providerReview, recover, activitySummary.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-sessions2] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
echo mock-resource > "$T/res.txt"; mkdir -p "$T/home" "$T/ws" "$T/repo"
( cd "$T/repo" && git init -q -b main && echo a > a.txt && git add a.txt && git -c user.name=t -c user.email=t@t commit -qm init )
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" \
  HOME="$T/home" OPENCLAW_WORKSPACE="$T/ws" GIT_CONFIG_GLOBAL=/dev/null
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T
python3 - <<'P' || fail "sessions2"
import json, os, urllib.request, subprocess
URL, T = os.environ["URL"], os.environ["T"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
K = "agent:main:s2"
ok("chat.send", {"sessionKey": K, "message": "hello"})
s = ok("sessions.activitySummary.ensure", {"sessionKey": K}); assert s["summary"]["roles"]["user"] == 1 and not s["cached"], s
assert ok("sessions.activitySummary.ensure", {"sessionKey": K})["cached"]
r = ok("sessions.recover", {"key": K}); assert r["messages"] >= 2, r
assert ok("sessions.pluginPatch", {"key": K, "pluginId": "sample_echo", "patch": {"a": 1}})["state"] == {"a": 1}
assert not rpc("sessions.pluginPatch", {"key": K, "pluginId": "nope", "patch": {}}).get("ok")
assert not rpc("sessions.providerReview.continue", {"sessionKey": K}).get("ok")
ws = T + "/ws"; open(ws + "/f.txt", "w").write("x")
assert ok("sessions.files.reveal", {"sessionKey": K, "path": "f.txt"})["path"] == ws + "/f.txt"
assert not rpc("sessions.files.reveal", {"sessionKey": K, "path": "/etc/passwd"}).get("ok")
g0 = ok("sessions.github.status", {"sessionKey": K}); assert g0["available"] is False, g0
subprocess.run(["git", "init", "-q", "-b", "main", ws], check=True)
st = ok("sessions.github.status", {"sessionKey": K}); assert st["available"] and any(c["path"] == "f.txt" for c in st["changed"]), st
assert ok("sessions.github.options", {"sessionKey": K})["branch"]
t = ok("sessions.catalog.startTerminal", {"sessionKey": K}); ok("terminal.close", {"sessionId": t["sessionId"]})
a = ok("sessions.catalog.archive", {"catalogId": "local", "sessionKey": K}); aid = a["archiveId"]
cats = ok("sessions.catalog.list", {})["catalogs"]; assert any(x["archiveId"] == aid for c in cats if c["id"] == "archive" for x in c["sessions"]), cats
assert not rpc("sessions.recover", {"key": K}).get("ok")
c = ok("sessions.catalog.continue", {"archiveId": aid, "message": "again"}); assert c["sessionKey"] == K and c["reply"], c
assert len(ok("sessions.catalog.read", {"sessionKey": K})["messages"]) >= 4
i = ok("sessions.catalog.import", {"sessionKey": "agent:main:imp", "messages": [{"role": "user", "content": "hi"}, {"role": "assistant", "content": "yo"}]}); assert i["imported"] == 2
assert not rpc("sessions.catalog.import", {"sessionKey": "agent:main:imp", "messages": []}).get("ok")
print("[e2e-sessions2] ok")
P
