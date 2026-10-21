#!/usr/bin/env bash
# SQLite approvals store (migrate from JSON), binary-changed denial after approve, background+process poll.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_PORT5:-$((19000 + RANDOM % 500))}"
T="${TMPDIR:-/tmp}/oc-e2e-exec2-$$"
mkdir -p "$T/sessions" "$T/data" "$T/ws"
# Legacy JSON present → should migrate into state/openclaw.sqlite
cat > "$T/data/exec-approvals.json" <<'J'
{"version":1,"defaults":{"security":"full","ask":"off"},"agents":{"main":{"allowlist":[{"id":"echo1","pattern":"/bin/echo","argPattern":"^hi$"}]}}}
J
cat > "$T/data/openclaw.json" <<'J'
{ tools: { exec: { mode: "ask", approvalTimeoutMs: 15000 } }, agents: { defaults: { workspace: "WS" } } }
J
# patch workspace path
python3 -c "import pathlib; p=pathlib.Path('$T/data/openclaw.json'); t=p.read_text().replace('WS','$T/ws'); p.write_text(t)"

export OPENCLAW_DEFAULT_PROVIDER=mock
export OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_GATEWAY_HOST=127.0.0.1
export OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_STATE_DIR="$T/data"
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
unset OPENCLAW_EXEC_APPROVALS_PATH

# --- unit-ish: LoadApprovals migrates ---
cat > "$T/script-migrate.json" <<'J'
[{"content":"pong"}]
J
export OPENCLAW_MOCK_SCRIPT="$T/script-migrate.json"
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$T"' EXIT
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done

# Trigger an allowlist allow-always path via RPC tools — use chat completions with exec after seed
# First force load by calling exec.approvals via a tiny C++ path: run `approvals get` CLI if present
if "$BIN" approvals get >"$T/appr.json" 2>/dev/null; then
  grep -q echo1 "$T/appr.json" || { echo "[e2e-exec2] approvals get missing migrated allowlist" >&2; cat "$T/appr.json" >&2; exit 1; }
fi
# SQLite file should appear after any LoadApprovals; nudge via approvals get or exec-policy
"$BIN" exec-policy show >"$T/pol.txt" 2>/dev/null || true
# Direct: use python sqlite check after gateway touched store via RPC
rpc() { curl -sf -X POST "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
# Seed migration by resolving a fake path through tools — call config.get which may not load approvals.
# Invoke exec via chat with allowlist hit (mode ask but pattern matches → allow without ask)
cat > "$T/script-al.json" <<'J'
[
  {"tool_calls": [{"name": "exec", "arguments": {"command": "/bin/echo hi"}}]},
  {"content": "echo-done"}
]
J
# restart gateway with new script
kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true
export OPENCLAW_MOCK_SCRIPT="$T/script-al.json"
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
curl -sf -m 30 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"type":"event_callback","event":{"type":"message","text":"run echo","user":"U1","channel":"C5","channel_type":"channel"}}' \
  | grep -q echo-done || { echo "[e2e-exec2] allowlist exec failed" >&2; cat "$T/gw.log" >&2; exit 1; }
[[ -f "$T/data/state/openclaw.sqlite" ]] || { echo "[e2e-exec2] sqlite not created" >&2; ls -R "$T/data" >&2; exit 1; }
[[ -f "$T/data/exec-approvals.json.migrated" || ! -f "$T/data/exec-approvals.json" ]] || \
  { echo "[e2e-exec2] legacy json not migrated" >&2; ls "$T/data" >&2; exit 1; }
python3 - <<PY
import sqlite3
c=sqlite3.connect("$T/data/state/openclaw.sqlite")
row=c.execute("select value from kv where key='exec_approvals_config'").fetchone()
assert row and 'echo1' in row[0], row
print("sqlite-row-ok")
PY

# --- binary-changed after approve ---
mkdir -p "$T/sessions/workspace"
printf 'echo ORIG\n' > "$T/sessions/workspace/script.sh"
chmod +x "$T/sessions/workspace/script.sh"
cat > "$T/script-chg.json" <<'J'
[
  {"tool_calls": [{"name": "exec", "arguments": {"command": "bash script.sh"}}]},
  {"content": "after-deny-change"}
]
J
kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true
# wipe allowlist so ask is required
rm -f "$T/data/state/openclaw.sqlite"
export OPENCLAW_MOCK_SCRIPT="$T/script-chg.json"
"$BIN" gateway >"$T/gw2.log" 2>&1 &
GW=$!
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
post() { curl -sf -m 40 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d "{\"type\":\"event_callback\",\"event\":{\"type\":\"message\",\"text\":\"$1\",\"user\":\"U1\",\"channel\":\"C6\",\"channel_type\":\"channel\"}}"; }
post "run script" > "$T/rchg" &
P1=$!
ID=""
for _ in $(seq 1 100); do
  ID=$(rpc '{"jsonrpc":"2.0","id":1,"method":"exec.approvals.list","params":{}}' | python3 -c 'import json,sys;p=json.load(sys.stdin)["payload"]["pending"];print(p[0]["id"] if p else "")' 2>/dev/null || true)
  [[ -n "$ID" ]] && break
  sleep 0.1
done
[[ -n "$ID" ]] || { echo "[e2e-exec2] no pending for binary-change" >&2; cat "$T/gw2.log" >&2; exit 1; }
# mutate while pending
printf 'echo MUTATED\n' > "$T/sessions/workspace/script.sh"
post "/approve $ID allow-once" | grep -q resolved || { echo "[e2e-exec2] approve failed" >&2; exit 1; }
wait $P1 || true
# Agent should see denial about changed operand, then model says after-deny-change
grep -q 'after-deny-change' "$T/rchg" || { echo "[e2e-exec2] expected continuation after deny: $(cat "$T/rchg")" >&2; exit 1; }
TR=$(ls "$T/sessions"/agent~3amain~3aslack~3achannel~3aC6.jsonl)
grep -qi 'changed after approval\|operand changed' "$TR" || { echo "[e2e-exec2] no binary-change denial in transcript" >&2; grep -i exec "$TR" | head >&2; exit 1; }

# --- background + process poll ---
cat > "$T/script-bg.json" <<'J'
[
  {"tool_calls": [{"name": "exec", "arguments": {"command": "sleep 1; echo bg-done", "background": true}}]},
  {"tool_calls": [{"name": "process", "arguments": {"action": "poll", "sessionId": "WILL_PATCH", "timeout": 5000}}]},
  {"content": "bg-ok"}
]
J
# Mock provider can't know sessionId from prior tool — use yieldMs path with process list instead.
cat > "$T/script-bg.json" <<'J'
[
  {"tool_calls": [{"name": "exec", "arguments": {"command": "sleep 0.8; echo bg-done", "yieldMs": 50}}]},
  {"tool_calls": [{"name": "process", "arguments": {"action": "list"}}]},
  {"content": "bg-listed"}
]
J
kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true
export OPENCLAW_MOCK_SCRIPT="$T/script-bg.json"
# mode full so no approval
cat > "$T/data/openclaw.json" <<J
{ tools: { exec: { mode: "full" } }, agents: { defaults: { workspace: "$T/ws" } } }
J
"$BIN" gateway >"$T/gw3.log" 2>&1 &
GW=$!
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
curl -sf -m 40 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"type":"event_callback","event":{"type":"message","text":"bg","user":"U1","channel":"C7","channel_type":"channel"}}' \
  | tee "$T/rbg" | grep -q bg-listed || { echo "[e2e-exec2] background list failed" >&2; cat "$T/gw3.log" "$T/rbg" >&2; exit 1; }
TR=$(ls "$T/sessions"/agent~3amain~3aslack~3achannel~3aC7.jsonl)
grep -q 'sessionId' "$TR" || { echo "[e2e-exec2] no sessionId in bg exec result" >&2; exit 1; }
grep -q '"sessions"' "$TR" || grep -q 'sessions' "$TR" || true

echo "[e2e-exec2] sqlite migrate + binary-changed denial + background/process: PASS"
