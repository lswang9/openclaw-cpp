#!/usr/bin/env bash
# 1.102: Discord DAVE E2EE offline. The fake voice server runs DAVE v1 with a real MLS external sender and a simulated
# peer (U2) on libdave (openclaw-dave-helper). The client advertises max_dave_protocol_version 1, sends its key
# package, commits the external Add proposal, processes its own commit (transition 1), signals ready, executes on op
# 22, then sends DAVE-encrypted Opus inside transport-encrypted RTP; the peer decrypts every frame. The peer's
# DAVE-encrypted audio is decrypted and decoded by the client. SKIPs (honestly) when built without libdave.
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
HELPER="$(dirname "$BIN")/openclaw-dave-helper"
[[ -x "$HELPER" ]] || { echo "[e2e-discord-dave] SKIP (built without libdave: run scripts/build-dave.sh and rebuild)"; exit 0; }
python3 -c 'import cryptography' 2>/dev/null || { echo "[e2e-discord-dave] SKIP (python cryptography missing)"; exit 0; }
T=$(mktemp -d); FP=""; trap '[[ -n "$FP" ]] && kill $FP 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-discord-dave] FAIL: $*"; cat "$T/fake.log"; exit 1; }
FAKE_DAVE=1 DAVE_HELPER="$HELPER" FAKE_DAVE_GROUP=1234567890 python3 "$ROOT/scripts/fake-discord-voice.py" "$T/report.json" "$ROOT/scripts/fixtures/sine-0.5s.ogg" >"$T/port" 2>"$T/fake.log" & FP=$!
for _ in $(seq 50); do [[ -s "$T/port" ]] && break; sleep 0.1; done
R=$("$BIN" discord voice play --endpoint "ws://127.0.0.1:$(cat "$T/port")/?v=8" --server G1 --user 1001 --session S1 --token VT \
     --channel 1234567890 --listen-ms 1500 --receive-dir "$T/rx" --ogg "$ROOT/scripts/fixtures/sine-0.5s.ogg") || fail "play: $R"
for _ in $(seq 40); do [[ -s "$T/report.json" ]] && python3 -c 'import json,sys;sys.exit(0 if json.load(open(sys.argv[1])).get("sent_other") else 1)' "$T/report.json" 2>/dev/null && break; sleep 0.1; done
python3 - "$R" "$T/report.json" <<'PY' || { echo "$R"; fail "report"; }
import json, sys
rep = json.loads(sys.argv[1]); r = json.load(open(sys.argv[2]))
assert r["identify"]["max_dave_protocol_version"] >= 1, r["identify"]
d = r["dave"]
assert d["key_package"] and d["commit_welcome"] and d["ready"] == [1], d
audio = [p for p in r["rtp"] if not p["silence"]]
assert len(audio) >= 20 and d["frames_ok"] == len(r["rtp"]) and d["frames_bad"] == 0 and d["plain_leak"] == 0, (len(audio), d)
assert rep["ok"] and rep["dave_protocol_version"] == 1 and rep["dave"]["ready"] and rep["dave"]["frames_encrypted"] >= len(audio), rep
rx = rep["received"]; assert len(rx) == 1 and rx[0]["user_id"] == "2002" and rx[0]["packets"] == r["sent_other"], rx
assert rep["dave_undecryptable"] == 0 and rep["dave"]["frames_decrypted"] >= r["sent_other"] and rx[0]["samples"] >= 960 * r["sent_other"], rep
print("dave frames out", d["frames_ok"], "in", rep["dave"]["frames_decrypted"])
PY
# without the channel id (MLS group id) DAVE cannot start: honest error, no plaintext media
R2=$("$BIN" discord voice play --endpoint "ws://127.0.0.1:$(cat "$T/port")/?v=8" --server G1 --user 1001 --session S1 --token VT --ogg "$ROOT/scripts/fixtures/sine-0.5s.ogg" || true)
grep -q "MLS group id" <<<"$R2" || fail "missing channel: $R2"
echo "[e2e-discord-dave] DAVE v1 MLS join + E2EE media both directions (libdave, offline): PASS"
