#!/usr/bin/env bash
# 1.108: upstream agent tools backed by gateway state, with chained turns (a turn's tool reads another session;
# sessions_spawn runs a child turn) and short-key normalization (k1 → agent:main:k1).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-agent-tools] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home" "$T/ws" "$T/docs"
echo "# Gateway pairing\nUse openclaw devices approve to pair a node." > "$T/docs/pairing.md"
cat > "$T/script.json" <<'J'
[
  {"content": "alpha-reply"},
  {"tool_calls": [{"name": "sessions_history", "arguments": {"sessionKey": "k1"}}]},
  {"content": "chained-done"},
  {"tool_calls": [{"name": "sessions_spawn", "arguments": {"task": "child task text", "label": "sub1"}}]},
  {"content": "child-result"},
  {"content": "spawn-done"}
]
J
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" \
  OPENCLAW_CONFIG_PATH="$T/none.json" HOME="$T/home" OPENCLAW_WORKSPACE="$T/ws" OPENCLAW_DOCS_DIR="$T/docs"
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T
python3 - <<'P' || fail "agent tools"
import json, os, urllib.request
URL, T = os.environ["URL"], os.environ["T"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def tool(n, a, sk=None):
    p = {"name": n, "args": a}
    if sk: p["sessionKey"] = sk
    r = rpc("tools.invoke", p); assert r.get("ok"), (n, r); return r["payload"]["result"]
assert rpc("chat.send", {"sessionKey":"k1","message":"hello alpha"})["payload"]["text"] == "alpha-reply"
h = tool("sessions_history", {"sessionKey":"k1"})
assert "error" not in h and any("hello alpha" in m["content"] for m in h["messages"]), h
r = rpc("chat.send", {"sessionKey":"k2","message":"read k1"}); assert r["payload"]["text"] == "chained-done", r
h2 = tool("sessions_history", {"sessionKey":"k2","includeTools":True})
assert any(m["role"] == "tool" and "hello alpha" in m["content"] for m in h2["messages"]), h2
st = tool("session_status", {"sessionKey":"k1"}); assert st["messageCount"] >= 2, st
r = rpc("chat.send", {"sessionKey":"k3","message":"spawn one"}); assert r["payload"]["text"] == "spawn-done", r
subs = tool("subagents", {"action":"list","all":True})["runs"]
assert subs and subs[0]["status"] == "done" and subs[0]["result"] == "child-result", subs
assert tool("sessions_search", {"query":"HELLO alpha"})["results"], "search"
g = tool("create_goal", {"objective":"ship 1.108"}, "k1"); assert g["status"] == "active"
assert tool("update_goal", {"status":"done"}, "k1")["status"] == "done"
assert tool("get_goal", {}, "k1")["objective"] == "ship 1.108"
assert tool("docs_search", {"query":"devices approve"})["results"][0]["path"] == "pairing.md"
im = tool("image_generate", {"prompt":"a cat","count":2}); assert im["ok"] and len(im["files"]) == 2
meta = tool("image", {"path": im["files"][0]}); assert meta["items"][0]["format"] == "png" and meta["items"][0]["width"] == 1
mu = tool("music_generate", {"prompt":"tune"}); assert open(mu["files"][0],"rb").read(4) == b"RIFF"
rm = tool("reminder", {"text":"stand up","inSeconds":3600}); assert rm["id"].startswith("reminder-")
assert any(j.get("id") == rm["id"] for j in json.dumps(tool("automations", {"action":"list"})) and tool("automations",{"action":"list"}).get("jobs", [])) or rm["id"] in json.dumps(tool("automations",{"action":"list"}))
tx = tool("transcripts", {"action":"save","title":"standup","transcript":"A: hi"}); assert tool("transcripts", {"action":"read","id":tx["id"]})["transcript"] == "A: hi"
s = tool("secrets", {"name":"NOPE"}); assert s["available"] is False and "value" not in s
assert "error" in tool("message", {"action":"send","channel":"","to":"","message":""})
names = rpc("tools.list", {})["payload"]["tools"]
for n in ["sessions_send","sessions_spawn","sessions_yield","sessions_search","subagents","message","automations","reminder","image","pdf","tts",
          "image_generate","video_generate","music_generate","create_goal","get_goal","update_goal","docs_search","skills_search","skills_read","transcripts","secrets","presence"]:
    assert n in names, n
print("ok")
P
echo "[e2e-agent-tools] short-key history, chained cross-session tool turn, spawn+subagents, search, goals, docs, media mock, reminder/automations, transcripts, secrets: PASS"
