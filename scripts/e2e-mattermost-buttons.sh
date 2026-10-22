#!/usr/bin/env bash
# 1.167: Mattermost interactive buttons — HMAC-signed props on send; callback updates post + agent turn.
set -eu
export OPENCLAW_DEFAULT_PROVIDER=mock
unset MATTERMOST_URL MATTERMOST_BOT_TOKEN OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN 2>/dev/null || true
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FK" ]] && kill $FK 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-mattermost-buttons] FAIL: $*"; tail -16 "$T/gw.log" 2>/dev/null; tail -8 "$T/f.log" 2>/dev/null; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
FP=$(freeport); python3 "$ROOT/scripts/fake_chatops.py" "$FP" "$T/f.log" & FK=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$FP/" >/dev/null 2>&1 && break; sleep 0.1; done
B="http://127.0.0.1:$FP"
PORT=$(freeport)
python3 - "$T/cfg.json" "$B" "$PORT" <<'PY'
import json,sys
B,P=sys.argv[2],sys.argv[3]
json.dump({"channels":{"mattermost":{"baseUrl":B,"botToken":"mm-tok","botUserId":"bot1",
  "dmPolicy":"open","groupPolicy":"open","mode":"poll","channels":["ch-seed"],
  "interactions":{"callbackBaseUrl":f"http://127.0.0.1:{P}"}}}}, open(sys.argv[1],"w"))
PY
echo '[{"content":"btn-ack"}]' > "$T/script.json"
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" \
  HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT OPENCLAW_MOCK_SCRIPT="$T/script.json" \
  "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
# outbound with buttons
SEND=$(curl -s "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"send","params":{"channel":"mattermost","to":"ch-btn","message":"Choose","buttons":[{"id":"yes-1","text":"Yes","style":"primary"},{"id":"no_2","text":"No"}]}}')
echo "$SEND" | python3 -c 'import json,sys;d=json.load(sys.stdin);r=d.get("result",d);assert r.get("ok") or r.get("payload",{}).get("ok") or r.get("delivered") or "recorded" in json.dumps(r),r' || fail "send: $SEND"
# extract signed context from fake log
CTX=$(python3 - "$T/f.log" <<'PY'
import json,sys
for l in open(sys.argv[1]):
    e=json.loads(l)
    if e.get("path")=="/api/v4/posts" and e.get("method")=="POST" and isinstance(e.get("body"),dict):
        props=e["body"].get("props") or {}
        atts=props.get("attachments") or []
        if not atts: continue
        acts=atts[0].get("actions") or []
        assert len(acts)>=2, acts
        a0=acts[0]
        assert a0["id"]=="yes1", a0  # sanitized
        assert a0["type"]=="button" and a0["name"]=="Yes"
        integ=a0["integration"]
        assert "/api/channels/mattermost/interactions" in integ["url"]
        ctx=integ["context"]
        assert "_token" in ctx and ctx["action_id"]=="yes1"
        print(json.dumps({"post_id": e["body"].get("id") or "p1", "context": ctx, "channel_id": e["body"]["channel_id"]}))
        break
else:
    raise SystemExit("no button post")
PY
) || fail "no signed button props in fake log"
# callback
RESP=$(python3 - "$PORT" "$CTX" <<'PY'
import json,sys,urllib.request
port,ctx=sys.argv[1],json.loads(sys.argv[2])
body={"user_id":"u-9","user_name":"alice","channel_id":ctx["channel_id"],"post_id":ctx["post_id"],"context":ctx["context"]}
req=urllib.request.Request(f"http://127.0.0.1:{port}/api/channels/mattermost/interactions",
    data=json.dumps(body).encode(), headers={"Content-Type":"application/json"}, method="POST")
print(urllib.request.urlopen(req, timeout=30).read().decode())
PY
)
echo "$RESP" | python3 -c 'import json,sys;d=json.load(sys.stdin); assert d=={} or d.get("ok") is not False, d' || fail "interaction resp: $RESP"
# post updated + agent reply posted
python3 - "$T/f.log" <<'PY' || fail "post update or reply missing"
import json,sys
logs=[json.loads(l) for l in open(sys.argv[1])]
puts=[e for e in logs if e.get("method")=="PUT" and e.get("path","").startswith("/api/v4/posts/")]
assert puts, "no PUT update"
att=(puts[-1]["body"].get("props") or {}).get("attachments") or []
assert att and "Yes" in att[0].get("text","") and "alice" in att[0].get("text",""), att
posts=[e for e in logs if e.get("path")=="/api/v4/posts" and e.get("method")=="POST" and (e.get("body") or {}).get("message")=="btn-ack"]
assert posts, "no agent reply btn-ack"
PY
# bad token rejected
CODE=$(python3 - "$PORT" "$CTX" <<'PY'
import json,sys,urllib.request,urllib.error
port,ctx=sys.argv[1],json.loads(sys.argv[2])
bad=dict(ctx["context"]); bad["_token"]="deadbeef"*8
body={"user_id":"u-9","user_name":"alice","channel_id":ctx["channel_id"],"post_id":ctx["post_id"],"context":bad}
req=urllib.request.Request(f"http://127.0.0.1:{port}/api/channels/mattermost/interactions",
    data=json.dumps(body).encode(), headers={"Content-Type":"application/json"}, method="POST")
try:
    urllib.request.urlopen(req, timeout=10); print(200)
except urllib.error.HTTPError as e:
    print(e.code)
PY
)
[[ "$CODE" == "403" ]] || fail "expected 403 got $CODE"
echo "[e2e-mattermost-buttons] OK"
