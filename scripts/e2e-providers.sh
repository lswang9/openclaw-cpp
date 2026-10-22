#!/usr/bin/env bash
# 1.125: config-driven model providers routed per "provider/model": Anthropic Messages, Gemini generateContent,
# OpenAI Responses and chat completions (against an offline fake), incl. a tool-call round trip and SecretRef keys.
set -eu
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FL=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FL" ]] && kill $FL 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-providers] FAIL: $*"; echo "T=$T"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_DEFAULT_PROVIDER ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY
FP=$(freeport); python3 "$ROOT/scripts/fake_llm.py" "$FP" "$T/llm.log" & FL=$!
B="http://127.0.0.1:$FP"
export GEM_KEY=gk-test
cat > "$T/cfg.json" <<JSON
{"secrets":{"providers":{"envp":{"source":"env"}}},
 "models":{"providers":{
   "anth":{"api":"anthropic-messages","baseUrl":"$B","apiKey":"ak-test","models":[{"id":"claude-t","maxTokens":321}]},
   "gem":{"api":"google-generative-ai","baseUrl":"$B/v1beta","apiKey":{"source":"env","provider":"envp","id":"GEM_KEY"}},
   "resp":{"api":"openai-responses","baseUrl":"$B/v1","apiKey":"rk"},
   "cc":{"baseUrl":"$B/v1","apiKey":"ck","headers":{"X-Extra":"1"}}}},
 "agents":{"defaults":{"model":{"primary":"cc/default-model"}},
   "list":[{"id":"main"},{"id":"a1","model":"anth/claude-t"},{"id":"a2","model":"gem/gemini-t"},{"id":"a3","model":"resp/gpt-t"},{"id":"a4","model":"cc/my-model"}]}}
JSON
PORT=$(freeport)
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
rpc() { curl -s "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
ask() {  # agent expected-text
  rpc chat.send "{\"agentId\":\"$1\",\"sessionKey\":\"agent:$1:t\",\"message\":\"what time is it\"}" > "$T/r.json"
  grep -q "$2" "$T/r.json" || fail "$1: expected $2: $(head -c 600 $T/r.json)"
}
ask a1 anthropic-final
ask a2 gemini-final
ask a3 responses-final
ask a4 "chat-final:my-model"
ask main "chat-final:default-model"
python3 - "$T/llm.log" <<'PY' || fail "wire format: $(head -c 400 $T/llm.log)"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
a=[x for x in L if x["path"].endswith("/v1/messages")]
assert a and a[0]["headers"]["anthropic-version"]=="2023-06-01" and a[0]["body"]["max_tokens"]==321, a[0]
assert a[0]["body"]["tools"] and "input_schema" in a[0]["body"]["tools"][0], a[0]["body"].get("tools")
assert any(c.get("type")=="tool_result" and c["tool_use_id"]=="tu_1" for m in a[-1]["body"]["messages"] if isinstance(m["content"],list) for c in m["content"])
assert "system" in a[0]["body"] and all(m["role"]!="system" for m in a[0]["body"]["messages"])
g=[x for x in L if ":generateContent" in x["path"]]
assert g and g[0]["path"].endswith("/v1beta/models/gemini-t:generateContent") and g[0]["headers"]["x-goog-api-key"]=="gk-test"
assert "functionDeclarations" in g[0]["body"]["tools"][0]
r=[x for x in L if x["path"].endswith("/responses")]
assert r and r[0]["headers"]["authorization"]=="Bearer rk" and r[0]["body"]["model"]=="gpt-t"
c=[x for x in L if x["path"].endswith("/chat/completions")]
assert c and c[0]["headers"]["x-extra"]=="1" and c[0]["headers"]["authorization"]=="Bearer ck"
PY
grep -q "default model route: cc/default-model" "$T/gw.log" || fail "default route log"
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/cs" HOME="$T" "$BIN" chat "what time" 2>&1 | grep -q "chat-final:default-model" || fail "local chat via config model"
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/cs" HOME="$T" OPENCLAW_MODEL=anth/claude-t "$BIN" chat "what time" 2>&1 | grep -q "anthropic-final" || fail "local chat OPENCLAW_MODEL"
OPENCLAW_CONFIG_PATH="$T/cfg.json" HOME="$T" OPENCLAW_MODEL=anthropic/claude-x "$BIN" chat "x" 2>&1 | grep -q "no API key for provider \"anthropic\"" || fail "missing key message"
# ---- streaming (agents.defaults.streaming=true → CompleteStreaming): SSE for all four wire formats
kill $GW; wait $GW 2>/dev/null || true; GW=""; : > "$T/llm.log"
python3 - "$T/cfg.json" <<'PY'
import json,sys;p=sys.argv[1];c=json.load(open(p));c["agents"]["defaults"]["streaming"]=True;json.dump(c,open(p,"w"))
PY
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s2" OPENCLAW_DATA_DIR="$T/d2" OPENCLAW_STATE_DIR="$T/st2" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
ask a1 anthropic-streamed
ask a2 gemini-streamed
ask a3 responses-streamed
ask a4 "chat-streamed:my-model"
python3 - "$T/llm.log" <<'PY' || fail "stream wire: $(head -c 300 $T/llm.log)"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
assert all(x["body"].get("stream") for x in L if x["path"].endswith(("/v1/messages","/responses","/chat/completions"))), "stream flag"
assert any(":streamGenerateContent?alt=sse" in x["path"] for x in L)
assert any(x["headers"].get("accept")=="text/event-stream" for x in L if x["path"].endswith("/v1/messages"))
PY
echo "[e2e-providers] OK"
