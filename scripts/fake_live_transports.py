#!/usr/bin/env python3
"""Offline fakes for 1.143 live channel transports. Usage: fake_live_transports.py <http_port> <irc_port> <log>
HTTP: Mattermost REST + WebSocket (/api/v4/websocket), signal-cli JSON-RPC daemon (/api/v1/events SSE, /api/v1/rpc).
TCP: a tiny IRC server. Test hooks: POST /__mm_push {post, channel_type}, /__signal_push {envelope}, /__irc_push {line}."""
import base64, hashlib, json, socket, socketserver, struct, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
HP, IP, LOG = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
LK = threading.Lock(); MMQ = []; SIGQ = []; IRC_CLIENTS = []
def log(o):
    with LK, open(LOG, "a") as f: f.write(json.dumps(o) + "\n")
def ws_frame(text):
    b = text.encode(); h = bytes([0x81]); n = len(b)
    h += bytes([n]) if n < 126 else (bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n))
    return h + b
def ws_read(sock):
    hdr = sock.recv(2)
    if len(hdr) < 2: return None
    n = hdr[1] & 0x7f
    if n == 126: n = struct.unpack(">H", sock.recv(2))[0]
    elif n == 127: n = struct.unpack(">Q", sock.recv(8))[0]
    mask = sock.recv(4) if hdr[1] & 0x80 else b"\0\0\0\0"
    data = b""
    while len(data) < n: data += sock.recv(n - len(data))
    return bytes(c ^ mask[i % 4] for i, c in enumerate(data)).decode(errors="replace")
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        p = self.path.split("?")[0]
        log({"m": "GET", "path": self.path, "auth": self.headers.get("Authorization", "")})
        if p == "/api/v4/users/me": return self.send(200, {"id": "bot1", "username": "openclaw"})
        if p == "/api/v4/websocket":
            if self.headers.get("Authorization") != "Bearer mm-tok": return self.send(401, {"message": "unauthorized"})
            key = self.headers["Sec-WebSocket-Key"]
            acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.send_response(101); self.send_header("Upgrade", "websocket"); self.send_header("Connection", "Upgrade"); self.send_header("Sec-WebSocket-Accept", acc); self.end_headers(); self.wfile.flush()
            s = self.connection
            s.sendall(ws_frame(json.dumps({"event": "hello", "data": {"server_version": "9"}, "broadcast": {"user_id": "bot1"}, "seq": 0})))
            msg = ws_read(s); log({"ws_in": msg})
            j = json.loads(msg or "{}")
            if j.get("action") == "authentication_challenge": s.sendall(ws_frame(json.dumps({"status": "OK", "seq_reply": j.get("seq")})))
            seq = 1
            while True:
                with LK: items = MMQ[:]; MMQ.clear()
                for it in items:
                    seq += 1
                    ev = {"event": "posted", "data": {"channel_type": it.get("channel_type", "D"), "post": json.dumps(it["post"])}, "broadcast": {"channel_id": it["post"]["channel_id"]}, "seq": seq}
                    try: s.sendall(ws_frame(json.dumps(ev)))
                    except Exception: return
                time.sleep(0.05)
        if p == "/api/v1/events":
            self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.send_header("Cache-Control", "no-cache"); self.end_headers(); self.wfile.flush()
            last = time.time()
            while True:
                with LK: items = SIGQ[:]; SIGQ.clear()
                try:
                    for it in items: self.wfile.write(("event: receive\ndata: " + json.dumps({"envelope": it["envelope"], "account": "+15550001"}) + "\n\n").encode()); self.wfile.flush()
                    if time.time() - last > 1: self.wfile.write(b":keepalive\n\n"); self.wfile.flush(); last = time.time()
                except Exception: return
                time.sleep(0.05)
        self.send(404, {})
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0)); body = json.loads(raw or b"{}")
        p = self.path
        if p == "/__mm_push":
            with LK: MMQ.append(body)
            return self.send(200, {"ok": True})
        if p == "/__signal_push":
            with LK: SIGQ.append(body)
            return self.send(200, {"ok": True})
        if p == "/__irc_push":
            for c in list(IRC_CLIENTS):
                try: c.sendall((body["line"] + "\r\n").encode())
                except Exception: pass
            return self.send(200, {"ok": True, "clients": len(IRC_CLIENTS)})
        log({"m": "POST", "path": p, "auth": self.headers.get("Authorization", ""), "body": body})
        if p == "/api/v4/posts": return self.send(201, {"id": "p-out", **body})
        if p == "/api/v1/rpc":
            method=body.get("method"); params=body.get("params") or {}
            if method=="getAttachment":
                import base64
                return self.send(200, {"jsonrpc":"2.0","id":body.get("id"),"result":{"data": base64.b64encode(b"PNGDATA").decode()}})
            if method=="sendReaction":
                return self.send(200, {"jsonrpc":"2.0","id":body.get("id"),"result":{"timestamp": params.get("targetTimestamp", 1)}})
            return self.send(200, {"jsonrpc": "2.0", "id": body.get("id"), "result": {"timestamp": 1}})
        self.send(404, {})
class Irc(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request; IRC_CLIENTS.append(s); buf = b""; st = {}
        try:
            while True:
                d = s.recv(4096)
                if not d: break
                buf += d
                while b"\r\n" in buf:
                    line, buf = buf.split(b"\r\n", 1); line = line.decode(errors="replace")
                    log({"irc": line})
                    if line == "CAP REQ :sasl": st["sasl"] = True; s.sendall(b":fake.irc CAP * ACK :sasl\r\n")
                    elif line == "AUTHENTICATE PLAIN": s.sendall(b"AUTHENTICATE +\r\n")
                    elif line.startswith("AUTHENTICATE "):
                        import base64 as _b
                        parts = _b.b64decode(line[13:]).split(b"\0")
                        ok = len(parts) == 3 and parts[2] == b"sasl-pw"
                        log({"sasl": [p.decode() for p in parts], "ok": ok})
                        s.sendall(b":fake.irc 903 * :SASL ok\r\n" if ok else b":fake.irc 904 * :SASL failed\r\n")
                    elif line == "CAP END" and st.get("nick"):
                        s.sendall(f":fake.irc 001 {st['nick']} :Welcome\r\n".encode()); s.sendall(b"PING :fake.irc\r\n")
                    if line.startswith("NICK "):
                        st["nick"] = line[5:]
                        if not st.get("sasl"): s.sendall(f":fake.irc 001 {line[5:]} :Welcome\r\n".encode()); s.sendall(b"PING :fake.irc\r\n")
        finally:
            if s in IRC_CLIENTS: IRC_CLIENTS.remove(s)
class TS(socketserver.ThreadingTCPServer): allow_reuse_address = True; daemon_threads = True
threading.Thread(target=TS(("127.0.0.1", IP), Irc).serve_forever, daemon=True).start()
ThreadingHTTPServer.daemon_threads = True
ThreadingHTTPServer(("127.0.0.1", HP), H).serve_forever()
