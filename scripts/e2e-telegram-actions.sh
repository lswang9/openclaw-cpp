#!/usr/bin/env bash
# 1.140: Telegram reactions / edits / deletes / pins / setMyCommands / inbound reactions+edits, offline fake.
# (derived from 1.135 e2e-telegram-actions:) Telegram media + inline keyboards against the offline fake: inbound photo → getFile download → image input
# to the model; inbound document saved and named; outbound buttons (reply_markup) via the message tool; callback_query
# presses become messages (answerCallbackQuery) and answer ask_user; MEDIA: directives / media param → sendPhoto /
# sendDocument multipart uploads.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FT=""; AQ=""
trap '[[ -n "$AQ" ]] && kill $AQ 2>/dev/null; [[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FT" ]] && kill $FT 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-telegram-actions] FAIL: $*"; echo "T=$T"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_DOCS_URL TELEGRAM_BOT_TOKEN TELEGRAM_API_BASE TELEGRAM_MODE TELEGRAM_WEBHOOK_URL OPENCLAW_MOCK_SCRIPT
FP=$(freeport); python3 "$ROOT/scripts/fake_telegram.py" "$FP" "$T/tg.log" & FT=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$FP/" >/dev/null 2>&1 && break; sleep 0.1; done
python3 - "$T/img.png" <<'PY'
import struct,zlib,sys
def ch(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
raw=b"".join(b"\0"+b"\xff\0\0"*3 for _ in range(2))
open(sys.argv[1],"wb").write(b"\x89PNG\r\n\x1a\n"+ch(b"IHDR",struct.pack(">IIBBBBB",3,2,8,2,0,0,0))+ch(b"IDAT",zlib.compress(raw))+ch(b"IEND",b""))
PY
printf 'quarterly numbers\n' > "$T/report.txt"
curl -s -X POST "http://127.0.0.1:$FP/__file?id=PHOTO1" --data-binary @"$T/img.png" >/dev/null
curl -s -X POST "http://127.0.0.1:$FP/__file?id=DOC1" --data-binary @"$T/report.txt" >/dev/null
echo "{\"channels\":{\"telegram\":{\"botToken\":\"TT-1:abc\",\"apiBase\":\"http://127.0.0.1:$FP\",\"dmPolicy\":\"open\",\"groupPolicy\":\"open\"}}}" > "$T/cfg.json"
PORT=$(freeport)
OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_TELEGRAM_POLL_TIMEOUT=1 OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
push() { curl -s -X POST "http://127.0.0.1:$FP/__push" -d "$1" >/dev/null; }
rpc() { curl -s -m 60 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
waitlog() { for i in $(seq 1 100); do python3 - "$T/tg.log" "$1" <<'PY' && return 0
import json,sys
for l in open(sys.argv[1]):
    e=json.loads(l); m=e["method"]; a=e["args"]
    if eval(sys.argv[2]): sys.exit(0)
sys.exit(1)
PY
sleep 0.1; done; fail "no Bot API call matching $1: $(tail -3 $T/tg.log)"; }
for i in $(seq 1 50); do grep -q getUpdates "$T/tg.log" 2>/dev/null && break; sleep 0.1; done
waitlog 'm=="setMyCommands" and [c["command"] for c in a["commands"]][:2]==["new","reset"]'
push '{"message":{"message_id":1,"chat":{"id":777,"type":"private"},"from":{"id":777,"first_name":"A"},"text":"hi"}}'
waitlog 'm=="sendMessage" and a["chat_id"]=="777" and a["text"]=="mock-ok"'
tl() { rpc tools.invoke "{\"name\":\"message\",\"args\":$1}" ; }
tl '{"action":"react","channel":"telegram","to":"777","messageId":"1","emoji":"👍"}' | grep -q '"ok": true' || fail "react rpc"
waitlog 'm=="setMessageReaction" and a["chat_id"]=="777" and a["message_id"]==1 and a["reaction"]==[{"type":"emoji","emoji":"👍"}]'
tl '{"action":"react","channel":"telegram","to":"777","messageId":1,"remove":true}' >/dev/null
waitlog 'm=="setMessageReaction" and a["reaction"]==[]'
tl '{"action":"edit","channel":"telegram","to":"777","messageId":100,"message":"edited text"}' >/dev/null
waitlog 'm=="editMessageText" and a["message_id"]==100 and a["text"]=="edited text"'
tl '{"action":"edit","channel":"telegram","to":"777","messageId":100,"buttons":[["A"]]}' >/dev/null
waitlog 'm=="editMessageReplyMarkup" and a["reply_markup"]["inline_keyboard"][0][0]["callback_data"]=="A"'
tl '{"action":"pin","channel":"telegram","to":"777","messageId":100}' >/dev/null
waitlog 'm=="pinChatMessage" and a["message_id"]==100'
tl '{"action":"delete","channel":"telegram","to":"777","messageId":100}' >/dev/null
waitlog 'm=="deleteMessage" and a["message_id"]==100'
tl '{"action":"react","channel":"telegram","to":"777"}' | grep -q 'messageId is required' || fail "missing messageId validation"
# inbound reaction → transcript note, no agent turn
N=$(grep -c '"sendMessage"' "$T/tg.log")
push '{"message_reaction":{"chat":{"id":777,"type":"private"},"message_id":100,"user":{"id":777},"date":1,"old_reaction":[],"new_reaction":[{"type":"emoji","emoji":"🔥"}]}}'
intr() { for i in $(seq 1 100); do grep -rqF -- "$1" "$T/s" 2>/dev/null && return 0; sleep 0.1; done; fail "transcript lacks $1"; }
intr '[telegram event] [reaction 🔥 on message 100]'
sleep 0.5; [[ $(grep -c '"sendMessage"' "$T/tg.log") == "$N" ]] || fail "reaction triggered a reply"
# inbound edit is marked
push '{"edited_message":{"message_id":1,"chat":{"id":777,"type":"private"},"from":{"id":777},"text":"hi again"}}'
intr '[edited message 1] hi again'
waitlog 'm=="getUpdates" and "message_reaction" in a.get("allowed_updates","")'

# 1.146: sendPoll + poll_answer rejoin; forum topic session + create/edit/close
tl '{"action":"poll","channel":"telegram","to":"777","question":"Coffee?","options":["Yes","No"],"pollPublic":true}' | grep -q '"ok": true' || fail "poll rpc"
waitlog 'm=="sendPoll" and a["question"]=="Coffee?" and a["options"]==["Yes","No"] and a.get("is_anonymous") is False'
POLL=poll-777-2
push "{\"poll_answer\":{\"poll_id\":\"$POLL\",\"user\":{\"id\":777},\"option_ids\":[0]}}"
intr '[poll answer] Coffee? → Yes'
push '{"message":{"message_id":50,"is_topic_message":true,"message_thread_id":42,"chat":{"id":-1001,"type":"supergroup","is_forum":true,"title":"Forum"},"from":{"id":9},"text":"topic hi"}}'
waitlog 'm=="sendMessage" and str(a.get("chat_id")) in ("-1001","\"-1001\"") and a.get("message_thread_id") in (42,"42")'
python3 - "$T/s" <<'PY' || fail "no topic session file"
import os,sys
root=sys.argv[1]; found=False
for dp,_,fs in os.walk(root):
    blob=(dp+" "+" ".join(fs)).lower()
    if "topic" in blob: found=True
raise SystemExit(0 if found else 1)
PY
tl '{"action":"create_topic","channel":"telegram","to":"-1001","name":"Ideas"}' | grep -q '"ok": true' || fail "create_topic"
waitlog 'm=="createForumTopic" and a["name"]=="Ideas"'
tl '{"action":"edit_topic","channel":"telegram","to":"-1001","threadId":42,"name":"Ideas v2"}' >/dev/null
waitlog 'm=="editForumTopic" and a["message_thread_id"]==42 and a["name"]=="Ideas v2"'
tl '{"action":"close_topic","channel":"telegram","to":"-1001","threadId":42}' >/dev/null
waitlog 'm=="closeForumTopic" and a["message_thread_id"]==42'
waitlog 'm=="getUpdates" and "poll_answer" in a.get("allowed_updates","")'
# 1.157: anonymous poll results (closed `poll` update), public poll close retires the route, payments, business messages
tl '{"action":"poll","channel":"telegram","to":"777","question":"Lunch?","options":["Pizza","Sushi","Salad"],"anonymous":true}' | grep -q '"ok": true' || fail "anon poll rpc"
waitlog 'm=="sendPoll" and a["question"]=="Lunch?" and a.get("is_anonymous") is True'
push '{"poll":{"id":"poll-777-3","question":"Lunch?","is_closed":false,"total_voter_count":1,"options":[{"text":"Pizza","voter_count":1},{"text":"Sushi","voter_count":0},{"text":"Salad","voter_count":0}]}}'
push '{"poll":{"id":"poll-777-3","question":"Lunch?","is_closed":true,"total_voter_count":3,"options":[{"text":"Pizza","voter_count":2},{"text":"Sushi","voter_count":1},{"text":"Salad","voter_count":0}]}}'
intr '[poll closed, anonymous] Lunch? → Pizza: 2, Sushi: 1, Salad: 0 (3 voters)'
push '{"poll":{"id":"poll-777-2","question":"Coffee?","is_closed":true,"total_voter_count":1,"options":[{"text":"Yes","voter_count":1},{"text":"No","voter_count":0}]}}'
intr '[poll closed] Coffee? → Yes: 1, No: 0 (1 voters)'
N=$(grep -c '"sendMessage"' "$T/tg.log")
push '{"poll_answer":{"poll_id":"poll-777-2","user":{"id":777},"option_ids":[1]}}'
sleep 0.6; grep -rqF 'Coffee? → No' "$T/s" && fail "retired poll still routed"
tl '{"action":"invoice","channel":"telegram","to":"777","title":"Pro","description":"Pro plan","payload":"pro-1","currency":"XTR","amount":50}' | grep -q '"ok": true' || fail "invoice rpc"
waitlog 'm=="sendInvoice" and a["payload"]=="pro-1" and a["prices"]==[{"label":"Total","amount":50}] and "provider_token" not in a'
push '{"pre_checkout_query":{"id":"pcq1","from":{"id":777},"currency":"XTR","total_amount":50,"invoice_payload":"pro-1"}}'
waitlog 'm=="answerPreCheckoutQuery" and a["pre_checkout_query_id"]=="pcq1" and a["ok"] is False'
push '{"message":{"message_id":60,"chat":{"id":777,"type":"private"},"from":{"id":777},"successful_payment":{"currency":"XTR","total_amount":5000,"invoice_payload":"pro-1","telegram_payment_charge_id":"ch_1"}}}'
intr '[payment received] 50.00 XTR payload=pro-1 charge=ch_1'
sleep 0.4; [[ $(grep -c '"sendMessage"' "$T/tg.log") == "$N" ]] || fail "payment/poll note triggered a reply"
push '{"business_message":{"message_id":70,"business_connection_id":"bc-1","chat":{"id":555,"type":"private"},"from":{"id":555},"text":"biz hi"}}'
waitlog 'm=="sendMessage" and str(a.get("chat_id"))=="555" and a.get("business_connection_id")=="bc-1"'
waitlog 'm=="getUpdates" and "business_message" in a.get("allowed_updates","") and "pre_checkout_query" in a.get("allowed_updates","")'
# 1.166: business callback edit/delete + topic icon cache
push '{"callback_query":{"id":"cb-biz","from":{"id":555},"data":"ok","message":{"message_id":80,"business_connection_id":"bc-2","chat":{"id":555,"type":"private"},"text":"btn"}}}'
waitlog 'm=="answerCallbackQuery" and a["callback_query_id"]=="cb-biz"'
tl '{"action":"edit","channel":"telegram","to":"555","messageId":80,"message":"edited biz"}' >/dev/null
waitlog 'm=="editMessageText" and a.get("business_connection_id")=="bc-2" and a["text"]=="edited biz"'
tl '{"action":"delete","channel":"telegram","to":"555","messageId":80}' >/dev/null
waitlog 'm=="deleteBusinessMessages" and a.get("business_connection_id")=="bc-2" and a["message_ids"]==[80]'
tl '{"action":"create_topic","channel":"telegram","to":"-1001","name":"Design","icon_color":7322096,"iconCustomEmojiId":"emoji-1"}' | grep -q '"ok": true' || fail "create_topic icon"
waitlog 'm=="createForumTopic" and a["name"]=="Design" and a.get("icon_custom_emoji_id")=="emoji-1"'
tl '{"action":"topic_icon","channel":"telegram","to":"-1001","threadId":42}' | python3 -c "import json,sys;r=json.load(sys.stdin);assert r.get('ok') or (r.get('result') or r).get('ok') or 'topicIcon' in json.dumps(r),r" || fail "topic_icon lookup"
# inbound forum_topic_created caches too
push '{"message":{"message_id":90,"message_thread_id":99,"chat":{"id":-1001,"type":"supergroup","is_forum":true},"forum_topic_created":{"name":"Ops","icon_color":16766590,"icon_custom_emoji_id":"emoji-2"}}}'
sleep 0.4
tl '{"action":"topic_icon","channel":"telegram","to":"-1001","threadId":99}' | grep -q 'Ops' || fail "inbound topic icon not cached"
echo "[e2e-telegram-actions] OK"
