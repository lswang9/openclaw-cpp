#!/usr/bin/env bash
# Discord voice offline: fake voice gateway (WS v8) + UDP; client does Identify/Ready/IP discovery/Select Protocol/
# Session Description/Speaking, sends Ogg-demuxed Opus as AES-GCM rtpsize RTP paced 20 ms, then 5 silence frames.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
python3 -c 'import cryptography' 2>/dev/null || { echo "[e2e-discord-voice] SKIP (python cryptography missing)"; exit 0; }
T=$(mktemp -d)
python3 "$ROOT/scripts/fake-discord-voice.py" "$T/report.json" > "$T/port" 2>"$T/fake.log" & FP=$!
trap 'kill $FP 2>/dev/null || true' EXIT
for _ in $(seq 50); do [[ -s "$T/port" ]] && break; sleep 0.1; done
fail() { echo "[e2e-discord-voice] FAIL: $*"; cat "$T/fake.log"; exit 1; }
R=$("$BIN" discord voice play --endpoint "ws://127.0.0.1:$(cat "$T/port")/?v=8" --server G1 --user U1 --session S1 --token VT --ogg "$ROOT/scripts/fixtures/sine-0.5s.ogg") || fail "play: $R"
for _ in $(seq 30); do [[ -s "$T/report.json" ]] && break; sleep 0.1; done
python3 - "$T/report.json" <<'PY' || fail "report check"
import json, sys
r = json.load(open(sys.argv[1]))
assert r["identify"]["token"] == "VT" and r["identify"]["server_id"] == "G1", r["identify"]
assert r["discovery"], "ip discovery"
assert r["select"]["data"]["mode"] == "aead_aes256_gcm_rtpsize", r["select"]
assert r["speaking"] == [1, 0], r["speaking"]
assert not r["errors"], r["errors"]
rtp = r["rtp"]; audio = [p for p in rtp if not p["silence"]]
assert len(audio) >= 20 and sum(p["silence"] for p in rtp) == 5, (len(audio), len(rtp))
for a, b in zip(rtp, rtp[1:]):
    assert (b["seq"] - a["seq"]) % 65536 == 1 and (b["ts"] - a["ts"]) % 2**32 == 960
assert r["heartbeats"] >= 1
print("frames", len(audio))
PY
printf '%s' "$R" | grep -Eq '"packets_sent": *[0-9]+' || fail "report"

# Part 2: gateway-driven: main gateway op 4 → VOICE_STATE_UPDATE/VOICE_SERVER_UPDATE → voice session; WAV input is
# Opus-encoded with libopus; the fake's second speaker (U2, RTP with header extension) is received + decoded.
T2=$(mktemp -d)
python3 "$ROOT/scripts/fake-discord-voice.py" "$T2/report.json" "$ROOT/scripts/fixtures/sine-0.5s.ogg" > "$T2/port" 2>"$T2/fake.log" & FP2=$!
trap 'kill $FP $FP2 ${GW:-} 2>/dev/null || true' EXIT
for _ in $(seq 50); do [[ -s "$T2/port" ]] && break; sleep 0.1; done
VP=$(cat "$T2/port")
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
python3 - "$T2/tone.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 48000))) for i in range(48000)))
PY
export OPENCLAW_SESSION_DIR="$T2/s"
DISCORD_BOT_TOKEN=BOT DISCORD_GATEWAY_URL="ws://127.0.0.1:$VP/gw" DISCORD_VOICE_INSECURE_WS=1 \
  "$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T2/gw.log" 2>&1 & GW=$!
for _ in $(seq 80); do curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break; sleep 0.1; done
rpc() { curl -s -m 60 -X POST "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
rpc '{"method":"discord.gateway.start"}' >/dev/null
for _ in $(seq 50); do rpc '{"method":"discord.voice.status"}' | grep -q '"bot_user_id": *"B1"' && break; sleep 0.1; done
rpc '{"method":"discord.voice.status"}' | grep -Eq '"opus": *true' || fail "built without libopus"
R2=$(rpc "{\"method\":\"discord.voice.play\",\"params\":{\"guild_id\":\"G1\",\"channel_id\":\"C1\",\"audio_path\":\"$T2/tone.wav\",\"listen_ms\":1200,\"receive_dir\":\"$T2/rx\"}}")
for _ in $(seq 30); do [[ -s "$T2/report.json" ]] && python3 -c 'import json,sys;r=json.load(open(sys.argv[1]));sys.exit(0 if len(r["op4"])>=2 else 1)' "$T2/report.json" && break; sleep 0.1; done
python3 - "$R2" "$T2/report.json" <<'PY' || { echo "$R2"; cat "$T2/fake.log"; tail -5 "$T2/gw.log"; fail "gateway voice"; }
import json, sys
rep = json.loads(sys.argv[1]); rep = rep.get("payload", rep)
r = json.load(open(sys.argv[2]))
assert rep["ok"], rep
assert rep["join"]["session_id"] == "VS1" and rep["join"]["token"] == "VT", rep["join"]
assert r["main_identify"] & 128, "GUILD_VOICE_STATES intent"
assert r["op4"][0]["channel_id"] == "C1" and r["op4"][-1]["channel_id"] is None, r["op4"]
assert r["identify"]["session_id"] == "VS1" and r["identify"]["user_id"] == "B1", r["identify"]
audio = [p for p in r["rtp"] if not p["silence"]]
assert 48 <= len(audio) <= 52, len(audio)   # 1 s of 20 ms frames
rx = rep["received"]; assert len(rx) == 1, rx
assert rx[0]["user_id"] == "U2" and rx[0]["packets"] == r["sent_other"] and rx[0]["samples"] >= 960 * r["sent_other"], rx
assert open(rx[0]["wav"], "rb").read(4) == b"RIFF"
print("encoded", len(audio), "received", rx[0]["packets"], "samples", rx[0]["samples"])
PY
echo "[e2e-discord-voice] PASS"
