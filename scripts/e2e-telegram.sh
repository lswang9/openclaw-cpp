#!/usr/bin/env bash
# 1.126: Telegram live Bot API against an offline fake — deleteWebhook + getUpdates long polling, offset persistence,
# DM reply, group reply_parameters, forum topic message_thread_id, 4096-char chunking.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FT=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FT" ]] && kill $FT 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-telegram] FAIL: $*"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN TELEGRAM_BOT_TOKEN TELEGRAM_API_BASE TELEGRAM_MODE TELEGRAM_WEBHOOK_URL
FP=$(freeport); python3 "$ROOT/scripts/fake_telegram.py" "$FP" "$T/tg.log" & FT=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$FP/" >/dev/null 2>&1 && break; sleep 0.1; done
LONG=$(python3 -c 'print(("word " * 1200).strip())')
python3 - "$T/script.json" "$LONG" <<'PY'
import json,sys; json.dump([{"content":"hello dm"},{"content":"hello group"},{"content":"hello topic"},{"content":sys.argv[2]}],open(sys.argv[1],"w"))
PY
echo "{\"channels\":{\"telegram\":{\"botToken\":\"TT-1:abc\",\"apiBase\":\"http://127.0.0.1:$FP\",\"dmPolicy\":\"open\",\"groupPolicy\":\"open\"}}}" > "$T/cfg.json"
PORT=$(freeport)
start() { OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_TELEGRAM_POLL_TIMEOUT=1 OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!
  for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done; }
start
push() { curl -s -X POST "http://127.0.0.1:$FP/__push" -d "$1" >/dev/null; }
waitlog() { for i in $(seq 1 100); do python3 - "$T/tg.log" "$1" <<'PY' && return 0
import json,sys
for l in open(sys.argv[1]):
    e=json.loads(l)
    if e["method"]=="sendMessage" and eval(sys.argv[2]): sys.exit(0)
sys.exit(1)
PY
sleep 0.1; done; fail "no sendMessage matching $1: $(grep sendMessage $T/tg.log | tail -3)"; }
for i in $(seq 1 50); do grep -q getUpdates "$T/tg.log" 2>/dev/null && break; sleep 0.1; done
grep -q '"deleteWebhook"' "$T/tg.log" || fail "deleteWebhook first"
push '{"message":{"message_id":1,"chat":{"id":555,"type":"private"},"from":{"id":555,"first_name":"A"},"text":"hi bot"}}'
waitlog 'e["args"]["chat_id"]=="555" and e["args"]["text"]=="hello dm"'
push '{"message":{"message_id":7,"chat":{"id":-100,"type":"supergroup"},"from":{"id":9},"text":"hey group"}}'
waitlog 'e["args"]["chat_id"]=="-100" and e["args"].get("reply_parameters",{}).get("message_id")==7'
push '{"message":{"message_id":8,"chat":{"id":-200,"type":"supergroup","is_forum":true},"from":{"id":9},"is_topic_message":true,"message_thread_id":42,"text":"topic msg"}}'
waitlog 'e["args"]["chat_id"]=="-200" and e["args"].get("message_thread_id")==42'
push '{"message":{"message_id":2,"chat":{"id":556,"type":"private"},"from":{"id":556},"text":"long please"}}'
waitlog 'e["args"]["chat_id"]=="556" and len(e["args"]["text"])<=4096'
N=$(grep -c '"chat_id": "556"' "$T/tg.log"); [[ $N -ge 2 ]] || fail "long reply should be chunked (got $N parts)"
python3 -c "import json;assert json.load(open('$T/d/telegram/update-offset.json'))['offset']==5" || fail "offset persisted: $(cat $T/d/telegram/update-offset.json)"
kill $GW; wait $GW 2>/dev/null || true; GW=""; : > "$T/tg.log"; start
for i in $(seq 1 50); do grep -q getUpdates "$T/tg.log" 2>/dev/null && break; sleep 0.1; done
grep -q '"offset": "5"\|"offset": 5' "$T/tg.log" || fail "restart resumes at offset 5: $(head -3 $T/tg.log)"
grep -q "telegram: long polling" "$T/gw.log" || fail "poll log"
echo "[e2e-telegram] OK"
