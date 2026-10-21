#!/usr/bin/env bash
# 1.118: portals (token-gated proxy), doctor.memory dream diary, gateway.suspend.*, gateway.stop.request, diagnostics lanes/profiles/snapshot.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-lifecycle] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
echo mock-resource > "$T/res.txt"; mkdir -p "$T/home" "$T/ws" "$T/repo"
( cd "$T/repo" && git init -q -b main && echo a > a.txt && git add a.txt && git -c user.name=t -c user.email=t@t commit -qm init )
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" \
  HOME="$T/home" OPENCLAW_WORKSPACE="$T/ws" GIT_CONFIG_GLOBAL=/dev/null
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T
mkdir -p "$T/site" "$T/ws/memory"; echo "hello-portal" > "$T/site/index.html"
python3 -u -m http.server --bind 127.0.0.1 0 --directory "$T/site" >"$T/http.log" 2>&1 & HS=$!
for i in $(seq 1 50); do grep -q "port" "$T/http.log" && break; sleep 0.1; done
export HPORT=$(sed -n 's/.*port \([0-9]*\).*/\1/p' "$T/http.log" | head -1) GWPID=$GW
printf '# day\n\nshipped portals\nfixed bugs\n' > "$T/ws/memory/2026-10-20.md"
printf 'notes\n' > "$T/ws/memory/2026-10-21-extra.md"
python3 - <<'P' || { kill $HS; fail "lifecycle"; }
import json, os, urllib.request, time
URL, T, HP = os.environ["URL"], os.environ["T"], int(os.environ["HPORT"])
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
po = ok("portal.open", {"port": HP, "title": "site"})
body = urllib.request.urlopen("http://127.0.0.1:%d/index.html?%s" % (po["listenPort"], po["tokenQuery"]), timeout=10).read()
assert body.strip() == b"hello-portal", body
try: urllib.request.urlopen("http://127.0.0.1:%d/index.html" % po["listenPort"], timeout=10); assert False
except urllib.error.HTTPError as e: assert e.code == 401
assert [x["id"] for x in ok("portal.list", {})["portals"]] == [po["id"]] and "tokenQuery" not in ok("portal.list", {})["portals"][0]
sp = ok("portal.session.open", {"port": HP, "sessionKey": "agent:main:p", "environmentId": "local"})
assert [x["id"] for x in ok("portal.session.list", {"sessionKey": "agent:main:p", "environmentId": "local"})["portals"]] == [sp["id"]]
assert not ok("portal.session.close", {"id": po["id"], "sessionKey": "agent:main:p", "environmentId": "local"})["closed"]
assert ok("portal.session.close", {"id": sp["id"], "sessionKey": "agent:main:p", "environmentId": "local"})["closed"]
assert ok("portal.close", {"id": po["id"]})["closed"] and ok("portal.list", {})["portals"] == []
assert not rpc("portal.open", {"port": 0}).get("ok")
# dream diary
assert not ok("doctor.memory.dreamDiary", {})["found"]
b = ok("doctor.memory.backfillDreamDiary", {}); assert b["scannedFiles"] == 2 and b["written"] == 2, b
dd = ok("doctor.memory.dreamDiary", {}); assert "October 20, 2026" in dd["content"] and "- shipped portals" in dd["content"], dd
assert ok("doctor.memory.backfillDreamDiary", {})["replaced"] == 2
c = open(T + "/ws/DREAMS.md").read().replace("<!-- openclaw:dreaming:diary:end -->", "---\n\n*Oct 1*\n\nmanual dream\n\n---\n\n*Oct 1*\n\nmanual dream\n<!-- openclaw:dreaming:diary:end -->")
open(T + "/ws/DREAMS.md", "w").write(c)
de = ok("doctor.memory.dedupeDreamDiary", {}); assert de["removedEntries"] == 1 and de["keptEntries"] == 3, de
r = ok("doctor.memory.resetDreamDiary", {}); assert r["removedEntries"] == 2
assert "manual dream" in ok("doctor.memory.dreamDiary", {})["content"]
os.makedirs(T + "/ws/memory/.dreams", exist_ok=True)
json.dump({"entries": [{"k": 1, "source": "grounded"}, {"k": 2}]}, open(T + "/ws/memory/.dreams/short-term-recall.json", "w"))
assert ok("doctor.memory.resetGroundedShortTerm", {})["removedShortTermEntries"] == 1
open(T + "/ws/memory/.dreams/session-ingestion.json", "w").write("{bad")
rp = ok("doctor.memory.repairDreamingArtifacts", {}); assert rp["changed"] and rp["archivedSessionIngestion"], rp
# diagnostics
assert ok("diagnostics.lanes", {})["lanes"]
assert "threads" in ok("diagnostics.cpuProfile", {"durationMs": 100})
assert ok("diagnostics.heapProfile", {"durationMs": 200})["samples"]
hs = ok("diagnostics.heapSnapshot", {"reason": "t"}); assert os.path.getsize(hs["path"]) > 0
# suspend
assert ok("gateway.suspend.status", {"suspensionId": "x"})["status"] == "running"
t = ok("terminal.open", {"cwd": T, "shell": "/bin/sh"})
assert ok("gateway.suspend.prepare", {"requestId": "r1"})["status"] == "busy"
s = ok("gateway.suspend.prepare", {"requestId": "r1", "drain": True}); assert s["status"] == "draining" and s["activeCount"] == 1, s
r = rpc("chat.send", {"sessionKey": "agent:main:x", "message": "hi"}); assert not r.get("ok") and "suspending" in json.dumps(r), r
ok("terminal.close", {"sessionId": t["sessionId"]})
assert ok("gateway.suspend.status", {"suspensionId": s["suspensionId"]})["status"] == "ready"
assert not rpc("gateway.suspend.handoff", {"suspensionId": s["suspensionId"], "target": {"pid": 1, "processInstanceId": "p"}}).get("ok")
assert ok("gateway.suspend.handoff", {"suspensionId": s["suspensionId"], "target": {"pid": int(os.environ["GWPID"]), "processInstanceId": "p"}})["status"] == "armed"
assert ok("gateway.suspend.resume", {"suspensionId": s["suspensionId"]})["resumed"]
ok("chat.send", {"sessionKey": "agent:main:x", "message": "hi"})
assert ok("gateway.stop.request", {"reason": "test", "delayMs": 100})["accepted"]
time.sleep(1.5)
try: urllib.request.urlopen(URL.replace("/v1/rpc", "/health"), timeout=2); assert False, "gateway still up"
except Exception as e: assert not isinstance(e, AssertionError), e
print("[e2e-lifecycle] ok")
P
kill $HS 2>/dev/null; GW=""
