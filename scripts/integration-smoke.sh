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

echo "[smoke] GET /pairing + pairing.link start/approve/claim/verify"
curl -sf "http://${HOST}:${PORT}/pairing" | grep -q 'Device pairing'
rpc() { curl -sf -X POST "http://${HOST}:${PORT}/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
START=$(rpc '{"method":"pairing.link.start","params":{"device_name":"smoke"}}')
LID=$(printf '%s' "$START" | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["link"]["id"])')
SEC=$(printf '%s' "$START" | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["poll_secret"])')
rpc "{\"method\":\"pairing.link.approve\",\"params\":{\"id\":\"$LID\"}}" | grep -q '"approved"'
CLAIM=$(rpc "{\"method\":\"pairing.link.claim\",\"params\":{\"id\":\"$LID\",\"poll_secret\":\"$SEC\"}}")
TOK=$(printf '%s' "$CLAIM" | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["token"])')
rpc "{\"method\":\"pairing.token.verify\",\"params\":{\"token\":\"$TOK\"}}" | grep -q '"valid": *true' || { echo "[smoke] FAIL pairing verify" >&2; exit 1; }

echo "[smoke] gateway auth: OPENCLAW_GATEWAY_TOKEN + ocd_ device token (HTTP + WS) + revoke"
PORT2=$((PORT + 1))
SESS2=$(mktemp -d)
OPENCLAW_GATEWAY_TOKEN=smoke-gw-token OPENCLAW_SESSION_DIR="$SESS2" OPENCLAW_PIDFILE="$SESS2/pid" \
  "$BIN" gateway --port "$PORT2" --host "$HOST" >/tmp/oc-smoke2-$$.log 2>&1 &
GW2=$!
for i in $(seq 1 50); do curl -sf "http://${HOST}:${PORT2}/health" >/dev/null && break; sleep 0.1; done
U2="http://${HOST}:${PORT2}"
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
fail2() { echo "[smoke] FAIL $1" >&2; kill "$GW2" 2>/dev/null; rm -rf "$SESS2"; exit 1; }
[[ $(code "$U2/v1/status") == 401 ]] || fail2 "no-token should be 401"
[[ $(code -H 'Authorization: Bearer smoke-gw-token' "$U2/v1/status") == 200 ]] || fail2 "gw token"
[[ $(code -H 'Authorization: Bearer ocd_bogus' "$U2/v1/status") == 401 ]] || fail2 "bogus ocd"
S2=$(curl -sf -X POST "$U2/v1/rpc" -H 'Content-Type: application/json' -d '{"method":"pairing.link.start","params":{"device_name":"smoke2"}}') || fail2 "unauth start"
L2=$(printf '%s' "$S2" | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["link"]["id"])')
P2=$(printf '%s' "$S2" | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["poll_secret"])')
[[ $(code -X POST "$U2/v1/rpc" -H 'Content-Type: application/json' -d "{\"method\":\"pairing.link.approve\",\"params\":{\"id\":\"$L2\"}}") == 401 ]] || fail2 "unauth approve must be 401"
curl -sf -X POST "$U2/v1/rpc" -H 'Authorization: Bearer smoke-gw-token' -H 'Content-Type: application/json' \
  -d "{\"method\":\"device.pair.approve\",\"params\":{\"id\":\"$L2\"}}" | grep -q 'pairing.link.approve' || fail2 "legacy approve route"
C2=$(curl -sf -X POST "$U2/v1/rpc" -H 'Content-Type: application/json' -d "{\"method\":\"pairing.link.claim\",\"params\":{\"id\":\"$L2\",\"poll_secret\":\"$P2\"}}")
T2=$(printf '%s' "$C2" | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["token"])')
TID=$(printf '%s' "$C2" | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["token_id"])')
[[ $(code -H "Authorization: Bearer $T2" "$U2/v1/status") == 200 ]] || fail2 "ocd token HTTP"
[[ $(code -H "X-OpenClaw-Token: $T2" "$U2/v1/tools") == 200 ]] || fail2 "ocd token X-OpenClaw-Token"
WS=$(curl -s -o /dev/null -w '%{http_code}' --max-time 1 --http1.1 -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
  -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' -H "Authorization: Bearer $T2" "$U2/ws" || true)
[[ "$WS" == 101 ]] || fail2 "ocd token WS upgrade got $WS"
curl -s -X POST "$U2/v1/rpc" -H "Authorization: Bearer $T2" -H 'Content-Type: application/json' \
  -d "{\"method\":\"pairing.token.revoke\",\"params\":{\"token_id\":\"$TID\"}}" | grep -q 'forbidden' || fail2 "device self-revoke must be forbidden"
curl -sf -X POST "$U2/v1/rpc" -H "Authorization: Bearer smoke-gw-token" -H 'Content-Type: application/json' \
  -d "{\"method\":\"pairing.token.revoke\",\"params\":{\"token_id\":\"$TID\"}}" | grep -q revoked || fail2 "revoke"
[[ $(code -H "Authorization: Bearer $T2" "$U2/v1/status") == 401 ]] || fail2 "revoked token must be 401"

echo "[smoke] roles: device token forbidden from pairing admin; operator allowed; /login cookie"
newtok() { # $1=name $2=role -> prints token
  local s l p
  s=$(curl -sf -X POST "$U2/v1/rpc" -H 'Content-Type: application/json' -d "{\"method\":\"pairing.link.start\",\"params\":{\"device_name\":\"$1\"}}")
  l=$(printf '%s' "$s" | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["link"]["id"])')
  p=$(printf '%s' "$s" | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["poll_secret"])')
  curl -sf -X POST "$U2/v1/rpc" -H 'Authorization: Bearer smoke-gw-token' -H 'Content-Type: application/json' \
    -d "{\"method\":\"pairing.link.approve\",\"params\":{\"id\":\"$l\",\"role\":\"$2\"}}" >/dev/null
  curl -sf -X POST "$U2/v1/rpc" -H 'Content-Type: application/json' \
    -d "{\"method\":\"pairing.link.claim\",\"params\":{\"id\":\"$l\",\"poll_secret\":\"$p\"}}" \
    | python3 -c 'import sys,json;print(json.load(sys.stdin)["payload"]["token"])'
}
DTOK=$(newtok dev device); OTOK=$(newtok op operator)
rpcas() { curl -s -X POST "$U2/v1/rpc" -H "Authorization: Bearer $1" -H 'Content-Type: application/json' -d "$2"; }
rpcas "$DTOK" '{"method":"pairing.link.list","params":{}}' | grep -q 'forbidden: device role' || fail2 "device list must be forbidden"
rpcas "$DTOK" '{"method":"pairing.token.revoke","params":{"token_id":"x"}}' | grep -q 'forbidden' || fail2 "device revoke forbidden"
rpcas "$DTOK" '{"method":"secrets.list","params":{}}' | grep -q 'forbidden' || fail2 "device secrets forbidden"
rpcas "$DTOK" '{"method":"tools.list","params":{}}' | grep -q '"ok": *true\|"ok":true' || fail2 "device tools.list allowed"
[[ $(code -H "Authorization: Bearer $DTOK" "$U2/v1/config") == 403 ]] || fail2 "device /v1/config 403"
rpcas "$OTOK" '{"method":"pairing.link.list","params":{}}' | grep -q '"links"' || fail2 "operator list allowed"
rpcas "$OTOK" '{"method":"secrets.list","params":{}}' | grep -q 'forbidden: operator' || fail2 "operator secrets forbidden"
[[ $(code "$U2/pairing") == 302 ]] || fail2 "/pairing without auth should redirect"
[[ $(code "$U2/login") == 200 ]] || fail2 "/login public"
[[ $(code -X POST "$U2/login" -H 'Content-Type: application/json' -d '{"token":"nope"}') == 401 ]] || fail2 "bad login 401"
CJ=$(mktemp)
curl -sf -c "$CJ" -X POST "$U2/login" -H 'Content-Type: application/json' -d "{\"token\":\"$OTOK\"}" | grep -q operator || fail2 "login"
grep -q oc_token "$CJ" || fail2 "cookie set"
[[ $(code -b "$CJ" "$U2/pairing") == 200 ]] || fail2 "/pairing with cookie"
[[ $(code -b "$CJ" "$U2/board") == 200 ]] || fail2 "/board with cookie"
curl -sf -b "$CJ" "$U2/v1/whoami" | grep -q operator || fail2 "whoami operator"
rm -f "$CJ"
kill "$GW2" 2>/dev/null; wait "$GW2" 2>/dev/null || true; rm -rf "$SESS2"

echo "[smoke] ALL PASS"
