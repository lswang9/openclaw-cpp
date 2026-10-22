#!/usr/bin/env bash
# 1.116: talk.* voice sessions (mock STT -> agent turn), talk.client.*, mcp.app view errors without MCP server.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
trap _reap EXIT
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-talk-mcpapp] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
echo mock-resource > "$T/res.txt"; mkdir -p "$T/home" "$T/ws" "$T/repo"
( cd "$T/repo" && git init -q -b main && echo a > a.txt && git add a.txt && git -c user.name=t -c user.email=t@t commit -qm init )
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_STT_PROVIDER=mock OPENCLAW_MCP_APP_POLL_MS=100 OPENCLAW_MCP_MOCK_RESOURCE_FILE="$T/res.txt" OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" \
  HOME="$T/home" OPENCLAW_WORKSPACE="$T/ws" GIT_CONFIG_GLOBAL=/dev/null
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T
python3 - <<'P' || fail "talk/mcpapp"
import json, os, urllib.request, base64
URL = os.environ["URL"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
assert ok("talk.config", {})["stt"]["configured"]
assert ok("talk.mode", {"enabled": True})["enabled"] is True
s = ok("talk.session.create", {"sessionKey": "agent:main:talk"}); sid = s["sessionId"]
b = lambda x: base64.b64encode(x.encode()).decode()
assert ok("talk.session.appendAudio", {"sessionId": sid, "audio": b("hello ")})["buffered"] == 6
t = ok("talk.session.appendAudio", {"sessionId": sid, "audio": b("voice"), "commit": True})
assert t["transcript"] == "hello voice" and t["reply"], t
st = ok("talk.session.steer", {"sessionId": sid, "text": "go on"}); assert st["reply"]
ok("talk.session.acknowledgeMark", {"sessionId": sid, "markName": "m1"})
assert ok("talk.session.submitToolResult", {"sessionId": sid, "callId": "c1", "result": {"x": 1}, "suppressResponse": True})["ok"]
c = ok("talk.session.close", {"sessionId": sid}); assert len(c["transcript"]) >= 4, c
assert not rpc("talk.session.steer", {"sessionId": sid, "text": "x"}).get("ok")
ok("talk.client.create", {"sessionKey": "agent:main:tc"})
ok("talk.client.transcript", {"sessionKey": "agent:main:tc", "text": "said", "role": "user"})
assert ok("talk.client.steer", {"sessionKey": "agent:main:tc", "text": "hi"})["reply"]
ok("talk.client.close", {"sessionKey": "agent:main:tc"})
assert not rpc("talk.client.steer", {"sessionKey": "agent:main:tc", "text": "hi"}).get("ok")
ok("mcp.app.discover", {})
v = ok("mcp.app.launch", {"uri": "ui://x", "sessionKey": "agent:main:app"}); vid = v["viewId"]; assert v["resource"]["text"].strip() == "mock-resource"
assert ok("mcp.app.view", {"viewId": vid})["uri"] == "ui://x"
u = ok("mcp.app.updateModelContext", {"viewId": vid, "content": [{"type": "text", "text": "ctx"}]})
assert ok("mcp.app.modelContext", {"viewId": vid})["state"]["items"]
assert not rpc("mcp.app.removeModelContext", {"viewId": vid, "updateId": "stale"}).get("ok")
ok("mcp.app.removeModelContext", {"viewId": vid, "updateId": u["updateId"]})
ok("mcp.app.writeResource", {"viewId": vid, "path": "a/b.txt", "content": "hi"})
assert ok("mcp.app.openFile", {"viewId": vid, "path": "a/b.txt"})["content"] == "hi"
assert not rpc("mcp.app.openFile", {"viewId": vid, "path": "../x"}).get("ok")
assert ok("mcp.app.subscribeResource", {"viewId": vid, "uri": "ui://y"})["subscriptions"] == ["ui://y"]
import time; time.sleep(0.4); open(os.environ["T"] + "/res.txt", "w").write("changed")
seen = False
for _ in range(40):
    if any("mcp.app.resourceUpdated" in json.dumps(l) for l in ok("logs.tail", {"n": 400})["lines"]): seen = True; break
    time.sleep(0.1)
assert seen, "resourceUpdated event not emitted"
assert ok("mcp.app.unsubscribeResource", {"viewId": vid, "uri": "ui://y"})["subscriptions"] == []
ok("mcp.app.mention", {"viewId": vid})
assert not rpc("mcp.app.view", {"viewId": "nope"}).get("ok")
assert ok("mcp.app.settings", {"appId": "a", "settings": {"z": 1}})["settings"]["z"] == 1
print("[e2e-talk-mcpapp] ok")
P
