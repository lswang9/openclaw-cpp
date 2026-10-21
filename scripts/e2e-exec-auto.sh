#!/usr/bin/env bash
# tools.exec.mode=auto: allowlist miss → provider reviewer (mock) decides allow / deny without a human.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_PORT8:-$((19800 + RANDOM % 150))}"
T="${TMPDIR:-/tmp}/oc-e2e-auto-$$"
mkdir -p "$T/sessions" "$T/data" "$T/ws"
echo "{ tools: { exec: { mode: \"auto\", approvalTimeoutMs: 2000 } }, agents: { defaults: { workspace: \"$T/ws\" } } }" > "$T/data/openclaw.json"
cat > "$T/script.json" <<'J'
[
  {"tool_calls": [{"name": "exec", "arguments": {"command": "echo auto-ran-ok"}}]},
  {"content": "{\"decision\":\"allow\",\"risk\":\"low\",\"reason\":\"harmless echo\"}"},
  {"content": "first-done"},
  {"tool_calls": [{"name": "exec", "arguments": {"command": "rm -rf ./important"}}]},
  {"content": "{\"decision\":\"deny\",\"risk\":\"high\",\"reason\":\"destructive\"}"},
  {"content": "second-done"}
]
J
mkdir -p "$T/ws/important"
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_GATEWAY_HOST=127.0.0.1
export OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_STATE_DIR="$T/data"
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
unset OPENCLAW_EXEC_APPROVALS_PATH OPENCLAW_EXEC_MODE
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$T"' EXIT
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
post() { curl -sf -m 30 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d "{\"type\":\"event_callback\",\"event\":{\"type\":\"message\",\"text\":\"$1\",\"user\":\"U1\",\"channel\":\"C9\",\"channel_type\":\"channel\"}}"; }
post "one" | grep -q first-done || { echo "[e2e-auto] first turn failed" >&2; cat "$T/gw.log" >&2; exit 1; }
post "two" | grep -q second-done || { echo "[e2e-auto] second turn failed" >&2; cat "$T/gw.log" >&2; exit 1; }
TR=$(ls "$T/sessions"/agent~3amain~3aslack~3achannel~3aC9.jsonl)
grep -q 'auto-ran-ok' "$TR" || { echo "[e2e-auto] reviewer-allowed command did not run" >&2; grep exec "$TR" >&2; exit 1; }
grep -q 'denied by auto reviewer' "$TR" || { echo "[e2e-auto] reviewer deny missing" >&2; exit 1; }
[[ -d "$T/ws/important" ]] || { echo "[e2e-auto] denied command ran!" >&2; exit 1; }
echo "[e2e-auto] exec mode=auto reviewer allow/deny via provider: PASS"
