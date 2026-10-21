#!/usr/bin/env bash
# 1.105: node host protocol + worker execution. A local `openclaw-cpp node run` process connects as role "node";
# node.invoke round-trips; a session dispatched to that device runs its turns on the node (worker.launch.v1 →
# worker.status.v1 receipts) while the transcript stays on the gateway; node exit fails the placement.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; ND=""; trap '[[ -n "$ND" ]] && kill $ND 2>/dev/null; [[ -n "$GW" ]] && kill $GW 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-node-worker] FAIL: $*"; tail -5 "$T/gw.log"; tail -5 "$T/node.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home" "$T/nodehome"
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" HOME="$T/home"
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
HOME="$T/nodehome" "$BIN" node run --url "http://127.0.0.1:$PORT" --node-id laptop-1 --display-name "Test laptop" >"$T/node.log" 2>&1 & ND=$!
for i in $(seq 1 50); do grep -q "registered as laptop-1" "$T/node.log" && break; sleep 0.1; done
grep -q "registered as laptop-1" "$T/node.log" || fail "node registration"
export URL="http://127.0.0.1:$PORT/v1/rpc" T
cat >"$T/t.py" <<'P'
import json, os, urllib.request, subprocess, time
URL, T = os.environ["URL"], os.environ["T"]
def rpc(m, p):
    return json.loads(urllib.request.urlopen(urllib.request.Request(URL, json.dumps({"jsonrpc":"2.0","id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"}), timeout=120).read())
def ok(m, p):
    j = rpc(m, p); assert j.get("ok") is True, (m, j); return j["payload"]
def err(m, p, needle):
    j = rpc(m, p); assert j.get("ok") is False, (m, j); assert needle in json.dumps(j), (m, needle, j); return j
nodes = {n["id"]: n for n in ok("node.list", {})["nodes"]}
assert nodes["laptop-1"]["online"] and nodes["laptop-1"]["kind"] == "node" and "worker.launch.v1" in nodes["laptop-1"]["commands"], nodes
d = ok("node.describe", {"nodeId": "laptop-1"}); assert d["displayName"] == "Test laptop" and d["connected"], d
pong = ok("node.invoke", {"nodeId": "laptop-1", "command": "ping", "params": {}})
assert pong["payload"] == {"pong": True, "nodeId": "laptop-1"} and json.loads(pong["payloadJSON"]) == pong["payload"], pong
err("node.invoke", {"nodeId": "laptop-1", "command": "system.run", "params": {}}, "does not declare")
err("node.invoke", {"nodeId": "ghost", "command": "ping"}, "not connected")
bad = err("node.invoke", {"nodeId": "laptop-1", "command": "worker.launch.v1", "params": {"environmentSession": 2}}, "environment lifetime")
assert bad.get("code") == "INVALID_REQUEST", bad
err("node.invoke.result", {"id": "nope", "nodeId": "laptop-1", "ok": True}, "unknown or expired")
# local turn first, then place the session on the node: following turns execute there
S = "agent:main:w1"
local = ok("chat.send", {"sessionKey": S, "message": "first local turn"}); assert "worker" not in local
p = ok("sessions.dispatch", {"key": S, "deviceId": "laptop-1"})["placement"]; assert p["state"] == "active", p
r = ok("chat.send", {"sessionKey": S, "message": "run me on the worker"})
assert r["worker"]["deviceId"] == "laptop-1" and r["worker"]["placementGeneration"] == p["generation"] and r["text"], r
hist = ok("chat.history", {"sessionKey": S})
msgs = hist.get("messages", hist.get("history", []))
texts = [m.get("content") for m in msgs]
assert "run me on the worker" in texts and r["text"] in texts, texts
log = open(T + "/node.log").read()
assert "session " + S + " history=2 ok" in log, log   # the worker saw the gateway's transcript
assert os.path.isdir(T + "/nodehome/.openclaw/worker/sessions"), "remote workspace dir created on the node host"
# reclaim → local again
ok("sessions.reclaim", {"key": S})
r2 = ok("chat.send", {"sessionKey": S, "message": "back home"}); assert "worker" not in r2, r2
# placed again, node exits → turn fails and placement becomes failed (recoverable)
ok("sessions.move", {"key": S, "target": {"kind": "device", "deviceId": "laptop-1"}})
open(T + "/kill", "w").write("1")
P
OPENCLAW_GATEWAY_URL="http://127.0.0.1:$PORT" python3 "$T/t.py" || fail "node worker flow"
kill $ND; wait $ND 2>/dev/null || true; ND=""
for i in $(seq 1 30); do curl -s "$URL" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"node.list","params":{}}' | grep -q '"online":false' && break; sleep 0.1; done
R=$(curl -s "$URL" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"chat.send","params":{"sessionKey":"agent:main:w1","message":"node gone"}}')
grep -q "not connected" <<<"$R" || fail "turn on a gone node must fail: $R"
python3 -c "import json;r=json.load(open('$T/data/worker-placements.json'))['main:agent:main:w1'];assert r['state']=='failed' and 'not connected' in r['recoveryError'],r" || fail "placement failed after node loss"
GEN=$(python3 -c "import json;print(json.load(open('$T/data/worker-placements.json'))['main:agent:main:w1']['generation'])")
R=$(curl -s "$URL" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sessions.reclaim\",\"params\":{\"key\":\"agent:main:w1\",\"recoverToGateway\":{\"expectedGeneration\":$GEN}}}")
python3 -c "import json,sys;j=json.loads(sys.argv[1]);assert j['ok'] and j['payload']['placement']['state']=='local',j" "$R" || fail "recover failed placement: $R"
echo "[e2e-node-worker] node host connect/invoke/result + worker.launch/status turns on a placed session + offline failure/recovery: PASS"
