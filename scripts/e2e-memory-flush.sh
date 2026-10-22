#!/usr/bin/env bash
# Offline e2e: scripted mock provider drives a pre-compaction memory flush turn through the real gateway.
set -euo pipefail
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_PORT:-18797}"
T="${TMPDIR:-/tmp}/oc-e2e-flush-$$"
mkdir -p "$T/sessions" "$T/data"
cat > "$T/script.json" <<'J'
[
  {"content": "r1"},
  {"tool_calls": [{"name": "write", "arguments": {"path": "memory/flush.md", "content": "- durable: user likes oolong\n"}}]},
  {"content": "NO_REPLY"},
  {"content": "Earlier: user said hello and likes oolong."},
  {"content": "r2"}
]
J
cat > "$T/data/openclaw.json" <<'J'
{ agents: { defaults: { contextTokens: 30, compaction: { reserveTokens: 10, keepRecent: 1 } } } }
J
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_GATEWAY_PORT="$PORT"
export OPENCLAW_GATEWAY_HOST=127.0.0.1 OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data"
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$T"; _reap' EXIT
for _ in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
post() { curl -sf -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d "{\"type\":\"event_callback\",\"event\":{\"type\":\"message\",\"text\":\"$1\",\"user\":\"U1\",\"channel\":\"C9\",\"channel_type\":\"channel\"}}"; }
post "hello there, I like oolong tea very much" | grep -q '"r1"'
post "second message after a long chat" | grep -q '"r2"' || { echo "[e2e] wrong reply" >&2; cat "$T/gw.log" >&2; exit 1; }
[[ -f "$T/sessions/workspace/memory/flush.md" ]] || { echo "[e2e] memory flush did not write memory/flush.md" >&2; ls -R "$T/sessions" >&2; exit 1; }
grep -q "oolong" "$T/sessions/workspace/memory/flush.md"
TR=$(ls "$T/sessions"/agent~3amain*.jsonl)
grep -q "Summary of earlier conversation" "$TR" || { echo "[e2e] no compaction summary" >&2; exit 1; }
if grep -q "Pre-compaction memory flush" "$TR"; then echo "[e2e] flush prompt leaked into transcript" >&2; exit 1; fi
echo "[e2e] memory flush wrote memory/flush.md, compaction summarized, flush turn not persisted: PASS"
