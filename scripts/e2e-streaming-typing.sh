#!/usr/bin/env bash
# Offline: token-level SSE streaming (content + split tool_call deltas) from an OpenAI-compatible fake,
# plugin onPartialReply/onTyping, and Discord typing keep-alive (POST /channels/{id}/typing) during a slow turn.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
command -v node >/dev/null || { echo "[e2e-st] SKIP (no node)"; exit 0; }
PORT=$((22800 + RANDOM % 200)); FP=$((PORT + 300)); T="${TMPDIR:-/tmp}/oc-e2e-st-$$"
mkdir -p "$T/src/stplug" "$T/home" "$T/data" "$T/sessions"
export HOME="$T/home" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_EXTENSIONS_DIR="$T/home/.openclaw/extensions" OPENCLAW_STATE_DIR="$T/state"
cat > "$T/fake.py" <<'P'
import json, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer
log = open(sys.argv[2], "a")
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        b = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if "/typing" in self.path:
            log.write(json.dumps({"typing": self.path, "auth": self.headers.get("Authorization")}) + "\n"); log.flush()
            self.send_response(204); self.send_header("Content-Length", "0"); self.end_headers(); return
        if self.path.endswith("/messages"):
            log.write(json.dumps({"sent": json.loads(b)}) + "\n"); log.flush()
            body = b'{"id":"m1"}'; self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body); return
        req = json.loads(b); log.write(json.dumps({"chat": req}) + "\n"); log.flush()
        assert req.get("stream") is True, req
        has_tool = any(m.get("role") == "tool" for m in req["messages"])
        self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.send_header("Connection", "close"); self.end_headers()
        def ev(d): self.wfile.write(("data: " + json.dumps(d) + "\n\n").encode()); self.wfile.flush(); time.sleep(0.25)
        if not has_tool:
            ev({"choices": [{"delta": {"role": "assistant", "tool_calls": [{"index": 0, "id": "call_x", "type": "function", "function": {"name": "no_such_tool", "arguments": "{\"a\""}}]}}]})
            ev({"choices": [{"delta": {"tool_calls": [{"index": 0, "function": {"arguments": ":1}"}}]}, "finish_reason": "tool_calls"}]})
        else:
            for piece in ["Hel", "lo ", "str", "eam"]: ev({"choices": [{"delta": {"content": piece}}]})
            ev({"choices": [{"delta": {}, "finish_reason": "stop"}]})
        self.wfile.write(b"data: [DONE]\n\n"); self.wfile.flush(); self.close_connection = True
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
P
python3 "$T/fake.py" "$FP" "$T/log" & SRV=$!; GW=
trap 'kill $SRV $GW 2>/dev/null || true; rm -rf "$T"' EXIT
cat > "$T/src/stplug/openclaw.plugin.json" <<'J'
{ "id": "stplug" }
J
cat > "$T/src/stplug/index.mjs" <<'J'
export default function register(api) {
  api.registerCommand({ name: 'streamit', handler: async () => {
    const deltas = []; let typing = 0; const kinds = [];
    const r = await api.runtime.channel.reply.dispatchReplyWithBufferedBlockDispatcher({
      ctx: { channel: 'stchat', from: 'erin', text: 'go', sessionKey: 'agent:main:stchat:dm:erin' },
      dispatcherOptions: { onPartialReply: (p) => deltas.push(p.text), onTyping: () => typing++, deliver: async (p, i) => kinds.push(i.kind) } });
    return { text: 'R:' + JSON.stringify({ deltas, typing, kinds, streamed: r.streamed }) };
  } });
}
J
"$BIN" plugins install "$T/src/stplug" >/dev/null
echo '{ agents: { defaults: { streaming: true } } }' > "$OPENCLAW_CONFIG_PATH"
export OPENCLAW_DEFAULT_PROVIDER=openai OPENAI_BASE_URL="http://127.0.0.1:$FP" OPENAI_API_KEY=k OPENCLAW_KEYCHAIN=off
export DISCORD_BOT_TOKEN=dtok DISCORD_API_BASE="http://127.0.0.1:$FP" OPENCLAW_TYPING_INTERVAL_MS=300
export OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_GATEWAY_HOST=127.0.0.1 OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data"
export OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
"$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
fail() { echo "[e2e-st] $*" >&2; tail -20 "$T/gw.log" >&2; cat "$T/log" >&2 2>/dev/null; exit 1; }
# 1) plugin command → streamed agent reply (tool-call deltas assembled, content deltas, typing events)
OUT=$(curl -sf -m 40 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"type":"event_callback","event":{"type":"message","text":"/streamit","user":"U1","channel":"Cs","channel_type":"channel"}}') || fail "plugin inbound"
python3 - "$OUT" "$T/log" <<'PY' || fail "plugin stream assertions: $OUT"
import json, sys
o = json.loads(sys.argv[1]); r = json.loads(o["reply"].split("R:", 1)[1])
assert r["deltas"] == ["Hel", "lo ", "str", "eam"], r
assert r["typing"] >= 2, r
assert r["streamed"] and r["kinds"][-1] == "final", r
chats = [json.loads(l)["chat"] for l in open(sys.argv[2]) if '"chat"' in l]
tool = [m for m in chats[1]["messages"] if m.get("role") == "tool"][0]
calls = [m for m in chats[1]["messages"] if m.get("tool_calls")][0]["tool_calls"]
assert tool["tool_call_id"] == "call_x" and calls[0]["function"]["arguments"] == '{"a":1}', (tool, calls)
PY
# 2) Discord inbound → typing keep-alive to the channel while the slow stream runs
: > "$T/log"
OUT=$(curl -sf -m 40 -X POST "http://127.0.0.1:$PORT/api/channels/discord/inbound" -H 'Content-Type: application/json' \
  -d '{"content":"hey","channel_id":"D77","author":{"id":"u9"},"guild_id":"G1"}') || fail "discord inbound"
echo "$OUT" | grep -q "Hello stream" || fail "discord reply: $OUT"
N=$(grep -c '/channels/D77/typing' "$T/log" || true)
[ "$N" -ge 2 ] || fail "expected >=2 typing pings, got $N"
grep -q '"Bot dtok"' "$T/log" || fail "typing auth header"
echo "[e2e-st] SSE token streaming (+tool_call deltas) → plugin onPartialReply/onTyping; Discord typing keep-alive x$N: PASS"
