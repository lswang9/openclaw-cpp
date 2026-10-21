#!/usr/bin/env bash
# Built-in OpenAI-compat uses auth-profiles; rotates on HTTP 401 when auth.order lists two profiles.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_AR_PORT:-$((21800 + RANDOM % 200))}"
FPORT=$((PORT+50))
T="${TMPDIR:-/tmp}/oc-e2e-ar-$$"
mkdir -p "$T/home" "$T/data" "$T/sessions" "$T/state/agents/main/agent"
export HOME="$T/home" OPENCLAW_STATE_DIR="$T/state" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json"
export OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
cat > "$T/fake.py" <<'P'
import json,sys
from http.server import BaseHTTPRequestHandler,HTTPServer
log=open(sys.argv[2],"a")
class H(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_POST(self):
        n=int(self.headers.get("Content-Length",0)); self.rfile.read(n)
        auth=self.headers.get("Authorization","")
        log.write(auth+"\n"); log.flush()
        if auth!="Bearer good-key":
            self.send_response(401); self.send_header("Content-Type","application/json"); self.end_headers()
            self.wfile.write(b'{"error":{"message":"bad key"}}'); return
        msg={"id":"c","choices":[{"message":{"role":"assistant","content":"rotated-ok"},"finish_reason":"stop"}]}
        b=json.dumps(msg).encode()
        self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
HTTPServer(("127.0.0.1",int(sys.argv[1])),H).serve_forever()
P
python3 "$T/fake.py" "$FPORT" "$T/authlog" & SRV=$!
trap 'kill $SRV $GW 2>/dev/null||true; rm -rf "$T"' EXIT
sleep 0.3
cat > "$T/state/agents/main/agent/auth-profiles.json" <<'J'
{"version":1,"profiles":{
  "openai:bad":{"type":"api_key","provider":"openai","key":"bad-key"},
  "openai:good":{"type":"api_key","provider":"openai","key":"good-key"}
}}
J
chmod 600 "$T/state/agents/main/agent/auth-profiles.json"
cat > "$OPENCLAW_CONFIG_PATH" <<J
{ auth: { order: { openai: ["openai:bad", "openai:good"] } },
  agents: { list: [ { id: "main", default: true, model: "openai/gpt-test" } ] } }
J
export OPENCLAW_DEFAULT_PROVIDER=openai OPENAI_BASE_URL="http://127.0.0.1:$FPORT" OPENAI_API_KEY=env-should-not-win
export OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_GATEWAY_HOST=127.0.0.1
echo '[]' > "$T/script.json"; export OPENCLAW_MOCK_SCRIPT="$T/script.json"
"$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
curl -sf -m 30 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"type":"event_callback","event":{"type":"message","text":"hi","user":"U1","channel":"Cr","channel_type":"channel"}}' \
  | tee "$T/out" | grep -q rotated-ok || { echo "[e2e-ar] rotation failed"; cat "$T/gw.log" "$T/authlog" "$T/out"; exit 1; }
python3 -c "a=open('$T/authlog').read().strip().splitlines(); assert a[0].endswith('bad-key') and a[1].endswith('good-key'), a"
echo "[e2e-ar] auth-profiles + multi-profile rotation on 401: PASS"
