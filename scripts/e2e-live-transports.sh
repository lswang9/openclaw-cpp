#!/usr/bin/env bash
# 1.143: live channel transports against offline fakes — Mattermost WebSocket event stream (no channel list; DMs),
# signal-cli JSON-RPC daemon (SSE events in, JSON-RPC send out), native IRC (NICK/USER/JOIN, PING/PONG, PRIVMSG in/out)
# and Twitch chat over IRC (PASS oauth:, channel replies).
set -eu
export OPENCLAW_DEFAULT_PROVIDER=mock
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
unset SIGNAL_API_BASE SIGNAL_NUMBER SIGNAL_HTTP_URL MATTERMOST_URL MATTERMOST_BOT_TOKEN IRC_SERVER TWITCH_MODE TWITCH_ACCESS_TOKEN OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN 2>/dev/null || true
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FK" ]] && kill $FK 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-live-transports] FAIL: $*"; echo "T=$T"; [[ -f "$T/gw.log" ]] && tail -15 "$T/gw.log"; [[ -f "$T/f.log" ]] && tail -6 "$T/f.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
HP=$(freeport); IP=$(freeport); python3 "$ROOT/scripts/fake_live_transports.py" "$HP" "$IP" "$T/f.log" & FK=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$HP/x" >/dev/null 2>&1 && break; sleep 0.1; done
B="http://127.0.0.1:$HP"
cat > "$T/cfg.json" <<J
{"channels": {
  "mattermost": {"baseUrl": "$B", "botToken": "mm-tok", "dmPolicy": "open", "groupPolicy": "open"},
  "signal": {"httpUrl": "$B", "number": "+15550001", "dmPolicy": "open"},
  "irc": {"server": "127.0.0.1:$IP", "tls": false, "nick": "clawbot", "sasl": {"username": "clawacct", "password": "sasl-pw"}, "channels": ["#lobby"], "dmPolicy": "open", "groupPolicy": "open"},
  "twitch": {"mode": "irc", "ircServer": "127.0.0.1:$IP", "tls": false, "accessToken": "tw-tok", "username": "clawtv", "channel": "streamer", "groupPolicy": "open"}}}
J
PORT=$(freeport)
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
waitlog() { for i in $(seq 1 150); do python3 - "$T/f.log" "$1" <<'PY' && return 0
import json,sys
for l in open(sys.argv[1]):
    e=json.loads(l)
    if eval(sys.argv[2]): sys.exit(0)
sys.exit(1)
PY
sleep 0.1; done; fail "no fake event matching: $1"; }
push() { curl -s -X POST "$B/$1" -H 'Content-Type: application/json' -d "$2" >/dev/null; }
# Mattermost WebSocket: auth challenge, then a DM "posted" event → reply via REST
waitlog 'e.get("ws_in") and json.loads(e["ws_in"])["action"]=="authentication_challenge" and json.loads(e["ws_in"])["data"]["token"]=="mm-tok"'
push __mm_push '{"channel_type":"D","post":{"id":"p1","channel_id":"dm-1","user_id":"u-alice","message":"hi over ws","type":""}}'
waitlog 'e.get("path")=="/api/v4/posts" and e["body"]["channel_id"]=="dm-1" and e["body"]["message"]=="mock-ok"'
push __mm_push '{"channel_type":"D","post":{"id":"p2","channel_id":"dm-1","user_id":"bot1","message":"own echo","type":""}}'
# signal-cli JSON-RPC daemon: SSE envelope in → JSON-RPC send out
waitlog 'e.get("path","").startswith("/api/v1/events")'
push __signal_push '{"envelope":{"sourceNumber":"+15559999","dataMessage":{"message":"hello signal"}}}'
waitlog 'e.get("path")=="/api/v1/rpc" and e["body"]["method"]=="send" and e["body"]["params"]["recipient"]==["+15559999"] and e["body"]["params"]["account"]=="+15550001"'
# native IRC: registration, JOIN, PONG, channel PRIVMSG → reply in channel; DM → reply to nick
waitlog 'e.get("irc")=="NICK clawbot"'
waitlog 'e.get("sasl")==["clawacct","clawacct","sasl-pw"] and e["ok"]'
waitlog 'e.get("irc")=="CAP END"'
waitlog 'e.get("irc")=="JOIN #lobby"'
waitlog 'e.get("irc")=="PONG :fake.irc"'
push __irc_push '{"line":":alice!a@h PRIVMSG #lobby :hello irc"}'
waitlog 'e.get("irc","").startswith("PRIVMSG #lobby :")'
push __irc_push '{"line":":bob!b@h PRIVMSG clawbot :private hi"}'
waitlog 'e.get("irc","").startswith("PRIVMSG bob :")'
# Twitch over IRC
waitlog 'e.get("irc")=="PASS oauth:tw-tok"'
waitlog 'e.get("irc")=="JOIN #streamer"'
push __irc_push '{"line":"@badges=;color= :viewer!v@v.tmi.twitch.tv PRIVMSG #streamer :hey stream"}'
waitlog 'e.get("irc","").startswith("PRIVMSG #streamer :")'
# own Mattermost echo produced no second reply
[[ $(grep -c '"/api/v4/posts"' "$T/f.log") -ge 1 ]] || fail "mm posts"
echo "[e2e-live-transports] OK"
