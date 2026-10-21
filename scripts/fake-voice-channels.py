#!/usr/bin/env python3
"""Fake ElevenLabs streaming TTS (returns a real WAV so ffmpeg can transcode) + Discord / Slack / Feishu / WhatsApp
media-upload endpoints for e2e-voice-channels.sh. Records what each platform received into REPORT."""
import io, json, math, os, re, struct, sys, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
REPORT = sys.argv[1]
state = {}
def rec(k, v): state.setdefault(k, []).append(v); json.dump(state, open(REPORT + ".tmp", "w")); os.replace(REPORT + ".tmp", REPORT)
def wav():
    b = io.BytesIO(); w = wave.open(b, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
    w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(i / 8.0))) for i in range(16000))); w.close(); return b.getvalue()
WAV = wav()
def parts(ctype, body):
    bd = re.search(r'boundary=("?)([^";]+)\1', ctype).group(2).encode()
    out = {}
    for chunk in body.split(b"--" + bd)[1:-1]:
        head, _, data = chunk.partition(b"\r\n\r\n"); data = data[:-2]
        name = re.search(rb'name="([^"]+)"', head).group(1).decode()
        fn = re.search(rb'filename="([^"]+)"', head)
        out[name] = {"data": data, "filename": fn.group(1).decode() if fn else None}
    return out
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, code, body):
        b = json.dumps(body).encode(); self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n); p = self.path; ct = self.headers.get("Content-Type", "")
        auth = self.headers.get("Authorization")
        if p.startswith("/v1/text-to-speech/"):
            self.send_response(200); self.send_header("Content-Type", "audio/mpeg"); self.send_header("Content-Length", str(len(WAV))); self.end_headers(); self.wfile.write(WAV); return
        if p == "/discord/channels/C1/messages":
            pp = parts(ct, body); pj = json.loads(pp["payload_json"]["data"])
            rec("discord", {"auth": auth, "payload": pj, "file": pp["files[0]"]["filename"], "ogg": pp["files[0]"]["data"][:4] == b"OggS"}); return self.send(200, {"id": "m1"})
        if p == "/slack/files.getUploadURLExternal":
            rec("slack_get", {"auth": auth, "form": body.decode()}); return self.send(200, {"ok": True, "upload_url": "http://127.0.0.1:%d/slack-upload/F1" % PORT, "file_id": "F1"})
        if p == "/slack-upload/F1": rec("slack_upload", {"bytes": len(body), "ogg": body[:4] == b"OggS"}); return self.send(200, {})
        if p == "/slack/files.completeUploadExternal": rec("slack_complete", {"auth": auth, "body": json.loads(body)}); return self.send(200, {"ok": True})
        if p == "/open-apis/auth/v3/tenant_access_token/internal": return self.send(200, {"code": 0, "tenant_access_token": "t-9"})
        if p == "/open-apis/im/v1/files":
            pp = parts(ct, body); rec("feishu_file", {"auth": auth, "file_type": pp["file_type"]["data"].decode(), "duration": pp["duration"]["data"].decode(), "ogg": pp["file"]["data"][:4] == b"OggS"})
            return self.send(200, {"code": 0, "data": {"file_key": "file_v1"}})
        if p.startswith("/open-apis/im/v1/messages"): rec("feishu_msg", {"auth": auth, "body": json.loads(body), "path": p}); return self.send(200, {"code": 0})
        if p == "/v21.0/PH1/media":
            pp = parts(ct, body); rec("wa_media", {"auth": auth, "type": pp["type"]["data"].decode(), "ogg": pp["file"]["data"][:4] == b"OggS"}); return self.send(200, {"id": "MED9"})
        if p == "/v21.0/PH1/messages": rec("wa_msg", {"auth": auth, "body": json.loads(body)}); return self.send(200, {"messages": [{"id": "wamid"}]})
        self.send(404, {"path": p})
s = ThreadingHTTPServer(("127.0.0.1", 0), H); PORT = s.server_address[1]; print(PORT, flush=True); s.serve_forever()
