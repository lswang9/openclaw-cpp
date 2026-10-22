#!/usr/bin/env bash
# 1.148: Signal attachments (outbound data-URI / inbound getAttachment) + reactions (sendReaction out, inbound reaction note).
set -eu
export OPENCLAW_DEFAULT_PROVIDER=mock
unset SIGNAL_API_BASE SIGNAL_NUMBER SIGNAL_HTTP_URL OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN 2>/dev/null || true
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FK" ]] && kill $FK 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-signal-rich] FAIL: $*"; tail -10 "$T/gw.log" 2>/dev/null; tail -6 "$T/f.log" 2>/dev/null; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
HP=$(freeport); IP=$(freeport); python3 "$ROOT/scripts/fake_live_transports.py" "$HP" "$IP" "$T/f.log" & FK=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$HP/x" >/dev/null 2>&1 && break; sleep 0.1; done
B="http://127.0.0.1:$HP"
printf '%s\n' "{\"channels\":{\"signal\":{\"httpUrl\":\"$B\",\"number\":\"+15550001\",\"dmPolicy\":\"open\",\"groupPolicy\":\"open\"}}}" > "$T/cfg.json"
PORT=$(freeport)
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
rpc() { curl -s -m 30 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
waitlog() { for i in $(seq 1 100); do python3 - "$T/f.log" "$1" <<'PY' && return 0
import json,sys
for l in open(sys.argv[1]):
    e=json.loads(l)
    if eval(sys.argv[2]): sys.exit(0)
sys.exit(1)
PY
sleep 0.1; done; fail "no fake event: $1"; }
push() { curl -s -X POST "$B/$1" -H 'Content-Type: application/json' -d "$2" >/dev/null; }
waitlog 'e.get("path","").startswith("/api/v1/events")'
push __signal_push '{"envelope":{"sourceNumber":"+15559999","dataMessage":{"message":"see pic","attachments":[{"id":"att-1","contentType":"image/png","filename":"x.png","size":7}]}}}'
waitlog 'e.get("path")=="/api/v1/rpc" and e["body"]["method"]=="getAttachment" and e["body"]["params"]["id"]=="att-1"'
waitlog 'e.get("path")=="/api/v1/rpc" and e["body"]["method"]=="send" and e["body"]["params"].get("recipient")==["+15559999"]'
N=$(python3 -c 'import json;print(sum(1 for l in open("'"$T/f.log"'") if json.loads(l).get("body",{}).get("method")=="send"))')
push __signal_push '{"envelope":{"sourceNumber":"+15559999","dataMessage":{"reaction":{"emoji":"🔥","targetSentTimestamp":99}}}}'
intr(){ for i in $(seq 1 80); do grep -rqF -- "$1" "$T/s" 2>/dev/null && return 0; sleep 0.1; done; fail "transcript lacks $1"; }
intr '[reaction 🔥 on 99]'
sleep 0.3
N2=$(python3 -c 'import json;print(sum(1 for l in open("'"$T/f.log"'") if json.loads(l).get("body",{}).get("method")=="send"))')
[[ "$N2" == "$N" ]] || fail "reaction triggered a reply ($N -> $N2)"
rpc tools.invoke '{"name":"message","args":{"action":"react","channel":"signal","to":"+15559999","emoji":"👍","targetTimestamp":99}}' | grep -q '"ok": true' || fail "react rpc"
waitlog 'e.get("path")=="/api/v1/rpc" and e["body"]["method"]=="sendReaction" and e["body"]["params"]["emoji"]=="👍" and e["body"]["params"]["targetTimestamp"]==99'
rpc send '{"channel":"signal","to":"+15559999","message":"pic","attachments":[{"base64":"UE5HREFUQQ==","mime":"image/png"}]}' >/dev/null
waitlog 'e.get("path")=="/api/v1/rpc" and e["body"]["method"]=="send" and any(isinstance(a,str) and a.startswith("data:image/png;base64,") for a in (e["body"]["params"].get("attachments") or []))'
find "$T/s/media/signal" -type f 2>/dev/null | grep -q . || fail "no inbound media file"
echo "[e2e-signal-rich] OK"
