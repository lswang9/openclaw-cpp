#!/usr/bin/env bash
# A2UI v0.8 host: validate (v0.9 rejected), push JSONL, server render (text, data binding, template list, escaping),
# Button → userAction with resolved context recorded as canvas event, deleteSurface, auth required.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); PORT=$((20000 + RANDOM % 10000))
export OPENCLAW_STATE_DIR="$T/state" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/openclaw.json" HOME="$T" OPENCLAW_SESSION_DIR="$T/sess"
export OPENCLAW_GATEWAY_TOKEN="gw-$RANDOM"
fail() { echo "[e2e-a2ui] FAIL: $*"; tail -20 "$T/gw.log"; kill $GW 2>/dev/null || true; exit 1; }
"$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 50); do curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break; sleep 0.1; done
U="http://127.0.0.1:$PORT"; A=(-H "Authorization: Bearer $OPENCLAW_GATEWAY_TOKEN")
rpc() { curl -s "${A[@]}" -X POST "$U/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
J=$(python3 - <<'PY'
import json
L=[{"surfaceUpdate":{"surfaceId":"main","components":[
 {"id":"root","component":{"Column":{"children":{"explicitList":["title","list","note","chk","btn","evil"]}}}},
 {"id":"title","component":{"Text":{"text":{"path":"/title"},"usageHint":"h1"}}},
 {"id":"list","component":{"List":{"children":{"template":{"componentId":"item","dataBinding":"/items"}}}}},
 {"id":"item","component":{"Text":{"text":{"path":"name"}}}},
 {"id":"note","component":{"TextField":{"label":{"literalString":"Note"},"text":{"path":"/note"}}}},
 {"id":"chk","component":{"CheckBox":{"label":{"literalString":"Ship"},"value":{"path":"/ship"}}}},
 {"id":"btnlabel","component":{"Text":{"text":{"literalString":"Approve"}}}},
 {"id":"btn","component":{"Button":{"child":"btnlabel","action":{"name":"approve","context":[{"key":"title","value":{"path":"/title"}},{"key":"n","value":{"literalNumber":3}},{"key":"note","value":{"path":"/note"}},{"key":"ship","value":{"path":"/ship"}}]}}}},
 {"id":"evil","component":{"Text":{"text":{"literalString":"<script>alert(1)</script>"}}}}]}},
 {"dataModelUpdate":{"surfaceId":"main","contents":[{"key":"title","valueString":"Deploy plan"},
   {"key":"items","valueMap":[{"key":"a","valueMap":[{"key":"name","valueString":"step-one"}]},{"key":"b","valueMap":[{"key":"name","valueString":"step-two"}]}]}]}},
 {"beginRendering":{"surfaceId":"main","root":"root"}}]
print(json.dumps("\n".join(json.dumps(x) for x in L)))
PY
)
rpc '{"method":"canvas.a2ui.validate","params":{"jsonl":"{\"createSurface\":{\"surfaceId\":\"x\"}}"}}' | grep -q 'v0.9' || fail "v0.9 must be rejected"
rpc '{"method":"canvas.a2ui.validate","params":{"jsonl":"{\"surfaceUpdate\":{}}"}}' | grep -q 'surfaceId required' || fail "missing surfaceId"
rpc "{\"method\":\"canvas.a2ui.validate\",\"params\":{\"jsonl\":$J}}" | grep -Eq '"messageCount": *3' || fail "validate count"
rpc "{\"method\":\"canvas.a2ui.pushJSONL\",\"params\":{\"jsonl\":$J}}" | grep -Eq '"applied": *3' || fail "push"
[[ $(curl -s -o /dev/null -w '%{http_code}' "$U/__openclaw__/a2ui/") == 302 ]] || fail "page needs auth"
P=$(curl -s "${A[@]}" "$U/__openclaw__/a2ui/")
for s in '<h1' 'Deploy plan' 'step-one' 'step-two' 'Approve' '&lt;script&gt;alert(1)'; do printf '%s' "$P" | grep -qF "$s" || fail "render missing $s"; done
printf '%s' "$P" | grep -qF '<script>alert(1)' && fail "xss not escaped"
IN() { curl -s -o /dev/null -w '%{http_code}' "${A[@]}" -X POST "$U/__openclaw__/a2ui/input" -H 'Content-Type: application/json' -d "$1"; }
[[ $(IN '{"surfaceId":"main","componentId":"note","value":"typed-by-user"}') == 200 ]] || fail "textfield input"
[[ $(IN '{"surfaceId":"main","componentId":"chk","value":true}') == 200 ]] || fail "checkbox input"
[[ $(IN '{"surfaceId":"main","componentId":"title","value":"x"}') == 400 ]] || fail "non-input must be 400"
curl -s "${A[@]}" "$U/__openclaw__/a2ui/" | grep -q 'value="typed-by-user"' || fail "input reflected in render"
R=$(curl -s "${A[@]}" -X POST "$U/__openclaw__/a2ui/action" -H 'Content-Type: application/json' -d '{"surfaceId":"main","componentId":"btn"}')
printf '%s' "$R" | grep -q '"name":"approve"' || fail "userAction name: $R"
printf '%s' "$R" | grep -q '"title":"Deploy plan"' || fail "context path resolved"
printf '%s' "$R" | grep -q '"n":3' || fail "context literal"
printf '%s' "$R" | grep -q '"note":"typed-by-user"' || fail "context from user input"
printf '%s' "$R" | grep -q '"ship":true' || fail "checkbox context"
printf '%s' "$R" | grep -q '"sessionKey":"a2ui:main"' || fail "delivered to agent session"
rpc '{"method":"sessions.list"}' > /tmp/a2sl.json; rpc '{"method":"chat.history","params":{"key":"agent:main:main"}}' > /tmp/a2h.json; grep -q "A2UI userAction" /tmp/a2h.json || fail "userAction in session history"
[[ $(curl -s -o /dev/null -w '%{http_code}' "${A[@]}" -X POST "$U/__openclaw__/a2ui/action" -H 'Content-Type: application/json' -d '{"surfaceId":"main","componentId":"title"}') == 404 ]] || fail "non-button 404"
rpc '{"method":"canvas.events","params":{"canvas_id":"a2ui"}}' | grep -q 'a2ui.userAction' || fail "event recorded"
rpc '{"method":"canvas.a2ui.push","params":{"messages":[{"deleteSurface":{"surfaceId":"main"}}]}}' | grep -Eq '"surfaces": *\[ *\]' || fail "deleteSurface"
rpc '{"method":"canvas.a2ui.push","params":{"text":"Hello from A2UI"}}' | grep -q '"main"' || fail "text helper"
curl -s "${A[@]}" "$U/__openclaw__/a2ui/" | grep -q 'Hello from A2UI' || fail "text render"
kill $GW; wait $GW 2>/dev/null || true
echo "[e2e-a2ui] PASS"
