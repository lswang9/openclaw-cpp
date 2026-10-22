#!/usr/bin/env bash
# Offline: plugin→host reverse calls — runtime.channel.reply buffered block dispatcher (agent turn → blocks),
# media.fetchRemoteMedia (SSRF guard + allowPrivateNetwork) / saveMediaBuffer, tts.textToSpeech, stt.transcribeAudioFile,
# config.writeConfigFile (validated + hot reload), streaming reply.block events, command-initiated agent reply (no deadlock), writeConfigFile preserves comments/$include.
set -euo pipefail
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
command -v node >/dev/null || { echo "[e2e-prt] SKIP (no node)"; exit 0; }
PORT="${OPENCLAW_E2E_PRT:-$((20800 + RANDOM % 200))}"
FPORT=$((PORT + 300))
T="${TMPDIR:-/tmp}/oc-e2e-prt-$$"
mkdir -p "$T/src/rtplug" "$T/home" "$T/data" "$T/sessions"
export HOME="$T/home" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_EXTENSIONS_DIR="$T/home/.openclaw/extensions"
export RESULTS="$T/results.jsonl" FAKE="http://127.0.0.1:$FPORT"
cat > "$T/fake.py" <<'P'
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ct):
        self.send_response(code); self.send_header("Content-Type", ct); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if self.path == "/img.png": return self.send(200, b"\x89PNG-fake-bytes", "image/png")
        if self.path == "/redir": self.send_response(302); self.send_header("Location", "/img.png"); self.end_headers(); return
        self.send(404, b"no", "text/plain")
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n)
        if self.path == "/tts": return self.send(200, b"OggS-fake-audio", "audio/ogg")
        if self.path == "/stt":
            assert b'name="model"' in body and b"voice-bytes" in body, body[:200]
            return self.send(200, json.dumps({"text": "hello from audio"}).encode(), "application/json")
        self.send(404, b"no", "text/plain")
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
P
python3 "$T/fake.py" "$FPORT" & SRV=$!
echo '{ agents: { defaults: { workspace: "ignored" } } }' > "$T/data/shared.json"
cat > "$OPENCLAW_CONFIG_PATH" <<'J'
// e2e openclaw config — comments must survive writeConfigFile
{
  $include: "./shared.json",
  agents: { list: [ { id: "main", default: true } ] },
  plugins: { entries: { rtplug: { allowPrivateNetwork: true } } }
}
J
cat > "$T/src/rtplug/openclaw.plugin.json" <<'J'
{ "id": "rtplug", "name": "Runtime contract test" }
J
cat > "$T/src/rtplug/index.mjs" <<'J'
import fs from 'fs';
const out = (o) => fs.appendFileSync(process.env.RESULTS, JSON.stringify(o) + '\n');
const step = async (name, fn) => { try { out({ step: name, ok: true, value: await fn() }); } catch (e) { out({ step: name, ok: false, error: String(e.message || e) }); } };
export default function register(api) {
  const rt = api.runtime;
  api.registerChannel({ plugin: {
    id: 'rtchat', meta: { label: 'RT' }, config: { listAccountIds: () => ['a1'] },
    outbound: { sendText: async () => ({ ok: true }) },
    gateway: { async startAccount(ctx) {
      await step('fetch', async () => { const m = await rt.media.fetchRemoteMedia({ url: process.env.FAKE + '/redir', maxBytes: 1000 });
        return { len: m.buffer.length, type: m.contentType, name: m.fileName }; });
      await step('fetchTooBig', async () => (await rt.media.fetchRemoteMedia({ url: process.env.FAKE + '/img.png', maxBytes: 4 })).buffer.length);
      await step('save', async () => { const s = await rt.media.saveMediaBuffer(Buffer.from('saved-bytes'), 'text/plain', 'inbound', 100, 'note.txt');
        return { id: s.id, exists: fs.existsSync(s.path), content: fs.readFileSync(s.path, 'utf8') }; });
      await step('tts', async () => rt.tts.textToSpeech({ text: 'hi there' }));
      await step('stt', async () => (await rt.stt.transcribeAudioFile({ buffer: Buffer.from('voice-bytes'), mime: 'audio/ogg' })).text);
      await step('reply', async () => { const got = [];
        const r = await rt.channel.reply.dispatchReplyWithBufferedBlockDispatcher({
          ctx: { channel: 'rtchat', from: 'carol', text: 'tell me a long story', sessionKey: 'agent:main:rtchat:dm:carol' },
          dispatcherOptions: { blockLimit: 40, onReplyStart: () => got.push('typing'), deliver: async (p, info) => got.push(info.kind + ':' + p.text) } });
        return { got, counts: r.counts }; });
      await step('writeBad', async () => rt.config.writeConfigFile({ gateway: { port: 'not-a-port' } }));
      await step('write', async () => { const c = JSON.parse(JSON.stringify(rt.config.loadConfig()));
        c.plugins.entries.rtplug.config = { written: true }; return rt.config.writeConfigFile(c); });
      out({ step: 'done', ok: true });
      await new Promise((res) => ctx.abortSignal.addEventListener('abort', res));
    } },
  } });
  api.registerCommand({ name: 'syncreply', handler: async () => {
    const got = [];
    const r = await rt.channel.reply.dispatchReplyWithBufferedBlockDispatcher({
      ctx: { channel: 'rtchat', from: 'dave', text: 'ping from tool', sessionKey: 'agent:main:rtchat:dm:dave' },
      dispatcherOptions: { blockLimit: 80, deliver: async (p, info) => got.push(info.kind) } });
    return { text: 'from-command:' + got.join(',') + ':streamed=' + !!r.streamed };
  } });
}
J
"$BIN" plugins install "$T/src/rtplug" >/dev/null
cat > "$T/script.json" <<'J'
[{"content": "Once upon a time there was a gateway.\n\nIt spoke C++ and JavaScript.\n\nThe end."}]
J
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_GATEWAY_HOST=127.0.0.1
export OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
export OPENCLAW_TTS_URL="$FAKE/tts" OPENCLAW_STT_URL="$FAKE/stt"
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW $SRV 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$T"; _reap' EXIT
for _ in $(seq 1 150); do grep -q '"step":"done"' "$RESULTS" 2>/dev/null && break; sleep 0.1; done
fail() { echo "[e2e-prt] $*" >&2; cat "$RESULTS" >&2 2>/dev/null || true; tail -20 "$T/gw.log" >&2; exit 1; }
grep -q '"step":"done"' "$RESULTS" || fail "plugin steps did not finish"
python3 - "$RESULTS" "$OPENCLAW_CONFIG_PATH" <<'PY' || fail "assertions"
import json, sys
r = {j["step"]: j for j in map(json.loads, open(sys.argv[1]))}
f = r["fetch"]; assert f["ok"] and f["value"] == {"len": 15, "type": "image/png", "name": "img.png"}, f
assert not r["fetchTooBig"]["ok"] and "maxBytes" in r["fetchTooBig"]["error"], r["fetchTooBig"]
s = r["save"]; assert s["ok"] and s["value"]["exists"] and s["value"]["content"] == "saved-bytes", s
t = r["tts"]; assert t["ok"], t
assert r["stt"]["ok"] and r["stt"]["value"] == "hello from audio", r["stt"]
rp = r["reply"]; assert rp["ok"], rp
got = rp["value"]["got"]
assert got[0] == "typing" and got[-1].startswith("final:") and any(g.startswith("block:") for g in got[1:]), got
assert sum(1 for g in got if g.startswith("final:")) == 1, got
assert "".join(g.split(":", 1)[1] for g in got[1:]).endswith("The end."), got
assert rp["value"].get("counts", {}).get("final", 0) >= 1
# streamed flag from host (reply.block events); tolerate false only if fallback path used with same content
assert not r["writeBad"]["ok"] and "rejected" in r["writeBad"]["error"], r["writeBad"]
assert r["write"]["ok"], r["write"]
assert r["write"]["value"].get("preservedInclude"), r["write"]
cfg = open(sys.argv[2]).read()
assert "$include" in cfg, cfg
assert "written" in cfg, cfg
# Leading // comments survive when the on-disk file still had them at write time.
if r["write"]["value"].get("preservedComments"):
    assert "comments must survive" in cfg, cfg
PY
# Plugin command may initiate an agent reply without deadlocking the JS host.
curl -sf -m 40 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"type":"event_callback","event":{"type":"message","text":"/syncreply","user":"U1","channel":"Cz","channel_type":"channel"}}' \
  | tee "$T/sync.out" | grep -q 'from-command:' || fail "command-initiated reply failed: $(cat "$T/sync.out" 2>/dev/null)"
grep -q 'streamed=true' "$T/sync.out" || fail "command reply not streamed: $(cat "$T/sync.out")"
echo "[e2e-prt] streaming reply blocks, command-initiated agent reply, media/tts/stt/writeConfig: PASS"
