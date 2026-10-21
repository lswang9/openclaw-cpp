#!/usr/bin/env bash
# 1.125: webhook ingress — POST /hooks/wake, /hooks/agent, mapped presets (gmail forEach), token rules, limits.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-hooks] FAIL: $*"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; exit 1; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
cat > "$T/cfg.json" <<'JSON'
{"hooks":{"enabled":true,"token":"hk-secret","presets":["gmail"],"allowRequestSessionKey":true,"allowedSessionKeyPrefixes":["hook:","agent:"],"maxBodyBytes":4096,
  "mappings":[{"id":"gh","match":{"path":"github"},"action":"agent","name":"GitHub","messageTemplate":"PR {{pull_request.number}}: {{pull_request.title}}","sessionKey":"hook:gh:{{pull_request.number}}"}]}}
JSON
OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
U="http://127.0.0.1:$PORT/hooks"
code() { curl -s -o "$T/out" -w '%{http_code}' "$@"; }
[[ $(code -X POST "$U/wake" -d '{"text":"x"}') == 401 ]] || fail "no token → 401"
[[ $(code -X POST "$U/wake" -H 'Authorization: Bearer nope' -d '{"text":"x"}') == 401 ]] || fail "bad token → 401"
[[ $(code -X POST "$U/wake?token=hk-secret" -d '{"text":"x"}') == 400 ]] || fail "query token → 400"
[[ $(code "$U/wake" -H 'Authorization: Bearer hk-secret') == 405 ]] || fail "GET → 405"
[[ $(code -X POST "$U/wake" -H 'X-OpenClaw-Token: hk-secret' -d '{}') == 400 ]] && grep -q "text required" "$T/out" || fail "wake text required"
[[ $(code -X POST "$U/wake" -H 'X-OpenClaw-Token: hk-secret' -d '{"text":"ping","mode":"next-heartbeat"}') == 200 ]] && grep -q '"next-heartbeat"' "$T/out" || fail "wake next-heartbeat: $(cat $T/out)"
[[ $(code -X POST "$U/agent" -H 'Authorization: Bearer hk-secret' -d '{"name":"x"}') == 400 ]] && grep -q "message required" "$T/out" || fail "agent message required"
[[ $(code -X POST "$U/agent" -H 'Authorization: Bearer hk-secret' -d '{"message":"hello","sessionKey":"evil:1"}') == 400 ]] && grep -q "allowedSessionKeyPrefixes" "$T/out" || fail "prefix policy"
[[ $(code -X POST "$U/agent" -H 'Authorization: Bearer hk-secret' -d '{"message":"hello","sessionKey":"hook:t1","waitForCompletion":true}') == 200 ]] || fail "agent wait: $(cat $T/out)"
python3 -c "import json;d=json.load(open('$T/out'));assert d['ok'] and d['sessionKey']=='agent:main:hook:t1' and d['text'],d" || fail "agent result $(cat $T/out)"
[[ $(code -X POST "$U/agent" -H 'Authorization: Bearer hk-secret' -d '{"message":"async"}') == 202 ]] && grep -q '"runId"' "$T/out" || fail "agent async"
[[ $(code -X POST "$U/github" -H 'Authorization: Bearer hk-secret' -d '{"pull_request":{"number":7,"title":"Fix"}}') == 202 ]] && grep -q 'agent:main:hook:gh:7' "$T/out" || fail "mapping: $(cat $T/out)"
[[ $(code -X POST "$U/gmail" -H 'Authorization: Bearer hk-secret' -d '{"messages":[{"id":"m1","from":"a@x","subject":"Hi"},{"id":"m2","from":"b@x","subject":"Yo"}]}') == 202 ]] || fail "gmail preset"
python3 -c "import json;d=json.load(open('$T/out'));k=[r['sessionKey'] for r in d['runs']];assert k==['agent:main:hook:gmail:m1','agent:main:hook:gmail:m2'],k" || fail "gmail forEach $(cat $T/out)"
[[ $(code -X POST "$U/nothing" -H 'Authorization: Bearer hk-secret' -d '{}') == 404 ]] || fail "unknown → 404"
python3 -c "print('{\"text\":\"' + 'x'*5000 + '\"}')" > "$T/big.json"
[[ $(code -X POST "$U/wake" -H 'Authorization: Bearer hk-secret' --data-binary @"$T/big.json") == 413 ]] || fail "413"
sleep 1; grep -q "hook run" "$T/gw.log" || curl -s "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d '{"id":1,"method":"logs.tail","params":{"n":200}}' | grep -q "hook run" || fail "hook run logged"
echo "[e2e-hooks] OK"
