#!/usr/bin/env bash
# 1.110: task suggestions, progress cards, cron scratch, node pending queue + capability updates, session metadata, chat.message.get, toolTitles, mentions, diff, transcripts.export, gateway identity, wake.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-rpc-local] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
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
subprocess.run(["git","init","-q",ws]); open(ws+"/f.txt","w").write("x"); subprocess.run(["git","-C",ws,"add","f.txt"])
subprocess.run(["git","-C",ws,"-c","user.name=t","-c","user.email=t@t","commit","-qm","i"]); open(ws+"/f.txt","w").write("y")
assert "+y" in ok("sessions.diff", {})["diff"]
ts = ok("taskSuggestions.create", {"title":"Fix","prompt":"do it","tldr":"t","cwd":ws,"sessionKey":"s1"})
assert ok("taskSuggestions.list", {"sessionKey":"s1"})["suggestions"][0]["id"] == ts["taskId"]
acc = ok("taskSuggestions.accept", {"taskId": ts["taskId"]}); assert acc["key"].endswith(ts["taskId"])
assert not rpc("taskSuggestions.accept", {"taskId": ts["taskId"]}).get("ok"), "accept is single-use"
t2 = ok("taskSuggestions.create", {"title":"B","prompt":"p","tldr":"t","cwd":ws,"sessionKey":"s1"}); ok("taskSuggestions.dismiss", {"taskId": t2["taskId"]})
assert ok("taskSuggestions.list", {})["suggestions"] == []
c = ok("progressCard.put", {"sessionKey":"s1","markdown":"## plan","plan":[{"step":"a","status":"pending"}]})["card"]; assert c["revision"] == 1
assert not rpc("progressCard.put", {"sessionKey":"s1","markdown":"x","expectedRevision":5}).get("ok")
assert ok("progressCard.get", {"sessionKey":"s1"})["card"]["steps"][0]["step"] == "a"
assert ok("cron.scratch.set", {"content":"notes","expectedRevision":0})["currentRevision"] == 1
assert ok("cron.scratch.get", {})["scratch"] == "notes"
assert not rpc("cron.scratch.set", {"content":"z","expectedRevision":0}).get("ok")
i1 = ok("node.pending.enqueue", {"nodeId":"n9","type":"sync","priority":1})["item"]
ok("node.pending.enqueue", {"nodeId":"n9","type":"urgent","priority":9})
items = ok("node.pending.pull", {"nodeId":"n9"})["items"]; assert items[0]["type"] == "urgent" and len(items) == 2
assert ok("node.pending.ack", {"nodeId":"n9","ids":[i1["id"]]})["acked"] == 1
assert len(ok("node.pending.drain", {"nodeId":"n9"})["items"]) == 1 and ok("node.pending.pull", {"nodeId":"n9"})["items"] == []
assert not rpc("node.skills.update", {"skills": []}).get("ok"), "http caller is not a node"
assert ok("sessions.groups.update", {"name":"g1","cwd":ws})["group"]["cwd"] == ws
assert ok("sessions.groups.rename", {"name":"g1","to":"g2"})["group"]["cwd"] == ws
assert ok("session.visibility.set", {"key":"s1","visibility":"team"})["visibility"] == "team"
assert not rpc("session.visibility.set", {"key":"s1","visibility":"world"}).get("ok")
assert ok("session.publicShare.set", {"key":"s1","enabled":True})["shareId"].startswith("share-")
assert ok("sessions.setInvolvement", {"key":"s1","hidden":True})["hidden"] is True
r = ok("chat.send", {"sessionKey":"m1","message":"hello there"})
assert any(ok("chat.message.get", {"sessionKey":"m1","messageId":str(i)})["message"].get("content","").endswith("hello there") for i in range(2)), rpc("chat.message.get", {"sessionKey":"m1","messageId":"0"})
assert "exec" in ok("chat.toolTitles", {})["titles"]
assert any(x["kind"] == "agent" for x in ok("mentions.list", {})["mentions"])
assert "hello there" in ok("transcripts.export", {"selector":{"sessionKey":"m1"}})["content"]
g = ok("gateway.identity.get", {}); assert g["gatewayId"] == ok("gateway.identity.get", {})["gatewayId"]
assert ok("wake", {"mode":"next-heartbeat","text":"ping"})["queued"]
assert ok("sessions.patchMany", {"targets":["m1"],"patch":{"label":"x"}})["results"][0]["key"] == "m1"
print("ok")
P
echo "[e2e-rpc-local] task suggestions, progress cards, cron scratch, node pending, session meta, chat reads, diff, export, identity, wake: PASS"

