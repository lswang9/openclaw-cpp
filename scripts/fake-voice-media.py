#!/usr/bin/env python3
"""Fake Telegram/WhatsApp/Discord-CDN media + OpenAI STT + ElevenLabs streaming TTS for e2e-voice-inbound.sh."""
import json, os, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
AUDIO = b"OggS" + b"\x00" * 200
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)): body = json.dumps(body).encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p = self.path
        if p.startswith("/botTOK/getFile"): return self.send(200, {"ok": True, "result": {"file_path": "voice/f1.oga"}})
        if p == "/file/botTOK/voice/f1.oga": return self.send(200, AUDIO + b"tg", "audio/ogg")
        if p == "/v21.0/MEDIA1":
            if self.headers.get("Authorization") != "Bearer WATOK": return self.send(401, {})
            return self.send(200, {"url": "http://127.0.0.1:%d/dl/media1" % self.server.server_address[1]})
        if p == "/dl/media1":
            if self.headers.get("Authorization") != "Bearer WATOK": return self.send(401, {})
            return self.send(200, AUDIO + b"wa!", "audio/ogg")
        if p == "/cdn/a.ogg": return self.send(200, AUDIO + b"dc!!", "audio/ogg")
        self.send(404, {})
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n)
        if self.path == "/v1/audio/transcriptions":
            tag = "tg" if b"OggS" in body and AUDIO + b"tg" in body else ("wa" if AUDIO + b"wa!" in body else ("dc" if AUDIO + b"dc!!" in body else "?"))
            return self.send(200, {"text": "hello from voice " + tag})
        if self.path.startswith("/v1/text-to-speech/") and self.path.endswith("/stream"):
            self.send_response(200); self.send_header("Content-Type", "audio/mpeg"); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for i in range(3):
                c = (b"ID3chunk%d" % i) * 50
                self.wfile.write(b"%x\r\n" % len(c) + c + b"\r\n"); self.wfile.flush(); time.sleep(0.15)
            self.wfile.write(b"0\r\n\r\n"); return
        self.send(404, {})
s = ThreadingHTTPServer(("127.0.0.1", 0), H); print(s.server_address[1], flush=True); s.serve_forever()
