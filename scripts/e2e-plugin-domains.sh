#!/usr/bin/env bash
# 1.142: plugin registrations wired into their domains: context engine (plugins.slots.contextEngine → assemble before
# each model call + afterTurn), memory prompt supplements (system prompt), agent harness (agentRuntime.id → runAttempt
# replaces the built-in loop), speech provider (tts.convert → synthesize → audio file).
set -euo pipefail
export OPENCLAW_DEFAULT_PROVIDER=mock
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY OPENCLAW_TTS_URL 2>/dev/null || true
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
command -v node >/dev/null || { echo "[e2e-plugin-domains] SKIP (no node)"; exit 0; }
T=$(mktemp -d); GW=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-plugin-domains] FAIL: $*"; echo "T=$T"; tail -15 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN
mkdir -p "$T/src/domplug" "$T/home"
export HOME="$T/home" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_EXTENSIONS_DIR="$T/home/.openclaw/extensions" OPENCLAW_STATE_DIR="$T/st"
echo '[]' > "$T/script.json"
cat > "$T/cfg.json" <<'J'
{"plugins": {"slots": {"contextEngine": "trim-engine"}},
 "agents": {"list": [{"id": "main", "default": true}, {"id": "hx", "agentRuntime": {"id": "dom-harness"}}]},
 "messages": {"tts": {"provider": "dom-speech"}}}
J
echo '{ "id": "domplug", "name": "Domain wiring" }' > "$T/src/domplug/openclaw.plugin.json"
cat > "$T/src/domplug/index.mjs" <<J
import fs from 'node:fs';
const LOG = '$T/plugin.log';
const log = (o) => fs.appendFileSync(LOG, JSON.stringify(o) + '\n');
export default function register(api) {
  api.registerContextEngine('trim-engine', () => ({
    info: { id: 'trim-engine', name: 'Trim' },
    async ingest() { return { ingested: true }; },
    async assemble({ messages }) {
      const lastUser = [...messages].reverse().find((m) => m.role === 'user');
      return { messages: lastUser ? [lastUser] : messages, estimatedTokens: 1, systemPromptAddition: 'ENGINE-ON' };
    },
    async afterTurn({ messages, prePromptMessageCount }) { log({ afterTurn: messages.length, pre: prePromptMessageCount }); },
  }));
  api.registerMemoryPromptSupplement(({ agentId }) => ['MEM-SUPP for ' + agentId]);
  api.registerAgentHarness({ id: 'dom-harness', label: 'Dom', supports: () => ({ supported: true }),
    async runAttempt({ prompt, agentId }) { log({ harness: prompt, agentId }); return { assistantTexts: ['harness says: ' + prompt] }; } });
  api.registerSpeechProvider({ id: 'dom-speech', label: 'Dom Speech', isConfigured: () => true,
    async synthesize({ text }) { return { audioBuffer: Buffer.from('RIFF-fake:' + text), outputFormat: 'wav', fileExtension: '.wav' }; } });
}
J
"$BIN" plugins install "$T/src/domplug" >/dev/null
PORT=$(freeport)
OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_MOCK_ECHO=1 OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_PIDFILE="$T/pid" \
  "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 1 100); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
rpc() { curl -s -m 60 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
rpc chat.send '{"sessionKey":"agent:main:d","message":"first"}' > "$T/a.json"
rpc chat.send '{"sessionKey":"agent:main:d","message":"second"}' > "$T/b.json"
python3 - "$T/b.json" <<'PY' || fail "context engine + memory supplement: $(head -c 600 $T/b.json)"
import json,sys
s=open(sys.argv[1]).read()
assert "mock-echo n=2" in s, "engine should trim history to system + last user"
assert "ENGINE-ON" in s and "MEM-SUPP for main" in s, "system prompt additions"
assert "second]" in s and "first" not in s.split("last=")[1]
PY
for _ in $(seq 1 50); do grep -q afterTurn "$T/plugin.log" 2>/dev/null && break; sleep 0.1; done
grep -q '"afterTurn"' "$T/plugin.log" || fail "afterTurn not called"
# full transcript still holds both turns (engine only shapes the model input)
grep -rqF 'first' "$T/s" || fail "transcript lost history"
rpc chat.send '{"agentId":"hx","sessionKey":"agent:hx:x","message":"run it"}' > "$T/h.json"
grep -q 'harness says: .*run it' "$T/h.json" || fail "harness: $(head -c 400 $T/h.json)"
grep -q '"agentId": *"hx"\|"agentId":"hx"' "$T/plugin.log" || fail "harness agentId"
rpc tts.convert '{"text":"hello voice"}' > "$T/t.json"
python3 - "$T/t.json" <<'PY' || fail "speech provider: $(head -c 400 $T/t.json)"
import json,sys
p=json.load(open(sys.argv[1]))["payload"]
p=p.get("result",p) if isinstance(p,dict) else p
assert p["mode"]=="plugin" and p["provider"]=="dom-speech" and p["path"].endswith(".wav"), p
assert open(p["path"],"rb").read()==b"RIFF-fake:hello voice"
PY
echo "[e2e-plugin-domains] OK"
