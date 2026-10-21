#!/usr/bin/env bash
# Offline: OpenAI / ElevenLabs / Deepgram-shaped TTS + STT against a fake server that asserts each vendor's request shape.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
FP=$((22100 + RANDOM % 300)); T="${TMPDIR:-/tmp}/oc-e2e-voice-$$"; mkdir -p "$T/out"
cat > "$T/fake.py" <<'P'
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def ok(self, body, ct):
        self.send_response(200); self.send_header("Content-Type", ct); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def bad(self, why):
        b = why.encode(); self.send_response(400); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        b = self.rfile.read(int(self.headers.get("Content-Length", 0))); h = self.headers; p = self.path
        if p == "/v1/audio/speech":
            j = json.loads(b)
            if h.get("Authorization") != "Bearer ok" or j.get("input") != "hi" or j.get("voice") != "nova" or "model" not in j: return self.bad("openai tts shape")
            return self.ok(b"ID3-openai", "audio/mpeg")
        if p.startswith("/v1/text-to-speech/"):
            j = json.loads(b)
            if h.get("xi-api-key") != "ok" or p != "/v1/text-to-speech/v123" or j.get("text") != "hi" or "model_id" not in j: return self.bad("11l tts shape")
            return self.ok(b"ID3-eleven", "audio/mpeg")
        if p.startswith("/v1/speak"):
            j = json.loads(b)
            if h.get("Authorization") != "Token ok" or "model=aura" not in p or j != {"text": "hi"}: return self.bad("dg tts shape")
            return self.ok(b"RIFF-deepgram", "audio/wav")
        if p == "/v1/audio/transcriptions":
            if h.get("Authorization") != "Bearer ok" or b'name="model"' not in b or b"voice-bytes" not in b: return self.bad("openai stt")
            return self.ok(json.dumps({"text": "openai heard"}).encode(), "application/json")
        if p == "/v1/speech-to-text":
            if h.get("xi-api-key") != "ok" or b'name="model_id"' not in b or b"scribe_v1" not in b: return self.bad("11l stt")
            return self.ok(json.dumps({"text": "eleven heard", "language_code": "en"}).encode(), "application/json")
        if p.startswith("/v1/listen"):
            if h.get("Authorization") != "Token ok" or h.get("Content-Type") != "audio/ogg" or b != b"voice-bytes" or "model=nova-3" not in p: return self.bad("dg stt")
            return self.ok(json.dumps({"results": {"channels": [{"alternatives": [{"transcript": "deepgram heard"}]}]}}).encode(), "application/json")
        self.bad("unknown " + p)
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
P
python3 "$T/fake.py" "$FP" & SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$T"' EXIT
sleep 0.4
fail() { echo "[e2e-voice] $*" >&2; exit 1; }
for spec in "openai:nova:ID3-openai" "elevenlabs:v123:ID3-eleven" "deepgram:aura-2-thalia-en:RIFF-deepgram"; do
  IFS=: read -r prov voice want <<<"$spec"
  R=$(OPENCLAW_TTS_PROVIDER=$prov OPENCLAW_TTS_URL="http://127.0.0.1:$FP" OPENCLAW_TTS_API_KEY=ok OPENCLAW_TTS_OUT_DIR="$T/out" \
      "$BIN" voice tts --voice "$voice" hi 2>&1) || fail "$prov tts cli: $R"
  echo "$R" | grep -q "$want" || fail "$prov tts: $R"
done
ls "$T/out" | grep -q '\.wav$' || fail "deepgram wav not saved"
for spec in "openai:/v1/audio/transcriptions:openai heard" "elevenlabs::eleven heard" "deepgram::deepgram heard"; do
  IFS=: read -r prov path want <<<"$spec"
  printf 'voice-bytes' > "$T/a.ogg"
  R=$(OPENCLAW_STT_PROVIDER=$prov OPENCLAW_STT_URL="http://127.0.0.1:$FP$path" OPENCLAW_STT_API_KEY=ok "$BIN" voice stt "$T/a.ogg" 2>&1) || fail "$prov stt cli: $R"
  echo "$R" | grep -q "$want" || fail "$prov stt: $R"
done
echo "[e2e-voice] openai/elevenlabs/deepgram TTS + STT request shapes: PASS"
