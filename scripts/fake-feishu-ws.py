#!/usr/bin/env python3
"""Fake Feishu open platform for e2e-feishu-ws.sh: tenant token, /callback/ws/endpoint, im/v1/messages, and a
long-connection WebSocket speaking binary pbbp2.Frame protobuf (hand-encoded here, independent of the C++ codec)."""
import base64, gzip, hashlib, json, os, socket, struct, sys, threading, time
REPORT = sys.argv[1]
state = {"pings": [], "acks": [], "replies": [], "endpoint_body": None, "errors": [], "connections": 0, "endpoint_calls": 0}
def dump(): json.dump(state, open(REPORT + ".tmp", "w")); os.replace(REPORT + ".tmp", REPORT)
# --- protobuf ---
def varint(v):
    o = b""
    while v >= 0x80: o += bytes([(v & 0x7f) | 0x80]); v >>= 7
    return o + bytes([v])
def fld(n, wt): return varint((n << 3) | wt)
def lenf(n, b): return fld(n, 2) + varint(len(b)) + b
def frame(seq, service, method, headers, payload=b"", encoding=""):
    o = fld(1, 0) + varint(seq) + fld(2, 0) + varint(0) + fld(3, 0) + varint(service) + fld(4, 0) + varint(method)
    for k, v in headers: o += lenf(5, lenf(1, k.encode()) + lenf(2, v.encode()))
    if encoding: o += lenf(6, encoding.encode())
    if payload: o += lenf(8, payload)
    return o
def rvar(b, i):
    v = s = 0
    while True:
        x = b[i]; i += 1; v |= (x & 0x7f) << s; s += 7
        if not x & 0x80: return v, i
def parse(b):
    i = 0; f = {"headers": {}, "payload": b""}
    while i < len(b):
        k, i = rvar(b, i); n, wt = k >> 3, k & 7
        if wt == 0: v, i = rvar(b, i); f[{1: "seq", 2: "log", 3: "service", 4: "method"}.get(n, n)] = v
        elif wt == 2:
            l, i = rvar(b, i); d = b[i:i + l]; i += l
            if n == 5:
                j = 0; kv = {}
                while j < len(d):
                    hk, j = rvar(d, j); hl, j = rvar(d, j); kv[hk >> 3] = d[j:j + hl].decode(); j += hl
                f["headers"][kv.get(1, "")] = kv.get(2, "")
            elif n == 8: f["payload"] = d
    return f
# --- ws ---
def recv_frame(c):
    h = c.recv(2)
    if len(h) < 2: return None
    n = h[1] & 0x7f
    if n == 126: n = struct.unpack(">H", c.recv(2))[0]
    elif n == 127: n = struct.unpack(">Q", c.recv(8))[0]
    mask = c.recv(4) if h[1] & 0x80 else b"\0" * 4
    d = b""
    while len(d) < n: d += c.recv(n - len(d))
    return h[0] & 0x0f, bytes(x ^ mask[i % 4] for i, x in enumerate(d))
def send_bin(c, d):
    n = len(d); h = b"\x82" + (bytes([n]) if n < 126 else b"\x7e" + struct.pack(">H", n))
    c.sendall(h + d)
EVENT = {"schema": "2.0", "header": {"event_id": "ev1", "event_type": "im.message.receive_v1"},
         "event": {"sender": {"sender_id": {"open_id": "ou_1"}}, "message": {"chat_id": "oc_1", "chat_type": "p2p", "message_type": "text",
                   "content": json.dumps({"text": "hello lark"})}}}
CARD = {"schema": "2.0", "header": {"event_id": "ev2", "event_type": "card.action.trigger"},
        "event": {"operator": {"open_id": "ou_2"}, "action": {"tag": "button", "value": {"text": "approve it"}},
                  "context": {"open_chat_id": "oc_9", "open_message_id": "om_1"}}}
def ws(c, path):
    state["connections"] += 1; conn = state["connections"]; dump()
    first_ping = conn == 1
    c.settimeout(0.2)
    while True:
        try: f = recv_frame(c)
        except socket.timeout: continue
        except OSError: break
        if f is None or f[0] == 8: break
        if f[0] != 2: state["errors"].append("non-binary frame op %d" % f[0]); dump(); continue
        p = parse(f[1])
        if p.get("method") == 0 and p["headers"].get("type") == "ping":
            state["pings"].append({"t": time.time(), "service": p.get("service")})
            send_bin(c, frame(0, p.get("service", 0), 0, [("type", "pong")], json.dumps({"PingInterval": 1}).encode()))
            if first_ping:
                first_ping = False
                body = json.dumps(EVENT).encode(); half = len(body) // 2
                for seq, part in ((1, body[half:]), (0, body[:half])):   # out of order on purpose
                    send_bin(c, frame(7, 33, 1, [("type", "event"), ("message_id", "M1"), ("sum", "2"), ("seq", str(seq)), ("trace_id", "tr")], part))
            dump()
        elif p.get("method") == 1:
            state["acks"].append({"headers": p["headers"], "payload": json.loads(p["payload"] or b"{}"), "seq": p.get("seq"), "conn": conn}); dump()
            if p["headers"].get("message_id") == "M1":   # then a gzip-encoded card callback
                send_bin(c, frame(8, 33, 1, [("type", "card"), ("message_id", "C1"), ("sum", "1"), ("seq", "0")], gzip.compress(json.dumps(CARD).encode()), "gzip"))
            elif p["headers"].get("message_id") == "C1":  # then a link preview callback
                pv = {"schema": "2.0", "header": {"event_type": "url.preview.get"}, "event": {"context": {"url": "http://127.0.0.1:%d/page/x" % PORT, "preview_token": "pt"}}}
                send_bin(c, frame(9, 33, 1, [("type", "card"), ("message_id", "P1"), ("sum", "1"), ("seq", "0")], json.dumps(pv).encode()))
            elif p["headers"].get("message_id") == "P1":  # drop the connection: client must reconnect per ClientConfig
                c.sendall(b"\x88\x00"); c.close(); return
def http(c, req, body):
    line = req.split(b"\r\n")[0].decode(); method, path = line.split(" ")[:2]
    hdr = {l.split(b":", 1)[0].strip().lower().decode(): l.split(b":", 1)[1].strip().decode() for l in req.split(b"\r\n")[1:] if b":" in l}
    if path == "/open-apis/auth/v3/tenant_access_token/internal": out = {"code": 0, "tenant_access_token": "t-123", "expire": 7200}
    elif path == "/callback/ws/endpoint":
        state["endpoint_body"] = json.loads(body); state["endpoint_locale"] = hdr.get("locale"); state["endpoint_calls"] += 1
        out = {"code": 0, "data": {"URL": "ws://127.0.0.1:%d/ws?device_id=d1&service_id=33" % PORT,
               "ClientConfig": {"PingInterval": 30, "ReconnectCount": 3, "ReconnectInterval": 1, "ReconnectNonce": 0}}}
    elif path.startswith("/page/"):
        d = b"<html><head><title>\n  Fake Page  Title </title></head></html>"
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(d) + d); c.close(); return
    elif path.startswith("/open-apis/im/v1/messages"):
        state["replies"].append({"auth": hdr.get("authorization"), "path": path, "body": json.loads(body)}); out = {"code": 0}
    else: out = {"code": 404}
    dump()
    d = json.dumps(out).encode()
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(d) + d); c.close()
def client(c):
    req = b""
    while b"\r\n\r\n" not in req: req += c.recv(4096)
    head, _, rest = req.partition(b"\r\n\r\n")
    if b"upgrade: websocket" in head.lower():
        key = [l.split(b":", 1)[1].strip() for l in head.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key")][0]
        acc = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
        c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + b"\r\n\r\n")
        return ws(c, head.split(b" ")[1].decode())
    n = int(([l.split(b":", 1)[1] for l in head.split(b"\r\n") if l.lower().startswith(b"content-length")] or [b"0"])[0])
    while len(rest) < n: rest += c.recv(4096)
    http(c, head, rest)
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 0)); s.listen(8)
PORT = s.getsockname()[1]; print(PORT, flush=True)
while True:
    c, _ = s.accept(); threading.Thread(target=client, args=(c,), daemon=True).start()
