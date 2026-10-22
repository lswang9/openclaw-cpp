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
echo "[e2e-telegram-actions] OK"
