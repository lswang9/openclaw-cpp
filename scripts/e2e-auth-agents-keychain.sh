#!/usr/bin/env bash
# Per-agent auth profiles (agent "work" uses its own store; others fall back to main) + libsecret keychain
# round trip when a Secret Service can be started (gnome-keyring under dbus-run-session); otherwise SKIP that half.
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT=$((22500 + RANDOM % 200)); FPORT=$((PORT + 60)); T="${TMPDIR:-/tmp}/oc-e2e-aak-$$"
mkdir -p "$T/home" "$T/data" "$T/sessions" "$T/state/agents/main/agent" "$T/state/agents/work/agent"
export HOME="$T/home" OPENCLAW_STATE_DIR="$T/state" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_KEYCHAIN=off
export OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
cat > "$T/fake.py" <<'P'
import json,sys
from http.server import BaseHTTPRequestHandler,HTTPServer
class H(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length",0)))
        who={"Bearer main-key":"from-main","Bearer work-key":"from-work"}.get(self.headers.get("Authorization",""))
        if not who: self.send_response(401); self.end_headers(); self.wfile.write(b'{"error":{"message":"bad"}}'); return
        b=json.dumps({"id":"c","choices":[{"message":{"role":"assistant","content":who},"finish_reason":"stop"}]}).encode()
        self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
HTTPServer(("127.0.0.1",int(sys.argv[1])),H).serve_forever()
P
python3 "$T/fake.py" "$FPORT" & SRV=$!; GW=
trap 'kill $SRV $GW 2>/dev/null||true; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-aak] $*" >&2; cat "$T/gw.log" 2>/dev/null | tail -20 >&2; exit 1; }
timeout 20 "$BIN" models auth paste-token --provider openai --profile-id openai:default <<<"main-key" >/dev/null
timeout 20 "$BIN" models auth paste-token --provider openai --profile-id openai:default --agent work <<<"work-key" >/dev/null
timeout 20 "$BIN" models auth list --agent work | grep -q 'agents/work/agent' || fail "--agent store path"
cat > "$OPENCLAW_CONFIG_PATH" <<J
{ agents: { list: [ { id: "main", default: true }, { id: "work" } ] },
  bindings: [ { agentId: "work", match: { channel: "slack", peer: { kind: "channel", id: "C8" } } } ] }
J
export OPENCLAW_DEFAULT_PROVIDER=openai OPENAI_BASE_URL="http://127.0.0.1:$FPORT" OPENAI_API_KEY=env-loses
export OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_GATEWAY_HOST=127.0.0.1
"$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
send() { curl -sf -m 20 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d "{\"type\":\"event_callback\",\"event\":{\"type\":\"message\",\"text\":\"hi\",\"user\":\"U1\",\"channel\":\"$1\",\"channel_type\":\"channel\"}}"; }
send C8 | grep -q from-work || fail "work agent did not use its own profile"
send C1 | grep -q from-main || fail "main agent profile"
kill $GW; wait $GW 2>/dev/null || true; GW=
rm -rf "$T/state/agents/work/agent/auth-profiles.json"
echo "[e2e-aak] per-agent auth profiles (work own store, main default): PASS"
# --- keychain ---
if ! command -v dbus-run-session >/dev/null || ! command -v gnome-keyring-daemon >/dev/null || ! compgen -G "/usr/lib/*/libsecret-1.so.0" >/dev/null; then
  echo "[e2e-aak] keychain: SKIP (no dbus-run-session/gnome-keyring/libsecret)"; exit 0; fi
cat > "$T/kc.sh" <<'K'
set -e
eval "$(printf 'pw\n' | gnome-keyring-daemon --unlock --components=secrets 2>/dev/null)" >/dev/null
export OPENCLAW_KEYCHAIN=auto
timeout 20 "$BIN" models auth keychain | grep -q '"libsecret"' || { timeout 20 "$BIN" models auth keychain; echo "SKIP-KC"; exit 0; }
timeout 20 "$BIN" models auth paste-token --provider openai --profile-id openai:kc <<<"kc-secret-123" >/dev/null
F="$OPENCLAW_STATE_DIR/agents/main/agent/auth-profiles.json"
grep -q kc-secret-123 "$F" && { echo "secret leaked into file"; exit 1; }
grep -q '"libsecret"' "$F" || { echo "no keychain marker"; cat "$F"; exit 1; }
timeout 20 "$BIN" models auth list | grep -q 'kc-s' || { echo "list did not hydrate from keychain"; exit 1; }
timeout 20 "$BIN" models auth remove openai:kc >/dev/null
echo KC-OK
K
R=$(BIN="$BIN" timeout 90 dbus-run-session -- bash "$T/kc.sh" 2>&1) || fail "keychain: $R"
case "$R" in *KC-OK*) echo "[e2e-aak] libsecret keychain store/lookup/remove (secret not in file): PASS";;
  *SKIP-KC*) echo "[e2e-aak] keychain: SKIP (secret service not usable here) $R";; *) fail "keychain: $R";; esac
