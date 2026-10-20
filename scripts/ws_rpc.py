#!/usr/bin/env python3
"""Minimal WS client for smoke: ws_rpc.py URL TOKEN_OR_- METHOD [ORIGIN_COOKIE]
Connects (Bearer token), sends one rpc frame, prints the first res frame. Exit 2 on handshake failure."""
import base64, json, os, socket, struct, sys
from urllib.parse import urlparse
u = urlparse(sys.argv[1]); tok = sys.argv[2]; method = sys.argv[3]
extra = sys.argv[4] if len(sys.argv) > 4 else ""
s = socket.create_connection((u.hostname, u.port), timeout=5)
key = base64.b64encode(os.urandom(16)).decode()
hdr = f"GET {u.path or '/ws'} HTTP/1.1\r\nHost: {u.hostname}:{u.port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" \
      f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
if tok != "-": hdr += f"Authorization: Bearer {tok}\r\n"
for h in filter(None, extra.split("|")): hdr += h + "\r\n"
s.sendall((hdr + "\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf:
    d = s.recv(4096)
    if not d: break
    buf += d
head, _, buf = buf.partition(b"\r\n\r\n")
if b" 101 " not in head.split(b"\r\n")[0]:
    print(head.split(b"\r\n")[0].decode()); sys.exit(2)
def send(txt):
    p = txt.encode(); m = os.urandom(4); b = bytes([0x81])
    n = len(p)
    b += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n)
    s.sendall(b + m + bytes(c ^ m[i % 4] for i, c in enumerate(p)))
def recv():
    global buf
    def need(k):
        global buf
        while len(buf) < k:
            d = s.recv(65536)
            if not d: raise EOFError
            buf += d
    need(2); n = buf[1] & 0x7f; off = 2
    if n == 126: need(4); n = struct.unpack(">H", buf[2:4])[0]; off = 4
    elif n == 127: need(10); n = struct.unpack(">Q", buf[2:10])[0]; off = 10
    need(off + n); p = buf[off:off + n]; buf = buf[off + n:]
    return p.decode("utf-8", "replace")
send(json.dumps({"type": "req", "id": "1", "method": method, "params": {}}))
for _ in range(20):
    f = recv()
    try: j = json.loads(f)
    except Exception: continue
    if j.get("type") == "res" or "ok" in j:
        print(json.dumps(j)); break
