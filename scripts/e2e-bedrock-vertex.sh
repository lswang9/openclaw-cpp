#!/usr/bin/env bash
# 1.137: Amazon Bedrock Converse (AWS SigV4 / bearer) + Google Vertex (ADC authorized_user + Anthropic/Gemini).
set -eu
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FL=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FL" ]] && kill $FL 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-bedrock-vertex] FAIL: $*"; echo "T=$T"; [[ -f "$T/gw.log" ]] && tail -20 "$T/gw.log"; [[ -f "$T/llm.log" ]] && tail -5 "$T/llm.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_DEFAULT_PROVIDER

FP=$(freeport); python3 "$ROOT/scripts/fake_cloud_llm.py" "$FP" "$T/llm.log" & FL=$!
B="http://127.0.0.1:$FP"

# ADC authorized_user pointing at the fake token endpoint
cat > "$T/adc.json" <<JSON
{"type":"authorized_user","client_id":"cid","client_secret":"csec","refresh_token":"rt-test"}
JSON
export GOOGLE_APPLICATION_CREDENTIALS="$T/adc.json"
export OPENCLAW_GOOGLE_TOKEN_URL="$B/token"
export GOOGLE_CLOUD_PROJECT=proj-test
export GOOGLE_CLOUD_LOCATION=us-east1

# Bedrock: SigV4 with dummy keys (fake only checks Credential= presence + X-Amz-Date)
export AWS_ACCESS_KEY_ID=AKIATEST
export AWS_SECRET_ACCESS_KEY=secretsecretsecretsecretsecret12
export AWS_REGION=us-east-1
unset AWS_SESSION_TOKEN AWS_BEARER_TOKEN_BEDROCK

python3 - "$T/cfg.json" "$B" <<'PY'
import json,sys
B=sys.argv[2]
cfg={
 "models":{"providers":{
   "amazon-bedrock":{"api":"bedrock-converse","auth":"aws-sdk","baseUrl":B,"region":"us-east-1",
     "models":[{"id":"anthropic.claude-test","maxTokens":256}]},
   "anthropic-vertex":{"api":"anthropic-vertex","auth":"google-adc","baseUrl":B,"project":"proj-test","location":"us-east1",
     "models":[{"id":"claude-test"}]},
   "google-vertex":{"api":"google-vertex","auth":"google-adc","baseUrl":B,"project":"proj-test","location":"us-east1",
     "models":[{"id":"gemini-test"}]}}},
 "agents":{"defaults":{"model":{"primary":"amazon-bedrock/anthropic.claude-test"}},
   "list":[{"id":"main"},{"id":"br","model":"amazon-bedrock/anthropic.claude-test"},
           {"id":"va","model":"anthropic-vertex/claude-test"},{"id":"vg","model":"google-vertex/gemini-test"}]}}
json.dump(cfg, open(sys.argv[1],"w"))
PY

PORT=$(freeport)
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; kill -0 $GW 2>/dev/null || fail "gateway died early"; sleep 0.2; done
[[ $(curl -sf -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/health") == 200 ]] || fail "gateway up"

ask() {
  curl -s -m 90 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' \
    -d "{\"id\":1,\"method\":\"chat.send\",\"params\":{\"agentId\":\"$1\",\"sessionKey\":\"agent:$1:t\",\"message\":\"what time\"}}" > "$T/r.json"
  grep -q "$2" "$T/r.json" || fail "$1: expected $2: $(head -c 600 $T/r.json)"
}
ask br bedrock-final
ask va vertex-anth-final
ask vg vertex-gem-final

python3 - "$T/llm.log" <<'PY' || fail "wire: $(head -c 800 $T/llm.log)"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
# token refresh happened
assert any(x.get("form",{}).get("refresh_token")=="rt-test" for x in L), L[:3]
# bedrock signed
b=[x for x in L if x["path"].endswith("/converse")]
assert b and b[0]["headers"].get("authorization","").startswith("AWS4-HMAC-SHA256 Credential=AKIATEST/"), b[0]["headers"]
assert "x-amz-date" in b[0]["headers"] and "x-amz-content-sha256" in b[0]["headers"]
assert "toolConfig" in b[0]["body"] and b[0]["body"]["messages"]
# vertex anth
a=[x for x in L if x["path"].endswith(":rawPredict")]
assert a and a[0]["headers"].get("authorization")=="Bearer ya29.test"
assert a[0]["body"].get("anthropic_version")=="vertex-2023-10-16"
assert "/projects/proj-test/locations/us-east1/publishers/anthropic/" in a[0]["path"]
# vertex gem
g=[x for x in L if ":generateContent" in x["path"] and "publishers/google" in x["path"]]
assert g and g[0]["headers"].get("authorization")=="Bearer ya29.test"
PY

# Bearer-token Bedrock path
kill $GW; wait $GW 2>/dev/null || true; GW=""
: > "$T/llm.log"
export AWS_BEARER_TOKEN_BEDROCK=bedrock-tok
unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s2" OPENCLAW_DATA_DIR="$T/d2" OPENCLAW_STATE_DIR="$T/st2" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; kill -0 $GW 2>/dev/null || fail "gateway died"; sleep 0.2; done
ask br bedrock-final
python3 - "$T/llm.log" <<'PY' || fail "bearer: $(cat $T/llm.log)"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
b=[x for x in L if x["path"].endswith("/converse")]
assert b and b[0]["headers"].get("authorization")=="Bearer bedrock-tok", b[0]["headers"] if b else L
PY

# Streaming
kill $GW; wait $GW 2>/dev/null || true; GW=""
: > "$T/llm.log"
unset AWS_BEARER_TOKEN_BEDROCK
export AWS_ACCESS_KEY_ID=AKIATEST AWS_SECRET_ACCESS_KEY=secretsecretsecretsecretsecret12
python3 - "$T/cfg.json" <<'PY'
import json,sys; c=json.load(open(sys.argv[1])); c["agents"]["defaults"]["streaming"]=True; json.dump(c,open(sys.argv[1],"w"))
PY
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s3" OPENCLAW_DATA_DIR="$T/d3" OPENCLAW_STATE_DIR="$T/st3" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; kill -0 $GW 2>/dev/null || fail "gateway died"; sleep 0.2; done
ask br bedrock-streamed
ask va vertex-anth-streamed
ask vg vertex-gem-streamed
python3 - "$T/llm.log" <<'PY' || fail "stream wire"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
assert any(x["path"].endswith("/converse-stream") for x in L)
assert any(x["path"].endswith(":streamRawPredict") for x in L)
assert any(":streamGenerateContent" in x["path"] for x in L)
PY

echo "[e2e-bedrock-vertex] OK"
