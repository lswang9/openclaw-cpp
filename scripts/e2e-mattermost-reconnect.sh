#!/usr/bin/env bash
# 1.158: Mattermost WebSocket reconnect backoff (upstream runWithReconnect: initial, ×2 on failure up to max, reset after a
# session that connected) + ping/pong liveness, against the offline fake (fake_live_transports.py /__mm_ctl).
set -eu
export OPENCLAW_DEFAULT_PROVIDER=mock
unset MATTERMOST_URL MATTERMOST_BOT_TOKEN OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN 2>/dev/null || true
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FK" ]] && kill $FK 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"' EXIT
fail() { echo "[e2e-mattermost-reconnect] FAIL: $*"; echo "T=$T"; [[ -f "$T/gw.log" ]] && grep mattermost "$T/gw.log" | tail -12; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
HP=$(freeport); IP=$(freeport); python3 "$ROOT/scripts/fake_live_transports.py" "$HP" "$IP" "$T/f.log" & FK=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$HP/x" >/dev/null 2>&1 && break; sleep 0.1; done
B="http://127.0.0.1:$HP"
echo "{\"channels\":{\"mattermost\":{\"baseUrl\":\"$B\",\"botToken\":\"mm-tok\",\"dmPolicy\":\"open\",\"groupPolicy\":\"open\"}}}" > "$T/cfg.json"
PORT=$(freeport)
OPENCLAW_MATTERMOST_RECONNECT_INITIAL_MS=200 OPENCLAW_MATTERMOST_RECONNECT_MAX_MS=800 OPENCLAW_MATTERMOST_RECONNECT_JITTER=0 \
OPENCLAW_MATTERMOST_PING_MS=400 OPENCLAW_MATTERMOST_PONG_TIMEOUT_MS=300 \
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
cnt() { python3 -c 'import json,sys;print(sum(1 for l in open(sys.argv[1]) if sys.argv[2] in json.loads(l)))' "$T/f.log" "$1"; }
waitn() { for i in $(seq 1 100); do [[ $(cnt "$1") -ge $2 ]] && return 0; sleep 0.1; done; fail "expected ≥$2 $1 (got $(cnt "$1"))"; }
ctl() { curl -s -X POST "$B/__mm_ctl" -H 'Content-Type: application/json' -d "$1" >/dev/null; }
waitn ws_connect 1; waitn ws_ping 2
[[ $(cnt ws_connect) == 1 ]] || fail "answered pings must keep the socket"
# drop + 3 failing handshakes: delays 200 (reset after a live session), 200, 400, 800
ctl '{"fail":3,"drop":true}'
waitn ws_connect 5
python3 - "$T/f.log" <<'PY' || fail "backoff gaps wrong"
import json,sys
ts=[json.loads(l)["ws_connect"] for l in open(sys.argv[1]) if "ws_connect" in json.loads(l)][1:5]
g=[b-a for a,b in zip(ts,ts[1:])]
exp=[0.2,0.4,0.8]
ok=all(e*0.8<=x<=e+0.45 for x,e in zip(g,exp))
print("gaps",[round(x,2) for x in g]); sys.exit(0 if ok else 1)
PY
for d in 200 400 800; do grep -q "mattermost reconnecting in ${d}ms" "$T/gw.log" || fail "no ${d}ms reconnect log"; done
# pong timeout → reconnect (delay reset to initial since the session had connected)
N=$(cnt ws_connect); ctl '{"nopong":true}'
for i in $(seq 1 60); do grep -q 'pong timeout' "$T/gw.log" && break; sleep 0.1; done
grep -q 'pong timeout' "$T/gw.log" || fail "no pong timeout"
ctl '{"nopong":false}'
waitn ws_connect $((N+1))
# traffic still flows after reconnects
sleep 0.5
curl -s -X POST "$B/__mm_push" -H 'Content-Type: application/json' -d '{"channel_type":"D","post":{"id":"p9","channel_id":"dm-1","user_id":"u-alice","message":"after reconnect","type":""}}' >/dev/null
for i in $(seq 1 100); do grep -q '"/api/v4/posts"' "$T/f.log" && break; sleep 0.1; done
grep -q '"/api/v4/posts"' "$T/f.log" || fail "no reply after reconnect"
echo "[e2e-mattermost-reconnect] PASS"
