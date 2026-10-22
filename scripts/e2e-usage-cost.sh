#!/usr/bin/env bash
# 1.115: usage/cost from transcripts with the cl100k tokenizer and configured model pricing.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
trap _reap EXIT
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
TOK="${OPENCLAW_TOKENIZER_PATH:-$HOME/.openclaw/tokenizers/cl100k_base.tiktoken}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-usage-cost] FAIL: $*"; tail -5 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home"
echo '[{"content":"hello world"}]' > "$T/script.json"
echo '{"agents":{"defaults":{"model":"acme/m1"}},"models":{"providers":{"acme":{"models":[{"id":"m1","cost":{"input":1000000,"output":2000000}}]}}}}' > "$T/cfg.json"
HAVE=0; [ -f "$TOK" ] && HAVE=1
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_TOKENIZER_PATH="$TOK"
HOME="$T/home" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" HAVE
python3 - <<'P' || fail "usage cost"
import json, os, urllib.request
def rpc(m, p):
    r = urllib.request.Request(os.environ["URL"], json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    return json.loads(urllib.request.urlopen(r, timeout=30).read())
rpc("chat.send", {"sessionKey": "c1", "message": "hello world"})
c = rpc("usage.cost", {})["payload"]
if os.environ["HAVE"] == "1":
    assert c["tokenizer"] == "cl100k_base", c
    assert c["outputTokens"] == 2, c            # "hello world" = 2 cl100k tokens
    assert abs(c["costUsd"] - (c["inputTokens"] * 1 + c["outputTokens"] * 2)) < 1e-6 and c["complete"], c
else:
    assert c["tokenizer"] == "heuristic", c
log = rpc("sessions.usage.logs", {"sessionKey": "c1"})["payload"]["entries"][0]; assert log["model"] == "acme/m1" and log["costUsd"] is not None
print("ok")
P
echo "[e2e-usage-cost] cl100k token counts + configured pricing: PASS (tokenizer file: $HAVE)"
