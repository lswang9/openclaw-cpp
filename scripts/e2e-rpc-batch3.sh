#!/usr/bin/env bash
# 1.112: terminals (PTY), session processes, model auth profiles, usage, web search status, environments, session storage/catalog/title. (was 1.110: task suggestions, progress cards, cron scratch, node pending queue + capability updates, session metadata, chat.message.get, toolTitles, mentions, diff, transcripts.export, gateway identity, wake.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
trap _reap EXIT
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-rpc-batch3] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home" "$T/ws" "$T/repo"
( cd "$T/repo" && git init -q -b main && echo a > a.txt && git add a.txt && git -c user.name=t -c user.email=t@t commit -qm init )
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" \
  HOME="$T/home" OPENCLAW_WORKSPACE="$T/ws" GIT_CONFIG_GLOBAL=/dev/null
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T
python3 - <<'P' || fail "rpc groups"
import json, os, urllib.request, subprocess
URL, T = os.environ["URL"], os.environ["T"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
ws = T + "/ws"
t = ok("terminal.open", {"cwd": ws, "cols": 100, "rows": 30, "shell": "/bin/sh"})
sid = t["sessionId"]; assert t["cols"] == 100
ok("terminal.input", {"sessionId": sid, "data": "echo term-$((40+2))\n"})
import time
buf = ""
for _ in range(50):
    buf = ok("terminal.attach", {"sessionId": sid})["buffer"]
    if "term-42" in buf: break
    time.sleep(0.1)
assert "term-42" in buf, buf
assert ok("terminal.resize", {"sessionId": sid, "cols": 120, "rows": 40})["cols"] == 120
open(T + "/up.txt", "w").write("u")
assert ok("terminal.upload", {"sessionId": sid, "name": "up.txt", "path": T + "/up.txt", "size": 1})["path"] == ws + "/up.txt"
tl = ok("terminal.list", {})["sessions"]; assert any(x["sessionId"] == sid and x["running"] for x in tl), tl
ok("terminal.close", {"sessionId": sid}); assert not rpc("terminal.attach", {"sessionId": sid}).get("ok")
assert "processes" in ok("sessions.processes.list", {})
ok("models.authSetApiKey", {"provider": "openai", "apiKey": "sk-test-123"})
st = ok("models.authStatus", {}); assert st["providers"]["openai"]["profiles"][0]["id"] == "openai:default" and "sk-test" not in json.dumps(st)
assert ok("models.authOrderSet", {"provider": "openai", "profileIds": ["openai:default"]})["order"] == ["openai:default"]
assert ok("models.authLogout", {"provider": "openai"})["removed"] == ["openai:default"]
assert "profiles" in ok("models.authRefresh", {})
ok("chat.send", {"sessionKey": "u1", "message": "count me"})
assert ok("usage.status", {})["messages"] >= 2 and ok("sessions.usage.logs", {"sessionKey": "u1"})["entries"][0]["sessionKey"] == "u1"
assert ok("sessions.usage.timeseries", {})["points"] and "inputTokens" in ok("usage.cost", {})
assert ok("webSearch.status", {})["configured"] is False and not rpc("webSearch.test", {"query": "x"}).get("ok")
e = ok("environments.create", {"idempotencyKey": "k1"}); assert ok("environments.create", {"idempotencyKey": "k1"})["id"] == e["id"]
x = ok("environments.session.exec", {"environmentId": e["id"], "argv": ["sh", "-c", "pwd; echo hi"]}); assert x["exitCode"] == 0 and e["id"] in x["stdout"]
assert ok("environments.status", {"environmentId": e["id"]})["status"] == "ready"
ok("environments.destroy", {"environmentId": e["id"]}); assert not rpc("environments.status", {"environmentId": e["id"]}).get("ok")
assert ok("sessions.title.prepare", {"message": "Fix the login bug in the gateway\nmore"})["title"] == "Fix the login bug in the gateway"
assert ok("sessions.storage.status", {})["sessions"] >= 1 and ok("sessions.storage.run", {})["ran"]
assert any(s2["sessionKey"] == "u1" for s2 in ok("sessions.catalog.list", {})["catalogs"][0]["sessions"])
assert ok("sessions.catalog.read", {"sessionKey": "u1", "limit": 1})["nextCursor"] == "1"
assert not rpc("device.token.revoke", {"deviceId": "nope"}).get("ok")
print("ok")
P
echo "[e2e-rpc-batch3] terminal pty, processes, models.auth*, usage, webSearch, environments, sessions storage/catalog/title: PASS"

