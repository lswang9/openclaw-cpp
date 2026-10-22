#!/usr/bin/env bash
# 1.104: claws add (install records) / status (drift) / remove (consumes the gateway deletion journal).
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
trap _reap EXIT
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-claws] FAIL: $*"; tail -5 "$T/gw.log" 2>/dev/null; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home"
echo "{\"gateway\": {\"port\": $PORT}}" >"$T/cfg.json"
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/cfg.json" HOME="$T/home" \
  OPENCLAW_STATE_DIR="$T/state" OPENCLAW_GATEWAY_URL="http://127.0.0.1:$PORT"
FX="$ROOT/tests/fixtures/claws"
start() { OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!; for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done; }
start
# preview: nothing mutated, integrity printed
"$BIN" claws add "$FX/workspace-agent.claw.json" --dry-run --json >"$T/plan.json" || fail "add dry-run"
python3 - "$T" <<'P' || fail "add plan"
import json,sys,os; T=sys.argv[1]; p=json.load(open(T+"/plan.json"))
assert p["agentId"]=="workspace-agent" and p["applied"] is False and p["planIntegrity"].startswith("sha256:"), p
assert set(p["files"])=={"SOUL.md","HEARTBEAT.md","reference/policy.md"}, p["files"]
assert not os.path.exists(T+"/state/workspace-workspace-agent") and "workspace-agent" not in open(T+"/cfg.json").read()
P
PI=$(python3 -c "import json;print(json.load(open('$T/plan.json'))['planIntegrity'])")
"$BIN" claws add "$FX/workspace-agent.claw.json" --yes --plan-integrity sha256:0000000000000000000000000000000000000000000000000000000000000000 2>/dev/null && fail "stale integrity accepted"
"$BIN" claws add "$FX/workspace-agent.claw.json" --yes --plan-integrity "$PI" | grep -q "added Claw agent 'workspace-agent'" || fail "add apply"
"$BIN" claws add "$FX/workspace-agent.claw.json" --yes 2>&1 | grep -q "already exists" || fail "duplicate add refused"
"$BIN" claws add "$FX/delegating-agent" --yes --agent-id helper >/dev/null || fail "add CLAW.md package"
WS="$T/state/workspace-workspace-agent"
[[ -f "$WS/SOUL.md" && -f "$WS/reference/policy.md" ]] || fail "workspace files written"
grep -q "Coordinate research" "$T/state/workspace-helper/AGENTS.md" || fail "CLAW.md body → AGENTS.md"
"$BIN" claws status --json >"$T/st.json"
python3 - "$T" <<'P' || fail "status"
import json,sys; T=sys.argv[1]; s=json.load(open(T+"/st.json"))["installs"]
ids={i["agentId"]:i for i in s}; assert set(ids)=={"workspace-agent","helper"} and all(not i["drift"] for i in s), s
assert ids["helper"]["package"]["name"]=="@acme/delegating-agent" and ids["helper"]["package"]["version"]=="1.0.0"
st=json.load(open(T+"/data/claws/state.json")); assert st["installs"]["workspace-agent"]["files"]["SOUL.md"]["digest"].startswith("sha256:")
cfg=json.load(open(T+"/cfg.json")); assert [a["id"] for a in cfg["agents"]["list"]]==["workspace-agent","helper"], cfg
P
# drift + agent state, then remove: modified file kept, owned files and agent dir deleted, journal closed
echo "my edits" >>"$WS/SOUL.md"
curl -sf "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"chat.send","params":{"agentId":"workspace-agent","sessionKey":"agent:workspace-agent:x","message":"hi"}}' >/dev/null
"$BIN" claws status workspace-agent | grep -q "file_modified SOUL.md" || fail "drift reported"
"$BIN" claws remove workspace-agent --dry-run --json >"$T/rp.json" || fail "remove dry-run"
python3 -c "import json;p=json.load(open('$T/rp.json'));assert p['retainedModifiedFiles']==['SOUL.md'] and sorted(p['removeFiles'])==['HEARTBEAT.md','reference/policy.md'] and p['removeConfigEntry'],p" || fail "remove plan"
RPI=$(python3 -c "import json;print(json.load(open('$T/rp.json'))['planIntegrity'])")
kill $GW; wait $GW 2>/dev/null || true; GW=""
"$BIN" claws remove workspace-agent --yes --plan-integrity "$RPI" 2>&1 | grep -q "Gateway must accept the deletion journal" || fail "remove without gateway must refuse"
grep -q '"workspace-agent"' "$T/data/claws/state.json" || fail "install kept when journal not begun"
start
"$BIN" claws remove workspace-agent --yes --plan-integrity "$RPI" --json >"$T/rr.json" || fail "remove apply"
python3 - "$T" <<'P' || fail "remove effects"
import json,sys,os; T=sys.argv[1]; r=json.load(open(T+"/rr.json")); WS=T+"/state/workspace-workspace-agent"
assert r["applied"] and os.path.exists(WS+"/SOUL.md") and not os.path.exists(WS+"/HEARTBEAT.md") and not os.path.exists(WS+"/reference"), r
assert any(p.endswith("/workspace-agent") for p in r["deletedPaths"]), r
for p in r["deletedPaths"]: assert not os.path.exists(p)
st=json.load(open(T+"/data/claws/state.json"))
assert "workspace-agent" not in st["installs"] and st["journals"]["workspace-agent"]["cleanupCompleted"] is True and "workspace-agent" not in st["leases"], st
assert st["journals"]["workspace-agent"]["operationId"]==r["operationId"]
cfg=json.load(open(T+"/cfg.json")); assert [a["id"] for a in cfg["agents"]["list"]]==["helper"], cfg
P
"$BIN" claws remove workspace-agent --yes 2>&1 | grep -q "not a Claw-created agent" || fail "second remove refused"
"$BIN" claws remove @acme/delegating-agent --yes >/dev/null || fail "remove by package name"
[[ ! -d "$T/state/workspace-helper" ]] || fail "unmodified workspace fully removed"
echo "[e2e-claws] claws add (plan integrity, install records) / status (drift) / remove (gateway journal, owned cleanup, config): PASS"
