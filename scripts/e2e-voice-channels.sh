#!/usr/bin/env bash
# Voice replies beyond Telegram via channels.voice.send: streamed TTS (fake ElevenLabs, real WAV) → ffmpeg Ogg/Opus →
# Discord voice message (flags 8192 + duration + waveform), Slack files.getUploadURLExternal/upload/complete,
# Feishu im/v1/files (opus) + msg_type=audio, WhatsApp Cloud /media + type=audio voice. Fake servers only.
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
command -v ffmpeg >/dev/null || { echo "[e2e-voice-channels] SKIP: ffmpeg not installed (needed for Ogg/Opus voice notes)"; exit 0; }
T=$(mktemp -d)
python3 "$ROOT/scripts/fake-voice-channels.py" "$T/rep.json" > "$T/fp" 2>"$T/fake.log" & FP=$!
GW=
trap 'kill $FP ${GW:-} 2>/dev/null || true; rm -rf "$T"; _reap' EXIT
for _ in $(seq 50); do [[ -s "$T/fp" ]] && break; sleep 0.1; done
B="http://127.0.0.1:$(cat "$T/fp")"
fail() { echo "[e2e-voice-channels] FAIL: $*"; cat "$T/fake.log"; tail -5 "$T/gw.log"; [[ -f "$T/rep.json" ]] && cat "$T/rep.json"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
export OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_STATE_DIR="$T/state" OPENCLAW_DATA_DIR="$T/data"
unset OPENCLAW_GATEWAY_TOKEN
echo '{}' > "$OPENCLAW_CONFIG_PATH"
export OPENCLAW_TTS_PROVIDER=elevenlabs OPENCLAW_TTS_URL="$B" OPENCLAW_TTS_API_KEY=k
export DISCORD_BOT_TOKEN=DTOK DISCORD_API_BASE="$B/discord" DISCORD_GATEWAY_URL="ws://127.0.0.1:1/"
export SLACK_BOT_TOKEN=xoxb-S SLACK_API_BASE="$B/slack"
export FEISHU_APP_ID=cli_v FEISHU_APP_SECRET=sec FEISHU_API_BASE="$B"
export WHATSAPP_TOKEN=WAT WHATSAPP_PHONE_NUMBER_ID=PH1 WHATSAPP_API_BASE="$B"
"$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 80); do curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break; sleep 0.1; done
rpc() { curl -s -X POST "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
for spec in discord:C1 slack:CS1 feishu:oc_7 whatsapp:15550001; do
  ch=${spec%%:*}; to=${spec#*:}
  R=$(rpc "{\"method\":\"channels.voice.send\",\"params\":{\"channel\":\"$ch\",\"to\":\"$to\",\"text\":\"hello voice\"}}")
  grep -Eq '"delivered": *true' <<<"$R" || fail "$ch not delivered: $R"
done
python3 - "$T/rep.json" <<'PY' || fail "assertions"
import json, sys
r = json.load(open(sys.argv[1]))
d = r["discord"][0]
assert d["auth"] == "Bot DTOK" and d["payload"]["flags"] == 8192 and d["ogg"] and d["file"] == "voice-message.ogg", d
a = d["payload"]["attachments"][0]; assert 0.5 < a["duration_secs"] < 2 and a["waveform"], a
assert r["slack_get"][0]["auth"] == "Bearer xoxb-S" and "filename=reply.ogg" in r["slack_get"][0]["form"], r["slack_get"]
assert r["slack_upload"][0]["ogg"] and r["slack_complete"][0]["body"]["channel_id"] == "CS1" and r["slack_complete"][0]["body"]["files"][0]["id"] == "F1", r
f = r["feishu_file"][0]; assert f["auth"] == "Bearer t-9" and f["file_type"] == "opus" and f["ogg"] and 500 <= int(f["duration"]) <= 2000, f
m = r["feishu_msg"][0]; assert m["body"]["msg_type"] == "audio" and json.loads(m["body"]["content"])["file_key"] == "file_v1" and m["body"]["receive_id"] == "oc_7", m
w = r["wa_media"][0]; assert w["auth"] == "Bearer WAT" and w["type"] == "audio/ogg" and w["ogg"], w
wm = r["wa_msg"][0]["body"]; assert wm["type"] == "audio" and wm["audio"] == {"id": "MED9", "voice": True} and wm["to"] == "15550001", wm
PY
echo "[e2e-voice-channels] PASS"
