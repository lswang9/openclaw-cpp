#!/usr/bin/env bash
# 1.135: Telegram media + inline keyboards against the offline fake: inbound photo → getFile download → image input
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
fail() { echo "[e2e-telegram-media] FAIL: $*"; echo "T=$T"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; exit 1; }
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
push '{"message":{"message_id":1,"chat":{"id":777,"type":"private"},"from":{"id":777,"first_name":"A"},"text":"hi"}}'
waitlog 'm=="sendMessage" and a["chat_id"]=="777" and a["text"]=="mock-ok"'
# photo → getFile + download → the model receives one image
push '{"message":{"message_id":2,"chat":{"id":777,"type":"private"},"from":{"id":777},"caption":"what is this","photo":[{"file_id":"SMALL","width":1,"height":1},{"file_id":"PHOTO1","width":3,"height":2,"file_size":70}]}}'
waitlog 'm=="getFile" and a["file_id"]=="PHOTO1"'
waitlog 'm=="sendMessage" and a["chat_id"]=="777" and a["text"].startswith("mock-saw-image:1") and "what is this" in a["text"]'
# document → saved under data/media/inbound and named in the text
push '{"message":{"message_id":3,"chat":{"id":777,"type":"private"},"from":{"id":777},"document":{"file_id":"DOC1","file_name":"report.txt","mime_type":"text/plain","file_size":18}}}'
waitlog 'm=="getFile" and a["file_id"]=="DOC1"'
intr() { for i in $(seq 1 100); do grep -rqF -- "$1" "$T/s" 2>/dev/null && return 0; sleep 0.1; done; fail "transcript lacks $1"; }
intr 'report.txt (text/plain, 18 bytes)]'
ls "$T"/d/media/inbound/*-report.txt >/dev/null 2>&1 || fail "document not saved"
# outbound buttons via the message tool
rpc tools.invoke '{"name":"message","args":{"channel":"telegram","to":"777","message":"Pick one","buttons":[["Red","Blue"],[{"text":"Docs","url":"https://docs.openclaw.ai"}]]}}' >/dev/null
waitlog 'm=="sendMessage" and a["text"]=="Pick one" and a["reply_markup"]["inline_keyboard"][0][1]=={"text":"Blue","callback_data":"Blue"} and a["reply_markup"]["inline_keyboard"][1][0]["url"].startswith("https://")'
# callback_query press → answerCallbackQuery + delivered as a user message
push '{"callback_query":{"id":"cb1","from":{"id":777},"data":"Red","message":{"message_id":50,"chat":{"id":777,"type":"private"},"text":"Pick one"}}}'
waitlog 'm=="answerCallbackQuery" and a["callback_query_id"]=="cb1"'
intr '[telegram/777] Red'
# ask_user mirrors options as buttons; a press answers it
SK=$(rpc tools.invoke '{"name":"conversations_list","args":{"channel":"telegram"}}' | python3 -c 'import json,sys;r=json.load(sys.stdin)["payload"]["result"];r=json.loads(r) if isinstance(r,str) else r;print(r["conversations"][0]["sessionKey"])')
rpc tools.invoke "{\"name\":\"ask_user\",\"args\":{\"question\":\"Colour?\",\"options\":[\"Green\",\"Purple\"],\"timeoutSeconds\":20},\"sessionKey\":\"$SK\"}" > "$T/ask.json" & AQ=$!
waitlog 'm=="sendMessage" and a["text"].startswith("Colour?") and a["reply_markup"]["inline_keyboard"]==[[{"text":"Green","callback_data":"Green"}],[{"text":"Purple","callback_data":"Purple"}]]'
push '{"callback_query":{"id":"cb2","from":{"id":777},"data":"Purple","message":{"message_id":51,"chat":{"id":777,"type":"private"},"text":"Colour?"}}}'
wait $AQ; AQ=""
grep -q 'Purple' "$T/ask.json" || fail "ask_user answer via button: $(head -c 300 $T/ask.json)"
# MEDIA directive + media param → multipart uploads
rpc send "{\"channel\":\"telegram\",\"to\":\"777\",\"message\":\"Here you go\\nMEDIA: $T/img.png\"}" >/dev/null
waitlog 'm=="sendPhoto" and a["chat_id"]=="777" and a["photo"]["filename"]=="img.png" and a["photo"]["mime"]=="image/png" and a["photo"]["sha_head"]=="iVBORw0KGgo="'
waitlog 'm=="sendMessage" and a["text"]=="Here you go"'
rpc send "{\"channel\":\"telegram\",\"to\":\"777\",\"media\":\"$T/report.txt\"}" >/dev/null
waitlog 'm=="sendDocument" and a["document"]["filename"]=="report.txt" and a["document"]["bytes"]==18'
rpc send '{"channel":"telegram","to":"777","media":"https://example.com/cat.jpg"}' >/dev/null
waitlog 'm=="sendPhoto" and a["photo"]=="https://example.com/cat.jpg"'
echo "[e2e-telegram-media] OK"
