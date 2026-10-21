#!/usr/bin/env bash
# 1.107: re-diff closures (agent, sessions.describe, projects.*, node.event/pair.remove) and the honest
# not-implemented contract; probe-unimplemented --check keeps the generated list current.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; ND=""; trap '[[ -n "$ND" ]] && kill $ND 2>/dev/null; [[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-rpc-real] FAIL: $*"; tail -5 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home" "$T/repo" "$T/ws"
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" HOME="$T/home" \
  OPENCLAW_WORKSPACE="$T/ws" GIT_CONFIG_GLOBAL=/dev/null
( cd "$T/repo" && git init -q -b main && git -c user.name=t -c user.email=t@t commit -q --allow-empty -m init && git remote add origin https://github.com/acme/repo.git )
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
HOME="$T/home" "$BIN" node run --url "http://127.0.0.1:$PORT" --node-id n1 >"$T/node.log" 2>&1 & ND=$!
for i in $(seq 1 50); do grep -q "registered as n1" "$T/node.log" && break; sleep 0.1; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T PORT
python3 - <<'P' || fail "rpc closures"
import json, os, urllib.request, socket, base64, struct, time
URL, T = os.environ["URL"], os.environ["T"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"jsonrpc":"2.0","id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=30).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    j = rpc(m, p); assert j.get("ok") is True, (m, j); return j["payload"]
def err(m, p, needle):
    j = rpc(m, p); assert j.get("ok") is False and needle in json.dumps(j), (m, needle, j); return j
a = ok("agent", {"message": "hello agent", "agentId": "main"})
assert a["status"] == "ok" and a["sessionKey"] == "agent:main:main" and a["text"] and a["result"]["payloads"][0]["text"] == a["text"], a
d = ok("sessions.describe", {"key": "agent:main:main", "includeLastMessage": True})
assert d["messageCount"] >= 2 and d["lastMessage"]["role"] == "assistant", d
err("sessions.describe", {"key": "nope"}, "session not found")
# projects: workspace + register (idempotent) + add (clone) + remove(deleteCheckout)
pl = ok("projects.list", {}); assert pl["projects"][0]["source"] == "workspace", pl
r = ok("projects.register", {"path": T + "/repo", "name": "My Repo"})
assert r["id"] == "my-repo" and r["source"] == "registered" and r["originUrl"] == "https://github.com/acme/repo.git", r
assert ok("projects.register", {"path": T + "/repo"}) == r, "idempotent register"
err("projects.register", {"path": T + "/home"}, "not a git repository")
c = ok("projects.add", {"gitUrl": "file://" + T + "/repo"}); assert c["source"] == "cloned" and os.path.isdir(c["repoRoot"] + "/.git"), c
err("projects.add", {"gitUrl": "--upload-pack=x"}, "gitUrl must be")
assert {p["id"] for p in ok("projects.list", {})["projects"]} == {"workspace", "my-repo", c["id"]}
err("projects.remove", {"id": "my-repo", "deleteCheckout": True}, "only to projects cloned")
assert ok("projects.remove", {"id": c["id"], "deleteCheckout": True}) == {"removed": True} and not os.path.exists(c["repoRoot"])
assert ok("projects.remove", {"id": "ghost"}) == {"removed": False}
# node.event only from node connections; node.pair.remove refuses a connected node
err("node.event", {"event": "x"}, "requires a connected node")
err("node.pair.remove", {"nodeId": "n1"}, "is connected")
assert ok("node.pair.remove", {"nodeId": "node-stub"})["removed"] is True
err("node.pair.approve", {"requestId": "nope"}, "unknown pairing request")
# honest contract: an unimplemented upstream method is an error, never a fake success
u = err("portal.open", {}, "not implemented in the C++ runtime")
st = ok("methods.list", {})
assert "portal.open" not in st["implemented"] and "agent" in st["implemented"], "implemented list excludes unimplemented"
print("ok")
P
for t in sessions_list sessions_history session_status agents_list nodes web_fetch; do
  curl -s "$URL" -H 'Content-Type: application/json' -d '{"id":1,"method":"tools.list","params":{}}' | grep -q "\"$t\"" || fail "tool $t missing"
done
curl -s "$URL" -H 'Content-Type: application/json' -d '{"id":1,"method":"tools.invoke","params":{"name":"agents_list","args":{}}}' | grep -q '"agents"' || fail "agents_list tool"
curl -s "$URL" -H 'Content-Type: application/json' -d '{"id":1,"method":"tools.invoke","params":{"name":"nodes","args":{"action":"list"}}}' | grep -q '"nodes"' || fail "nodes tool"
python3 "$ROOT/scripts/probe-unimplemented.py" "$URL" --check || fail "unimplemented list stale (run scripts/probe-unimplemented.py URL --write)"
echo "[e2e-rpc-real] agent, sessions.describe, projects.*, node.event/pair.*, honest not-implemented + probe check: PASS"
