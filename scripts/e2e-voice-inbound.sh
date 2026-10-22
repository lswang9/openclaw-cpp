#!/usr/bin/env bash
# Inbound voice notes are auto-transcribed (Telegram voice, WhatsApp Cloud audio, Discord audio attachment) before
# the agent sees them; streaming TTS delivers ElevenLabs /stream chunks (CLI + tts.stream RPC). Fake servers only.
set -euo pipefail
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); PORT=$((20000 + RANDOM % 10000))
export OPENCLAW_STATE_DIR="$T/state" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/openclaw.json" HOME="$T" OPENCLAW_SESSION_DIR="$T/sess"
unset OPENCLAW_GATEWAY_TOKEN
FAKE_VOICE_LOG="$T/voice.log" python3 "$ROOT/scripts/fake-voice-media.py" > "$T/fp" 2>"$T/fake.log" & FPID=$!
for _ in $(seq 50); do [[ -s "$T/fp" ]] && break; sleep 0.1; done
FP=$(cat "$T/fp"); B="http://127.0.0.1:$FP"
export OPENCLAW_STT_PROVIDER=openai OPENCLAW_STT_URL="$B/v1/audio/transcriptions" OPENCLAW_STT_API_KEY=k
export OPENCLAW_DEFAULT_PROVIDER=mock  # offline: transcripts are asserted, not model output
unset DEEPSEEK_API_KEY
export TELEGRAM_API_BASE="$B" TELEGRAM_BOT_TOKEN=TOK WHATSAPP_API_BASE="$B" WHATSAPP_TOKEN=WATOK OPENCLAW_MEDIA_ALLOW_PRIVATE=1
export TELEGRAM_WEBHOOK_SECRET=S3cret OPENCLAW_TTS_AUTO=inbound
export OPENCLAW_TTS_PROVIDER=elevenlabs OPENCLAW_TTS_URL="$B" OPENCLAW_TTS_API_KEY=k
GW=
cleanup() { [[ -n "$GW" ]] && kill $GW 2>/dev/null; kill $FPID 2>/dev/null; true; }
trap 'cleanup; _reap' EXIT
fail() { echo "[e2e-voice-inbound] FAIL: $*"; tail -20 "$T/gw.log" 2>/dev/null; exit 1; }
# streaming TTS via CLI
R=$("$BIN" voice tts --stream --out "$T/s.mp3" hello stream) || fail "cli stream: $R"
printf '%s' "$R" | grep -Eq '"chunks_written": *[2-9]' || fail "expected >=2 chunks: $R"
grep -q ID3chunk2 "$T/s.mp3" || fail "stream file content"
"$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 80); do curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break; sleep 0.1; done
U="http://127.0.0.1:$PORT"
rpc() { curl -s -X POST "$U/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
rpc '{"method":"tts.stream","params":{"text":"hi"}}' | grep -Eq '"chunks": *[2-9]' || fail "tts.stream rpc"
post() { curl -s -m 30 -X POST "$U/api/channels/$1/inbound" -H 'Content-Type: application/json' -H 'X-Telegram-Bot-Api-Secret-Token: S3cret' -d "$2"; }
C=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$U/api/channels/telegram/inbound" -H 'Content-Type: application/json' -H 'X-Telegram-Bot-Api-Secret-Token: wrong' -d '{"message":{"chat":{"id":1},"text":"x"}}')
[[ "$C" == 401 ]] || fail "telegram bad secret accepted ($C)"
post telegram '{"message":{"chat":{"id":42,"type":"private"},"from":{"id":42},"voice":{"file_id":"F1","mime_type":"audio/ogg"}}}' >/dev/null
post whatsapp '{"entry":[{"changes":[{"value":{"messages":[{"from":"15550001","type":"audio","audio":{"id":"MEDIA1","mime_type":"audio/ogg"}}]}}]}]}' >/dev/null
post discord "{\"channel_id\":\"c9\",\"author\":{\"id\":\"u9\"},\"content\":\"\",\"attachments\":[{\"url\":\"$B/cdn/a.ogg\",\"content_type\":\"audio/ogg\",\"filename\":\"voice-message.ogg\"}]}" >/dev/null
ALL=""
for k in $(rpc '{"method":"sessions.list"}' | python3 -c 'import json,sys;print(" ".join(json.load(sys.stdin)["payload"]["sessions"]))'); do
  ALL+=$(rpc "{\"method\":\"chat.history\",\"params\":{\"key\":\"$k\"}}")
done
for tag in tg wa dc; do printf '%s' "$ALL" | grep -q "\[Audio\] Transcript: hello from voice $tag" || fail "transcript $tag missing"; done
grep -q "/botTOK/sendAudio ok" "$T/voice.log" 2>/dev/null || fail "telegram voice reply (streamed TTS) not uploaded"
echo "[e2e-voice-inbound] PASS"
