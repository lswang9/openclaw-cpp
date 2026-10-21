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
export OPENCLAW_CONFIG_PATH="${OPENCLAW_DATA_DIR}/openclaw.json"
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

rpc() { curl -sf -X POST "http://${HOST}:${PORT}/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
echo "// hot reload test" > "$OPENCLAW_CONFIG_PATH.tmp"; echo "{ session: { reset: { mode: 'off' } }, skills: { entries: { 'hello-skill': { enabled: false } } }, }" >> "$OPENCLAW_CONFIG_PATH.tmp"
mv "$OPENCLAW_CONFIG_PATH.tmp" "$OPENCLAW_CONFIG_PATH"
RL=$(rpc '{"jsonrpc":"2.0","id":9,"method":"config.reload","params":{}}')
echo "$RL" | grep -Eq '"reloaded": ?true' && echo "$RL" | grep -Eq '"systemPromptChanged": ?true' || { echo "[smoke] hot reload failed: $RL" >&2; exit 1; }
sleep 1; echo "{ gateway: { port: 'nope' } }" > "$OPENCLAW_CONFIG_PATH"
rpc '{"jsonrpc":"2.0","id":10,"method":"config.reload","params":{}}' | grep -q 'expected integer' || { echo "[smoke] invalid reload not rejected" >&2; exit 1; }
rpc '{"jsonrpc":"2.0","id":11,"method":"tools.list","params":{}}' | grep -q tools
echo "[smoke] config hot reload + invalid config rejected (gateway keeps running)"
rm -f "$OPENCLAW_CONFIG_PATH"

curl -sf -X POST "http://${HOST}:${PORT}/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"type":"event_callback","event":{"type":"message","text":"hi","user":"U1","channel":"C1","channel_type":"channel","ts":"2.0","thread_ts":"1.0"}}' >/dev/null
grep -q 'agent:main:slack:channel:C1:thread:1.0' "$OPENCLAW_SESSION_DIR/sessions.json" || { echo "[smoke] upstream session key missing" >&2; exit 1; }
echo "[smoke] slack thread -> agent:main:slack:channel:C1:thread:1.0"

if command -v node >/dev/null 2>&1 && [ -z "${OPENCLAW_JS_FORCE_MOCK:-}" ]; then
  rpc '{"jsonrpc":"2.0","id":12,"method":"sample.status","params":{}}' | grep -q '"plugin": *"sample-upstream"' || { echo "[smoke] plugin gateway method failed" >&2; exit 1; }
  PING=$(curl -sf -X POST "http://${HOST}:${PORT}/api/channels/slack/inbound" -H 'Content-Type: application/json' \
    -d '{"type":"event_callback","event":{"type":"message","text":"/ping hey","user":"U2","channel":"D2","channel_type":"im","ts":"3.0"}}')
  echo "$PING" | grep -q 'pong hey' || { echo "[smoke] plugin command /ping failed: $PING" >&2; exit 1; }
  echo "[smoke] upstream JS plugin: gateway method + /ping command"
  HR=$(curl -sf -X POST "http://${HOST}:${PORT}/plugins/sample/hello" -H 'Content-Type: application/json' -d '{"name":"Al"}')
  echo "$HR" | grep -q '"hello":"Al"' || { echo "[smoke] plugin http route failed: $HR" >&2; exit 1; }
  EC=$(curl -sf -X POST "http://${HOST}:${PORT}/api/channels/echochan/inbound" -H 'Content-Type: application/json' -d '{"user":"u9","msg":"hi plugin channel"}')
  echo "$EC" | grep -q '"ok": *true' || { echo "[smoke] plugin channel inbound failed: $EC" >&2; exit 1; }
  rpc '{"jsonrpc":"2.0","id":14,"method":"sample.status","params":{}}' | grep -q '"to": *"u9"' || { echo "[smoke] plugin channel outbound missing" >&2; exit 1; }
  "$BIN" sample greet Smoke | grep -q 'hello, Smoke!' || { echo "[smoke] plugin CLI failed" >&2; exit 1; }
  echo "[smoke] plugin http route + plugin channel (in/out) + plugin CLI"
  cat > "$OPENCLAW_CONFIG_PATH" <<'CFG'
{ agents: { list: [ { id: "main", default: true }, { id: "work", model: "samplellm/echo-1", systemPrompt: "You are the work agent." } ] },
  bindings: [ { agentId: "work", match: { channel: "slack", peer: { kind: "channel", id: "C8" } } } ] }
CFG
  sleep 1
  MA=$(curl -sf -X POST "http://${HOST}:${PORT}/api/channels/slack/inbound" -H 'Content-Type: application/json' \
    -d '{"type":"event_callback","event":{"type":"message","text":"route me","user":"U8","channel":"C8","channel_type":"channel","ts":"8.0"}}')
  echo "$MA" | grep -q 'samplellm/echo-1' || { echo "[smoke] work agent model not used: $MA" >&2; exit 1; }
  WORK_SESS="$OPENCLAW_DATA_DIR/agents/work/sessions/sessions.json"
  grep -q 'agent:work:slack:channel:C8' "$WORK_SESS" || { echo "[smoke] agent:work session key missing in $WORK_SESS" >&2; ls -R "$OPENCLAW_DATA_DIR/agents" 2>/dev/null >&2; exit 1; }
  # main still uses the legacy OPENCLAW_SESSION_DIR; work must NOT share that index
  ! grep -q 'agent:work:' "$OPENCLAW_SESSION_DIR/sessions.json" 2>/dev/null || { echo "[smoke] work session leaked into main store" >&2; exit 1; }
  rpc '{"jsonrpc":"2.0","id":15,"method":"agents.route","params":{"channel":"slack","peerKind":"channel","peerId":"C1"}}' | grep -q '"agentId": *"main"' || { echo "[smoke] default route wrong" >&2; exit 1; }
  rm -f "$OPENCLAW_CONFIG_PATH"; sleep 1
  echo "[smoke] multi-agent binding: slack C8 -> agent:work (plugin provider model), others -> main"
fi

SI=$(curl -sf -X POST "http://${HOST}:${PORT}/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"payload":"{\"type\":\"block_actions\",\"user\":{\"id\":\"U5\"},\"channel\":{\"id\":\"C5\"},\"actions\":[{\"action_id\":\"openclaw_btn_0\",\"value\":\"approve\",\"text\":{\"text\":\"Approve\"}}]}"}')
echo "$SI" | grep -q 'clicked' || { echo "[smoke] slack block_actions failed: $SI" >&2; exit 1; }
grep -q 'agent:main:slack:channel:C5' "$OPENCLAW_SESSION_DIR/sessions.json" || { echo "[smoke] slack interactive session missing" >&2; exit 1; }
echo "[smoke] slack interactive block_actions -> agent turn"

DC=$(rpc '{"jsonrpc":"2.0","id":13,"method":"discord.commands","params":{}}')
echo "$DC" | grep -q '"name": *"status"' || { echo "[smoke] discord command specs missing native: $DC" >&2; exit 1; }
DS=$(curl -sf -X POST "http://${HOST}:${PORT}/api/channels/discord/inbound" -H 'Content-Type: application/json' \
  -d '{"type":2,"id":"1","token":"t","channel_id":"C7","guild_id":"G","member":{"user":{"id":"U7"}},"data":{"name":"status"}}')
echo "$DS" | grep -q 'Session agent:main:discord:channel:C7' || { echo "[smoke] discord /status failed: $DS" >&2; exit 1; }
echo "[smoke] discord slash /status (no model turn) + command specs"

echo "{ channels: { slack: { signingSecret: 'smoke-secret' } } }" > "$OPENCLAW_CONFIG_PATH"; sleep 1
rpc '{"jsonrpc":"2.0","id":16,"method":"config.reload","params":{}}' >/dev/null
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://${HOST}:${PORT}/api/channels/slack/inbound" -H 'Content-Type: application/json' -d '{"type":"event_callback","event":{"type":"message","text":"x","user":"U1","channel":"C1"}}')
[[ "$CODE" == "401" ]] || { echo "[smoke] unsigned slack request not rejected ($CODE)" >&2; exit 1; }
FORM='command=%2Fstatus&text=&user_id=U3&channel_id=D3&channel_name=directmessage&team_id=T1'
TS=$(date +%s)
SIG=$(python3 -c "import hmac,hashlib,sys;print('v0='+hmac.new(b'smoke-secret',('v0:'+sys.argv[1]+':'+sys.argv[2]).encode(),hashlib.sha256).hexdigest())" "$TS" "$FORM")
SL=$(curl -sf -X POST "http://${HOST}:${PORT}/api/channels/slack/inbound" -H 'Content-Type: application/x-www-form-urlencoded' \
  -H "X-Slack-Request-Timestamp: $TS" -H "X-Slack-Signature: $SIG" --data "$FORM")
echo "$SL" | grep -q '"text": *"Session agent:main' || { echo "[smoke] signed slack slash /status failed: $SL" >&2; exit 1; }
rm -f "$OPENCLAW_CONFIG_PATH"; sleep 1
echo "[smoke] slack signing secret: unsigned → 401, signed form-encoded slash /status ok"

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
LOGIN=$(curl -sf -c "$CJ" -X POST "$U2/login" -H 'Content-Type: application/json' -d "{\"token\":\"$OTOK\"}")
printf '%s' "$LOGIN" | grep -q operator || fail2 "login"
grep -q oc_session "$CJ" || fail2 "session cookie set"
grep -q "$OTOK" "$CJ" && fail2 "cookie must not contain raw token"
CSRF=$(awk '$6=="oc_csrf"{print $7}' "$CJ")
[[ -n "$CSRF" ]] || fail2 "csrf cookie"
[[ $(code -b "$CJ" "$U2/pairing") == 200 ]] || fail2 "/pairing with cookie"
[[ $(code -b "$CJ" "$U2/board") == 200 ]] || fail2 "/board with cookie"
curl -sf -b "$CJ" "$U2/pairing" | grep -q 'X-CSRF-Token' || fail2 "csrf shim injected"
curl -sf -b "$CJ" "$U2/v1/whoami" | grep -q operator || fail2 "whoami operator"
echo "[smoke] csrf: cookie POST without/with X-CSRF-Token"
[[ $(code -b "$CJ" -X POST "$U2/v1/rpc" -H 'Content-Type: application/json' -d '{"method":"pairing.link.list","params":{}}') == 403 ]] || fail2 "cookie POST without csrf must 403"
[[ $(code -b "$CJ" -X POST "$U2/v1/rpc" -H 'X-CSRF-Token: wrong' -H 'Content-Type: application/json' -d '{"method":"pairing.link.list","params":{}}') == 403 ]] || fail2 "bad csrf 403"
curl -sf -b "$CJ" -X POST "$U2/v1/rpc" -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' -d '{"method":"pairing.link.list","params":{}}' | grep -q '"links"' || fail2 "cookie POST with csrf"
echo "[smoke] ws roles: device forbidden / allowed over WebSocket; cookie WS origin check"
python3 scripts/ws_rpc.py "$U2/ws" "$DTOK" pairing.link.list | grep -q 'forbidden: device role' || fail2 "WS device pairing.link.list must be forbidden"
python3 scripts/ws_rpc.py "$U2/ws" "$DTOK" secrets.list | grep -q 'forbidden' || fail2 "WS device secrets forbidden"
python3 scripts/ws_rpc.py "$U2/ws" "$DTOK" tools.list | grep -q '"ok": true' || fail2 "WS device tools.list allowed"
python3 scripts/ws_rpc.py "$U2/ws" "$OTOK" pairing.link.list | grep -q '"ok": true' || fail2 "WS operator list allowed"
python3 scripts/ws_rpc.py "$U2/ws" "$OTOK" secrets.list | grep -q 'forbidden: operator' || fail2 "WS operator secrets forbidden"
SID=$(awk '$6=="oc_session"{print $7}' "$CJ")
HOSTP=${U2#http://}
python3 scripts/ws_rpc.py "$U2/ws" - pairing.link.list "Cookie: oc_session=$SID|Origin: http://$HOSTP" | grep -q '"ok": true' || fail2 "cookie WS same-origin"
python3 scripts/ws_rpc.py "$U2/ws" - tools.list "Cookie: oc_session=$SID|Origin: http://evil.example" && fail2 "cookie WS cross-origin must fail"
echo "[smoke] logout invalidates session server-side"
[[ $(code -b "$CJ" -X POST "$U2/logout") == 403 ]] || fail2 "logout without csrf 403"
[[ $(code -b "$CJ" -X POST -H "X-CSRF-Token: $CSRF" "$U2/logout") == 200 ]] || fail2 "logout"
[[ $(code -b "oc_session=$SID" "$U2/v1/whoami") == 401 ]] || fail2 "session must be dead after logout"
[[ $(code -b "oc_token=$OTOK" "$U2/v1/whoami") == 401 ]] || fail2 "legacy raw-token cookie no longer accepted"
rm -f "$CJ"
kill "$GW2" 2>/dev/null; wait "$GW2" 2>/dev/null || true; rm -rf "$SESS2"

"$BIN" --version | grep -q "openclaw-cpp" && echo "[smoke] --version ok"
if "$BIN" --definitely-unknown >/dev/null 2>&1; then echo "[smoke] unknown flag accepted" >&2; exit 1; fi; echo "[smoke] unknown flag rejected"
OPENCLAW_DEFAULT_PROVIDER=mock "$BIN" chat "hello" | grep -q . && echo "[smoke] chat (mock) ok"
"$BIN" skills check | grep -q eligible && echo "[smoke] skills check ok"
"$BIN" context | grep -q systemPromptChars && echo "[smoke] context ok"
CFGT="${TMPDIR:-/tmp}/oc-smoke-cfg-$$.json"
OPENCLAW_CONFIG_PATH="$CFGT" "$BIN" config set agents.defaults.model.primary deepseek/deepseek-chat >/dev/null
[[ "$(OPENCLAW_CONFIG_PATH="$CFGT" "$BIN" config get agents.defaults.model.primary)" == "deepseek/deepseek-chat" ]] && OPENCLAW_CONFIG_PATH="$CFGT" "$BIN" config validate >/dev/null && echo "[smoke] config set/get/validate ok"; rm -f "$CFGT" "$CFGT.bak"
OPENCLAW_DEFAULT_PROVIDER=mock "$BIN" chat -s smoke:reset "hi" >/dev/null
OPENCLAW_DEFAULT_PROVIDER=mock "$BIN" chat -s smoke:reset "/new" | grep -q "New session" && grep -q "previousSessionId" "$OPENCLAW_SESSION_DIR/sessions.json" && echo "[smoke] /new session reset ok"
ls "$OPENCLAW_SESSION_DIR"/archive/smoke~3areset.*.jsonl >/dev/null || { echo "[smoke] no jsonl transcript" >&2; exit 1; }
if grep -q "\"role\":\"system\"" "$OPENCLAW_SESSION_DIR"/archive/*.jsonl; then echo "[smoke] system prompt persisted" >&2; exit 1; fi; echo "[smoke] jsonl transcript (no system prompt persisted) ok"
"$BIN" skills list | grep -q "/hello_skill" && OPENCLAW_DEFAULT_PROVIDER=mock "$BIN" chat "/hello_skill hi" >/dev/null && echo "[smoke] skill slash command ok" || { echo "[smoke] skill slash failed" >&2; exit 1; }
"$BIN" tokenizer count "hello world" | grep -q "\"tokens\"" && "$BIN" tokenizer status | grep -q backend && echo "[smoke] tokenizer ok" || { echo "[smoke] tokenizer failed" >&2; exit 1; }
SK="${TMPDIR:-/tmp}/oc-smoke-skill-$$"; mkdir -p "$SK/echoer"
printf -- '---\nname: echoer\ndescription: dispatch test\ncommand-dispatch: tool\ncommand-tool: echo\n---\nbody\n' > "$SK/echoer/SKILL.md"
OUT=$(OPENCLAW_SKILLS_DIR="$SK" OPENCLAW_DEFAULT_PROVIDER=mock "$BIN" chat "/echoer hi")
echo "$OUT" | grep -q echoed && ! echo "$OUT" | grep -q mock-default || { echo "[smoke] command-dispatch failed: $OUT" >&2; exit 1; }
rm -rf "$SK"; echo "[smoke] skill command-dispatch: tool (no model turn) ok"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-memory-flush.sh"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-embeddings.sh"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-ts-plugin.sh"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-exec-approvals.sh"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-exec-sqlite-bg.sh"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-hot-reload-channels.sh"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-config-lock.sh"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-stop-model-response-url.sh"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-plugins-install.sh"
OPENCLAW_BIN="$BIN" bash "$ROOT/scripts/e2e-plugin-contracts.sh"
echo "[smoke] ALL PASS"
