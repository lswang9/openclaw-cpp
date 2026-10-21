#!/usr/bin/env bash
# Offline e2e: real exec tool under tools.exec.mode=ask; the run waits for an approval that arrives as a
# chat `/approve <id>` on the same channel while the first request is still in flight.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_PORT2:-18798}"
T="${TMPDIR:-/tmp}/oc-e2e-exec-$$"
mkdir -p "$T/sessions" "$T/data"
cat > "$T/script.json" <<'J'
[
  {"tool_calls": [{"name": "exec", "arguments": {"command": "echo approved-via-chat && echo $((6*7))"}}]},
  {"content": "done-after-exec"},
  {"tool_calls": [{"name": "exec", "arguments": {"command": "rm -rf ./nothing-here"}}]},
  {"content": "done-after-deny"}
]
J
cat > "$T/data/openclaw.json" <<'J'
{ tools: { exec: { mode: "ask", approvalTimeoutMs: 20000 } } }
J
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_GATEWAY_PORT="$PORT"
export OPENCLAW_GATEWAY_HOST=127.0.0.1 OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data"
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
export OPENCLAW_EXEC_APPROVALS_PATH="$T/data/exec-approvals.json"
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$T"' EXIT
for _ in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
post() { curl -sf -m 30 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d "{\"type\":\"event_callback\",\"event\":{\"type\":\"message\",\"text\":\"$1\",\"user\":\"U1\",\"channel\":\"C4\",\"channel_type\":\"channel\"}}"; }
rpc() { curl -sf -X POST "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
wait_pending() {
  for _ in $(seq 1 100); do
    ID=$(rpc '{"jsonrpc":"2.0","id":1,"method":"exec.approvals.list","params":{}}' | python3 -c 'import json,sys;p=json.load(sys.stdin)["payload"]["pending"];print(p[0]["id"] if p else "")')
    [[ -n "$ID" ]] && return 0; sleep 0.1
  done
  echo "[e2e-exec] no pending approval" >&2; cat "$T/gw.log" >&2; exit 1
}
post "run the thing" > "$T/r1" &
P1=$!
wait_pending
curl -sf -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d "{\"type\":\"event_callback\",\"event\":{\"type\":\"message\",\"text\":\"/approve $ID allow-once\",\"user\":\"U2\",\"channel\":\"C4\",\"channel_type\":\"channel\"}}" \
  | grep -q 'Not authorized' || { echo "[e2e-exec] other user could approve" >&2; exit 1; }
post "/approve $ID allow-once" | grep -q "resolved: allow-once" || { echo "[e2e-exec] /approve failed" >&2; exit 1; }
wait $P1
grep -q 'done-after-exec' "$T/r1" || { echo "[e2e-exec] run did not finish: $(cat "$T/r1")" >&2; exit 1; }
TR=$(ls "$T/sessions"/agent~3amain~3aslack*.jsonl)
grep -q 'approved-via-chat' "$TR" && grep -q '42' "$TR" || { echo "[e2e-exec] exec output missing from transcript" >&2; exit 1; }
rpc '{"jsonrpc":"2.0","id":2,"method":"slack.status","params":{}}' | grep -q 'Exec approval required' || true
post "second" > "$T/r2" &
P2=$!
wait_pending
rpc "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"exec.approval.resolve\",\"params\":{\"id\":\"$ID\",\"decision\":\"deny\"}}" | grep -q '"resolved": *true'
wait $P2
grep -q 'done-after-deny' "$T/r2" || { echo "[e2e-exec] deny run did not finish" >&2; exit 1; }
grep -q 'exec denied by operator' "$TR" || { echo "[e2e-exec] denial not returned to the agent" >&2; exit 1; }
echo "[e2e-exec] exec approvals: chat /approve (requester only) allow-once ran real command; RPC deny returned denial to agent: PASS"
