#!/usr/bin/env python3
"""Offline fake Telegram Bot API. Usage: fake_telegram.py <port> <logfile>. POST /__push {update} queues an update
for getUpdates; every Bot API call is appended to the log as JSON."""
import json, sys, threading, time, base64
from email.parser import BytesParser
from email.policy import default as email_default
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
PORT, LOG = int(sys.argv[1]), sys.argv[2]
Q = []; LOCK = threading.Condition(); NEXT = [1]
FILES = {}  # file_id -> bytes (POST /__file?id=<file_id> with the raw body); served via getFile + /file/bot<token>/<path>
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, obj, code=200):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def rec(self, method, args):
        with open(LOG, "a") as f: f.write(json.dumps({"method": method, "args": args}) + "\n")
    def handle_api(self, args):
        u = urlparse(self.path); parts = u.path.strip("/").split("/")
        if u.path == "/__file":
            FILES[parse_qs(u.query)["id"][0]] = args["__raw"]; return self.send({"ok": True})
        if self.path == "/__push":
            with LOCK:
                upd = args; upd["update_id"] = NEXT[0]; NEXT[0] += 1; Q.append(upd); LOCK.notify_all()
            return self.send({"ok": True})
        if len(parts) != 2 or parts[0] != "botTT-1:abc": return self.send({"ok": False, "error_code": 401, "description": "Unauthorized"}, 401)
        m = parts[1]; self.rec(m, args)
        if m == "getUpdates":
            off = int(args.get("offset", 0)); to = int(args.get("timeout", 0)); end = time.time() + min(to, 2)
            with LOCK:
                while True:
                    res = [x for x in Q if x["update_id"] >= off]
                    if res or time.time() >= end: break
                    LOCK.wait(0.1)
            return self.send({"ok": True, "result": res})
        if m == "sendMessage":
            if len(args.get("text", "")) > 4096: return self.send({"ok": False, "error_code": 400, "description": "message is too long"}, 400)
            return self.send({"ok": True, "result": {"message_id": 100, "chat": {"id": args.get("chat_id")}, **({"message_thread_id": args["message_thread_id"]} if "message_thread_id" in args else {})}})
        if m == "sendPoll":
            pid = "poll-" + str(args.get("chat_id")) + "-" + str(len(args.get("options", [])))
            return self.send({"ok": True, "result": {"message_id": 200, "chat": {"id": args.get("chat_id")}, "poll": {"id": pid, "question": args.get("question"), "options": [{"text": o, "voter_count": 0} for o in args.get("options", [])], "is_anonymous": args.get("is_anonymous", True)}}})
        if m in ("createForumTopic", "editForumTopic", "closeForumTopic", "reopenForumTopic"):
            r={"message_thread_id": args.get("message_thread_id", 42), "name": args.get("name", "topic"), "icon_color": args.get("icon_color", 0x6FB9F0)}
            if "icon_custom_emoji_id" in args: r["icon_custom_emoji_id"]=args["icon_custom_emoji_id"]
            return self.send({"ok": True, "result": r})
        if m == "deleteBusinessMessages":
            return self.send({"ok": True, "result": True})
        if m == "getFile":
            fid = args.get("file_id", "")
            if fid not in FILES: return self.send({"ok": False, "error_code": 400, "description": "file not found"}, 400)
            return self.send({"ok": True, "result": {"file_id": fid, "file_size": len(FILES[fid]), "file_path": "files/" + fid}})
        if m in ("sendPhoto", "sendDocument", "sendVideo", "sendAudio", "sendVoice", "sendAnimation"):
            return self.send({"ok": True, "result": {"message_id": 101, "chat": {"id": args.get("chat_id")}}})
        return self.send({"ok": True, "result": True})
    def do_GET(self):
        u = urlparse(self.path); parts = u.path.strip("/").split("/")
        if len(parts) == 4 and parts[0] == "file" and parts[1] == "botTT-1:abc":
            data = FILES.get(parts[3])
            if data is None: self.send_response(404); self.end_headers(); return
            self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data); return
        q = {k: v[0] for k, v in parse_qs(u.query).items()}; self.handle_api(q)
    def do_POST(self):
        b = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        ct = self.headers.get("Content-Type", "")
        if urlparse(self.path).path == "/__file": args = {"__raw": b}
        elif ct.startswith("multipart/form-data"):
            msg = BytesParser(policy=email_default).parsebytes(b"Content-Type: " + ct.encode() + b"\r\n\r\n" + b)
            args = {}
            for part in msg.iter_parts():
                name = part.get_param("name", header="content-disposition"); data = part.get_payload(decode=True) or b""
                fn = part.get_filename()
                args[name] = {"filename": fn, "bytes": len(data), "mime": part.get_content_type(), "sha_head": base64.b64encode(data[:8]).decode()} if fn else data.decode()
        else:
            try: args = json.loads(b or b"{}")
            except Exception: args = {}
        self.handle_api(args)
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
