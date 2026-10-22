#!/usr/bin/env bash
# 1.100: companion side chat (ask/state/reset), worker placement ledger (dispatch/move/reclaim incl. failed→recover),
# claws.removalJournal (begin/rollback with binding, physical identity, config/install/journal digests, lease owner).
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
trap _reap EXIT
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "${KEEP:-}" ]] || rm -rf "$T"' EXIT
fail() { echo "[e2e-subsys] FAIL: $*"; tail -5 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" HOME="$T/home"
mkdir -p "$T/data" "$T/home"
echo '{"nodes":[{"id":"mac-1","name":"Mac","kind":"desktop","online":true},{"id":"old","name":"Old","kind":"desktop","online":false}]}' >"$T/data/nodes.json"
start() { OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!; for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done; }
start
export URL="http://127.0.0.1:$PORT/v1/rpc" OPENCLAW_GATEWAY_URL="http://127.0.0.1:$PORT"
cat >"$T/t.py" <<'P'
import json, os, sys, urllib.request, hashlib
URL = os.environ["URL"]
def rpc(m, p):
    r = urllib.request.urlopen(urllib.request.Request(URL, json.dumps({"jsonrpc":"2.0","id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"}))
    return json.loads(r.read())
def ok(m, p):
    j = rpc(m, p); assert j.get("ok") is True, (m, j); return j["payload"]
def err(m, p, needle):
    j = rpc(m, p); assert j.get("ok") is False, (m, j); s = json.dumps(j.get("error")); assert needle in s, (m, needle, s); return s
phase = sys.argv[1]
if phase == "a":
    ok("chat.send", {"sessionKey": "agent:main:s1", "message": "the build uses CMake"})
    # companion
    assert ok("sessions.companion.state", {"sessionKey": "agent:main:s1"})["exchanges"] == []
    a = ok("sessions.companion.ask", {"sessionKey": "agent:main:s1", "question": "what build system?"})
    assert a["answer"] and isinstance(a["ts"], int), a
    ok("sessions.companion.ask", {"sessionKey": "agent:main:s1", "question": "and again?", "selectionContext": "CMake"})
    st = ok("sessions.companion.state", {"sessionKey": "agent:main:s1"})["exchanges"]
    assert [e["question"] for e in st] == ["what build system?", "and again?"], st
    hist = ok("chat.history", {"sessionKey": "agent:main:s1"})
    assert not any("what build system" in json.dumps(m) for m in hist["messages"]), "companion must not write the session"
    assert ok("sessions.companion.reset", {"sessionKey": "agent:main:s1"}) == {"ok": True}
    assert ok("sessions.companion.state", {"sessionKey": "agent:main:s1"})["exchanges"] == []
    err("sessions.companion.ask", {"sessionKey": "agent:main:s1", "question": "x" * 401}, "400")
    err("sessions.companion.reset", {"sessionKey": "agent:main:s1", "bogus": 1}, "unexpected property")
    err("sessions.companion.state", {"sessionKey": "nope"}, "session not found")
    # placement
    err("sessions.reclaim", {"key": "agent:main:s1"}, "not placed on a worker")
    err("sessions.dispatch", {"key": "agent:main:s1", "deviceId": "old"}, "offline")
    err("sessions.dispatch", {"key": "agent:main:s1", "deviceId": "mac-1", "autoDevice": True}, "exactly one")
    err("sessions.dispatch", {"key": "agent:main:s1", "profileId": "cloud"}, "not configured")
    d = ok("sessions.dispatch", {"key": "agent:main:s1", "deviceId": "mac-1", "machineClass": "m1"})
    pl = d["placement"]
    assert d["ok"] and d["key"] == "agent:main:s1" and pl["state"] == "active" and pl["generation"] == 1 and pl["environmentId"] == "device:mac-1", d
    assert len(pl["workerBundleHash"]) == 64 and pl["runner"]["deviceId"] == "mac-1" and "deviceId" not in pl, pl
    err("sessions.dispatch", {"key": "agent:main:s1", "autoDevice": True}, "already placed")
    err("sessions.move", {"key": "agent:main:s1", "target": {"kind": "gateway"}, "expectedSource": {"generation": 9, "environmentId": "device:mac-1", "ownerEpoch": 1}}, "changed")
    r = ok("sessions.reclaim", {"key": "agent:main:s1"})["placement"]
    assert r["state"] == "reclaimed" and r["generation"] == 2 and r["environmentId"] == "device:mac-1", r
    m = ok("sessions.move", {"key": "agent:main:s1", "target": {"kind": "device", "deviceId": "mac-1"}})["placement"]
    assert m["state"] == "active" and m["generation"] == 3, m
elif phase == "b":
    # device went offline across restart → failed; recover with the exact generation
    err("sessions.reclaim", {"key": "agent:main:s1"}, "recoverToGateway")
    err("sessions.reclaim", {"key": "agent:main:s1", "recoverToGateway": {"expectedGeneration": 1}}, "generation changed")
    r = ok("sessions.reclaim", {"key": "agent:main:s1", "recoverToGateway": {"expectedGeneration": 3}})["placement"]
    assert r["state"] == "local" and r["generation"] == 4, r
    print("placement ok")
P
python3 "$T/t.py" a || fail "companion/placement"
kill $GW; wait $GW 2>/dev/null || true
echo '{"nodes":[{"id":"mac-1","name":"Mac","kind":"desktop","online":false}]}' >"$T/data/nodes.json"
start
python3 "$T/t.py" b >/dev/null || fail "placement failure recovery"
# removal journal via the local CLI transport (computes binding/identity/digests like upstream's clawRemovalJournalGateway)
J=$("$BIN" claws removal-journal begin --agent helper --operation op-1 --owner w1 --json) || fail "journal begin: $J"
python3 -c "import json,sys;j=json.loads(sys.argv[1]);assert j['ok'] and j['journal']['agentId']=='helper' and j['journal']['cleanupPaths'] and not j['journal']['cleanupCompleted'],j" "$J" || fail "journal shape"
"$BIN" claws removal-journal begin --agent helper --operation op-2 --owner other --json 2>&1 | grep -q "held by another owner" || fail "lease owner"
"$BIN" claws removal-journal rollback --agent helper --operation wrong --owner w1 --json 2>&1 | grep -q "no longer owns" || fail "rollback op mismatch"
R=$("$BIN" claws removal-journal rollback --agent helper --operation op-1 --owner w1 --json) || fail "rollback"
python3 -c "import json,sys;j=json.loads(sys.argv[1]);assert j=={'ok':True,'journal':None},j" "$R" || fail "rollback shape"
"$BIN" claws removal-journal begin --agent main --operation x --owner w1 --json 2>&1 | grep -q "differs from its deletion owner" || fail "reserved agent"
"$BIN" claws removal-journal begin --agent helper --operation op-3 --owner w1 --stale-journal --json 2>&1 | grep -q "changed before mutation" || fail "journal digest guard"
B=$(curl -s "$URL" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"claws.removalJournal","params":{"phase":"begin","agentId":"helper"}}')
grep -q "Invalid Claw removal journal parameters" <<<"$B" || fail "schema: $B"
echo "[e2e-subsys] companion side chat, placement ledger (dispatch/move/reclaim/recover), claws removal journal: PASS"
