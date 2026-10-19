#!/usr/bin/env bash
# Boots gateway with mock provider (no live LLM) and hits HTTP surface.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_SMOKE_PORT:-18796}"
HOST="${OPENCLAW_SMOKE_HOST:-127.0.0.1}"
PIDFILE="${TMPDIR:-/tmp}/openclaw-smoke-$$.pid"

if [[ ! -x "$BIN" ]]; then
  echo "missing binary: $BIN (build first)" >&2
  exit 2
fi

export OPENCLAW_DEFAULT_PROVIDER=mock
export OPENCLAW_PIDFILE="$PIDFILE"
export OPENCLAW_GATEWAY_PORT="$PORT"
export OPENCLAW_GATEWAY_HOST="$HOST"
# isolate session/data
export OPENCLAW_SESSION_DIR="${TMPDIR:-/tmp}/oc-smoke-sessions-$$"
export OPENCLAW_DATA_DIR="${TMPDIR:-/tmp}/oc-smoke-data-$$"
mkdir -p "$OPENCLAW_SESSION_DIR" "$OPENCLAW_DATA_DIR"

cleanup() {
  if [[ -f "$PIDFILE" ]]; then
    pid=$(cat "$PIDFILE" 2>/dev/null || true)
    if [[ -n "${pid:-}" ]]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
    rm -f "$PIDFILE"
  fi
  rm -rf "$OPENCLAW_SESSION_DIR" "$OPENCLAW_DATA_DIR"
}
trap cleanup EXIT

echo "[smoke] starting gateway mock on ${HOST}:${PORT}"
# Use a private pidfile for the shell job; gateway also writes OPENCLAW_PIDFILE — keep them same
"$BIN" gateway --port "$PORT" --host "$HOST" >/tmp/oc-smoke-$$.log 2>&1 &
echo $! >"$PIDFILE"
# wait for health
ok=0
for i in $(seq 1 50); do
  if curl -sf "http://${HOST}:${PORT}/health" >/dev/null; then ok=1; break; fi
  sleep 0.1
done
if [[ "$ok" != 1 ]]; then
  echo "[smoke] FAIL health" >&2
  cat /tmp/oc-smoke-$$.log >&2 || true
  exit 1
fi

echo "[smoke] GET /health"
curl -sf "http://${HOST}:${PORT}/health" | grep -q ok

echo "[smoke] GET /v1/status"
curl -sf "http://${HOST}:${PORT}/v1/status" | grep -q 'openclaw-cpp'

echo "[smoke] GET /v1/tools"
curl -sf "http://${HOST}:${PORT}/v1/tools" | grep -q '"tools"'

echo "[smoke] POST /v1/sessions"
CREATE=$(curl -sf -X POST "http://${HOST}:${PORT}/v1/sessions" \
  -H 'Content-Type: application/json' \
  -d '{"key":"smoke-1"}')
echo "$CREATE" | grep -q created

echo "[smoke] POST /v1/rpc tools.list"
RPC_TOOLS=$(curl -sf -X POST "http://${HOST}:${PORT}/v1/rpc" \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools.list","params":{}}')
echo "$RPC_TOOLS" | grep -q 'tools'

echo "[smoke] POST /v1/rpc sessions.create"
RPC_SESS=$(curl -sf -X POST "http://${HOST}:${PORT}/v1/rpc" \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"sessions.create","params":{"key":"smoke-2"}}')
echo "$RPC_SESS" | grep -q 'smoke-2\|ok\|result'

echo "[smoke] POST /v1/chat/completions (mock provider)"
CHAT=$(curl -sf -X POST "http://${HOST}:${PORT}/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  -d '{"session_id":"smoke-1","messages":[{"role":"user","content":"ping"}],"max_turns":1}')
echo "$CHAT" | grep -Eq 'mock|choices|content' || { echo "[smoke] FAIL chat: $CHAT" >&2; exit 1; }
echo "$CHAT" | head -c 200; echo

echo "[smoke] ALL PASS"
