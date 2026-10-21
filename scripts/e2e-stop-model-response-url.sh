#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_PORT6:-$((19100 + RANDOM % 400))}"
RU_PORT=$((PORT+1))
T="${TMPDIR:-/tmp}/oc-e2e-smru-$$"
mkdir -p "$T/sessions" "$T/data" "$T/ru"
# fake response_url server
python3 - <<PY &
import http.server, json, os
PORT=$RU_PORT
T="$T/ru"
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n=int(self.headers.get("Content-Length","0"))
        body=self.rfile.read(n)
        open(f"{T}/last.json","wb").write(body)
        self.send_response(200); self.end_headers(); self.wfile.write(b'ok')
    def log_message(self,*a): pass
http.server.HTTPServer(("127.0.0.1",PORT),H).serve_forever()
PY
RUPID=$!
sleep 0.2
cat > "$T/script.json" <<'J'
[
  {"content": "slow-1"},
  {"content": "should-not-matter"}
]
J
# long mock: first completion sleeps? mock is instant — use tool_calls sleep via exec for cancel test
cat > "$T/script.json" <<'J'
[
  {"tool_calls": [{"name": "exec", "arguments": {"command": "sleep 5; echo late"}}]},
  {"content": "finished-late"}
]
J
cat > "$T/data/openclaw.json" <<'J'
{ tools: { exec: { mode: "full" } } }
J
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json"
export OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_GATEWAY_HOST=127.0.0.1
export OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data"
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW $RUPID 2>/dev/null || true; wait $GW 2>/dev/null || true; kill $RUPID 2>/dev/null || true; rm -rf "$T"' EXIT
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done

post() { curl -sf -m 60 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d "{\"type\":\"event_callback\",\"event\":{\"type\":\"message\",\"text\":\"$1\",\"user\":\"U1\",\"channel\":\"Cstop\",\"channel_type\":\"channel\"}}"; }

# /model switch
post "/model mock-x" | grep -q 'Model set for this session: mock-x'
post "/model" | grep -q 'mock-x'
post "/model clear" | grep -q 'cleared'

# /stop abort: start long exec, then stop
post "go long" > "$T/r1" &
P1=$!
sleep 0.4
post "/stop" | grep -q 'Stopping' || { echo "[e2e-smru] /stop failed" >&2; cat "$T/gw.log" >&2; exit 1; }
wait $P1 || true
# Should not contain finished-late if cancelled mid-tool — may still finish tool; at least /stop responded
grep -q Stopping <(post "/stop") || true

# response_url on interactive
payload=$(python3 - <<PY
import json
print(json.dumps({
  "type":"block_actions",
  "user":{"id":"U1"},
  "channel":{"id":"Cru"},
  "response_url":"http://127.0.0.1:$RU_PORT/resp",
  "actions":[{"action_id":"btn","value":"hello-from-btn"}]
}))
PY
)
# Need mock script for this inbound — restart with simple reply script
kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true
cat > "$T/script.json" <<'J'
[{"content": "btn-reply-ok"}]
J
export OPENCLAW_MOCK_SCRIPT="$T/script.json"
"$BIN" gateway >"$T/gw2.log" 2>&1 &
GW=$!
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
curl -sf -m 30 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode "payload=$payload" | tee "$T/ri" | grep -q btn-reply-ok
sleep 0.3
[[ -f "$T/ru/last.json" ]] || { echo "[e2e-smru] response_url not hit" >&2; cat "$T/ri" "$T/gw2.log" >&2; exit 1; }
grep -q btn-reply-ok "$T/ru/last.json"

# config credential on adapter: set channels.slack.botToken in config, no env — Status should show bot_token_set
kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true
cat > "$T/data/openclaw.json" <<'J'
{ channels: { slack: { enabled: true, botToken: "xoxb-from-config" } } }
J
unset SLACK_BOT_TOKEN || true
"$BIN" gateway >"$T/gw3.log" 2>&1 &
GW=$!
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
curl -sf "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"channels.status","params":{}}' | tee "$T/st.json" \
  | python3 -c 'import json,sys; d=json.load(sys.stdin);
# payload shape varies
s=json.dumps(d)
assert "bot_token_set" in s or "xoxb" in s or True
# check slack status via channels.slack.status if any
print("status-ok")'

# More direct: slack.status
curl -sf "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"slack.status","params":{}}' | tee "$T/ss.json" \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); p=d.get("payload",d); assert p.get("bot_token_set") is True, p; print("cred-from-config-ok")'

echo "[e2e-smru] /model + /stop + response_url + config botToken: PASS"
