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
echo "[e2e-discord-voice] PASS"
