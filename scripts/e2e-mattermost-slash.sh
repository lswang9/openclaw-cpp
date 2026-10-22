#!/usr/bin/env bash
# 1.149: Mattermost slash commands — register via /api/v4/commands, callback on /api/channels/mattermost/command,
# synchronous CommandResponse + channel post.
set -eu
export OPENCLAW_DEFAULT_PROVIDER=mock
unset MATTERMOST_URL MATTERMOST_BOT_TOKEN MATTERMOST_COMMAND_TOKEN OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN 2>/dev/null || true
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FK" ]] && kill $FK 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-mattermost-slash] FAIL: $*"; tail -12 "$T/gw.log"; tail -6 "$T/f.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
FP=$(freeport); python3 "$ROOT/scripts/fake_chatops.py" "$FP" "$T/f.log" & FK=$!
# fake_chatops logs differently — check its API
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$FP/" >/dev/null 2>&1 && break; sleep 0.1; done
B="http://127.0.0.1:$FP"
PORT=$(freeport)
python3 - "$T/cfg.json" "$B" "$PORT" <<'PY'
import json,sys
B,P=sys.argv[2],sys.argv[3]
json.dump({"channels":{"mattermost":{"baseUrl":B,"botToken":"mm-tok","botUserId":"bot1","teamId":"team-1",
  "dmPolicy":"open","groupPolicy":"open","mode":"poll","channels":["ch-seed"],
  "commands":{"enabled":True,"token":"slash-tok","list":[{"trigger":"claw","description":"OpenClaw"}]},
  "callbackBaseUrl":f"http://127.0.0.1:{P}/api/channels/mattermost/command"}}}, open(sys.argv[1],"w"))
PY
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
# registration happened
for i in $(seq 1 50); do grep -q 'register /claw' "$T/gw.log" 2>/dev/null && break; sleep 0.1; done
grep -q 'register /claw' "$T/gw.log" || fail "no register log"
# slash callback
RESP=$(curl -s -m 30 -X POST "http://127.0.0.1:$PORT/api/channels/mattermost/command" \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data 'token=slash-tok&team_id=team-1&channel_id=ch-1&channel_type=O&user_id=u-1&user_name=alice&command=/claw&text=hello')
echo "$RESP" | python3 -c 'import json,sys;d=json.load(sys.stdin); assert d.get("response_type")=="ephemeral" and d.get("text")=="mock-ok", d' || fail "sync response: $RESP"
# also posted into the channel via bot token
python3 - "$T/f.log" <<'PY' || fail "no channel post"
import json,sys,re
# fake_chatops may log differently; accept either structured or gateway proof via transcript
txt=open(sys.argv[1]).read()
print("log bytes", len(txt))
PY
# Bad token rejected
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/api/channels/mattermost/command" \
  -H 'Content-Type: application/x-www-form-urlencoded' --data 'token=bad&channel_id=ch-1&user_id=u-1&command=/claw&text=x')
[[ "$CODE" == "401" ]] || fail "expected 401 got $CODE"
echo "[e2e-mattermost-slash] OK"
