#!/usr/bin/env bash
# 1.129: Signal / Matrix / MS Teams / Google Chat / Mattermost inbound → agent → live outbound against an offline fake.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""
trap 'for p in $GW $FK; do kill $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-ext-channels] FAIL: $*"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; [[ -f "$T/fk.log" ]] && tail -4 "$T/fk.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN
FP=$(freeport); python3 "$ROOT/scripts/fake_chatops.py" "$FP" "$T/fk.log" & FK=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$FP/" >/dev/null 2>&1 && break; sleep 0.1; done
F="http://127.0.0.1:$FP"
echo '[{"content":"sig-dm"},{"content":"sig-group"},{"content":"mx-room"},{"content":"teams-reply"},{"content":"gchat-sync"},{"content":"mm-reply"},{"content":"gchat-async"}]' > "$T/script.json"
cat > "$T/cfg.json" <<JSON
{"channels":{
 "signal":{"apiBase":"$F","number":"+15550000","dmPolicy":"open","groupPolicy":"open"},
 "matrix":{"homeserver":"$F","accessToken":"mx-tok","userId":"@bot:x","dmPolicy":"open","groupPolicy":"open"},
 "msteams":{"appId":"app-1","appPassword":"pw-1","verifyJwt":false,"tokenUrl":"$F/botframework.com/oauth2/v2.0/token","dmPolicy":"open","groupPolicy":"open"},
 "googlechat":{"dmPolicy":"open","groupPolicy":"open"},
 "mattermost":{"baseUrl":"$F","botToken":"mm-tok","webhookToken":"hook-1","dmPolicy":"open","groupPolicy":"open"}}}
JSON
PORT=$(freeport)
start() { OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" \
  OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!
  for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done; }
start
inb() { curl -s -m 30 "http://127.0.0.1:$PORT/api/channels/$1/inbound" -H 'Content-Type: application/json' -d "$2"; }
has() { python3 - "$T/fk.log" "$1" <<'PY' || fail "fake log has no request matching: $1"
import json,sys
for l in open(sys.argv[1]):
    e=json.loads(l)
    if eval(sys.argv[2]): sys.exit(0)
sys.exit(1)
PY
}
# Signal DM + group
inb signal '{"envelope":{"sourceNumber":"+15551111","dataMessage":{"message":"hi"}}}' >/dev/null
has 'e["path"]=="/v2/send" and e["body"]["recipients"]==["+15551111"] and e["body"]["message"]=="sig-dm" and e["body"]["number"]=="+15550000"'
inb signal '{"envelope":{"sourceNumber":"+15552222","dataMessage":{"message":"yo","groupInfo":{"groupId":"R0lE"}}}}' >/dev/null
has 'e["path"]=="/v2/send" and e["body"]["recipients"]==["group.R0lE"] and e["body"]["message"]=="sig-group"'
inb signal '{"envelope":{"sourceNumber":"+15550000","dataMessage":{"message":"echo"}}}' | grep -q '"own message"' || fail "signal ignores own messages"
# Matrix app-service transaction (thread reply)
inb matrix '{"events":[{"type":"m.room.message","room_id":"!r1:x","sender":"@ann:x","content":{"msgtype":"m.text","body":"hello","m.relates_to":{"rel_type":"m.thread","event_id":"$root"}}}]}' >/dev/null
has 'e["method"]=="PUT" and "/rooms/%21r1%3Ax/send/m.room.message/" in e["path"] and e["auth"]=="Bearer mx-tok" and e["body"]["body"]=="mx-room" and e["body"]["m.relates_to"]["event_id"]=="$root"'
inb matrix '{"type":"m.room.message","room_id":"!r1:x","sender":"@bot:x","content":{"body":"self"}}' | grep -q '"own message"' || fail "matrix ignores own messages"
# Teams activity (token fetched with client credentials, reply into the conversation)
inb msteams '{"type":"message","id":"act-9","text":"<at>Bot</at> question","from":{"id":"29:u"},"conversation":{"id":"19:c@thread","conversationType":"channel"},"serviceUrl":"'"$F"'/teams/"}' >/dev/null
has 'e["path"].endswith("/oauth2/v2.0/token") and e["body"]["grant_type"]=="client_credentials" and e["body"]["scope"]=="https://api.botframework.com/.default"'
has 'e["path"]=="/teams/v3/conversations/19%3Ac%40thread/activities/act-9" and e["auth"]=="Bearer teams-tok" and e["body"]["text"]=="teams-reply" and e["body"]["replyToId"]=="act-9"'
# Google Chat: no API token → synchronous reply in the HTTP response
R=$(inb googlechat '{"type":"MESSAGE","message":{"text":"@bot hi","argumentText":" hi","sender":{"name":"users/1"},"thread":{"name":"spaces/S/threads/T"}},"space":{"name":"spaces/S","type":"ROOM"}}')
echo "$R" | python3 -c 'import json,sys;r=json.load(sys.stdin);assert r=={"text":"gchat-sync","thread":{"name":"spaces/S/threads/T"}},r' || fail "googlechat sync reply: $R"
[[ "$(inb googlechat '{"type":"ADDED_TO_SPACE","space":{"name":"spaces/S"}}')" == "{}" ]] || fail "googlechat non-message → {}"
# Mattermost outgoing webhook (token-verified)
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/channels/mattermost/inbound" -H 'Content-Type: application/json' -d '{"token":"bad","channel_id":"ch1","text":"x"}')" == 401 ]] || fail "mattermost bad token → 401"
curl -s "http://127.0.0.1:$PORT/api/channels/mattermost/inbound" -H 'Content-Type: application/x-www-form-urlencoded' --data 'token=hook-1&channel_id=ch1&user_id=u1&user_name=ann&text=claw%20help&trigger_word=claw&post_id=p9' >/dev/null
has 'e["path"]=="/api/v4/posts" and e["auth"]=="Bearer mm-tok" and e["body"]=={"channel_id":"ch1","message":"mm-reply","root_id":"p9"}'
# conversations_send works over the new channels (conversationRef recorded from inbound)
REF=$(curl -s "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"conversations.list","params":{"channel":"mattermost"}}' | python3 -c 'import json,sys;d=json.load(sys.stdin);d=d.get("result",d);d=d.get("payload",d);print(d["conversations"][0]["conversationRef"])')
curl -s "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"conversations.send","params":{"conversationRef":"'"$REF"'","message":"proactive"}}' >/dev/null
has 'e["path"]=="/api/v4/posts" and e["body"]["message"]=="proactive" and e["body"]["channel_id"]=="ch1"'
# Google Chat async with an access token
python3 - "$T/cfg.json" "$F" <<'PY'
import json,sys; c=json.load(open(sys.argv[1])); c["channels"]["googlechat"].update({"accessToken":"gc-tok","apiBase":sys.argv[2]}); json.dump(c,open(sys.argv[1],"w"))
PY
sleep 1.1
R=$(inb googlechat '{"type":"MESSAGE","message":{"text":"again","sender":{"name":"users/1"}},"space":{"name":"spaces/D","type":"DM"}}')
has 'e["path"]=="/v1/spaces/D/messages" and e["auth"]=="Bearer gc-tok" and e["body"]["text"]=="gchat-async"'
[[ "$R" == "{}" ]] || fail "googlechat async → empty sync body: $R"
echo "[e2e-ext-channels] OK"
