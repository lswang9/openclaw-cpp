#!/usr/bin/env python3
"""Fake Discord voice server: hand-rolled WebSocket (voice gateway v8) + UDP (IP discovery + RTP decrypt with AES-GCM rtpsize).
Prints its WS port; writes a JSON report to argv[1] when Speaking(0) arrives."""
import base64, hashlib, json, os, socket, struct, sys, threading
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
REPORT = sys.argv[1]; KEY = os.urandom(32); SSRC = 4242
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); udp.bind(("127.0.0.1", 0)); UPORT = udp.getsockname()[1]
state = {"op4": [], "rtp": [], "discovery": False, "identify": None, "select": None, "speaking": [], "heartbeats": 0, "errors": []}
def udp_loop():
    while True:
        data, addr = udp.recvfrom(4096)
        if len(data) == 74 and data[:2] == b"\x00\x01":
            state["discovery"] = struct.unpack(">I", data[4:8])[0] == SSRC
            state["client_addr"] = addr
            ip = addr[0].encode().ljust(64, b"\x00")
            udp.sendto(b"\x00\x02\x00\x46" + data[4:8] + ip + struct.pack(">H", addr[1]), addr); continue
        try:
            hdr, ct, ctr = data[:12], data[12:-4], data[-4:]
            if hdr[0] != 0x80 or hdr[1] != 0x78: raise ValueError("bad rtp header")
            pt = AESGCM(KEY).decrypt(ctr + b"\x00" * 8, ct, hdr)
            seq, ts, ssrc = struct.unpack(">HII", hdr[2:12])
            state["rtp"].append({"seq": seq, "ts": ts, "ssrc": ssrc, "len": len(pt), "silence": pt == b"\xf8\xff\xfe", "sha": hashlib.sha256(pt).hexdigest()[:16]})
        except Exception as e: state["errors"].append(str(e))
threading.Thread(target=udp_loop, daemon=True).start()
def recv_frame(c):
    h = c.recv(2)
    if len(h) < 2: return None
    n = h[1] & 0x7f
    if n == 126: n = struct.unpack(">H", c.recv(2))[0]
    elif n == 127: n = struct.unpack(">Q", c.recv(8))[0]
    mask = c.recv(4) if h[1] & 0x80 else b"\x00" * 4
    d = b""
    while len(d) < n: d += c.recv(n - len(d))
    d = bytes(b ^ mask[i % 4] for i, b in enumerate(d))
    return h[0] & 0x0f, d
def send(c, obj):
    d = json.dumps(obj).encode(); n = len(d)
    h = b"\x81" + (bytes([n]) if n < 126 else b"\x7e" + struct.pack(">H", n))
    c.sendall(h + d)
def ogg_packets(path):
    b = open(path, "rb").read(); i = 0; out = []; cur = b""
    while i + 27 <= len(b) and b[i:i+4] == b"OggS":
        n = b[i+26]; lace = b[i+27:i+27+n]; d = i + 27 + n
        for l in lace:
            cur += b[d:d+l]; d += l
            if l < 255:
                if not cur.startswith(b"OpusHead") and not cur.startswith(b"OpusTags"): out.append(cur)
                cur = b""
        i = d
    return out
OTHER_SSRC = 777
def dump():
    json.dump(state, open(REPORT + ".tmp", "w")); os.replace(REPORT + ".tmp", REPORT)
def send_other_audio(c):
    """Another speaker (U2): op 5 Speaking + AES-GCM rtpsize RTP with a one-word header extension."""
    send(c, {"op": 5, "d": {"user_id": "U2", "ssrc": OTHER_SSRC, "speaking": 1}})
    addr = state.get("client_addr")
    if not addr or not OGG: return
    seq, ts = 100, 5000
    for k, pkt in enumerate(ogg_packets(OGG)):
        hdr = bytes([0x90, 0x78]) + struct.pack(">HII", seq, ts, OTHER_SSRC) + b"\xbe\xde\x00\x01"
        ctr = struct.pack(">I", 1000 + k)
        ct = AESGCM(KEY).encrypt(ctr + b"\x00" * 8, b"\x10\x00\x00\x00" + pkt, hdr)
        udp.sendto(hdr + ct + ctr, addr); seq += 1; ts += 960
        import time; time.sleep(0.005)
    state["sent_other"] = len(ogg_packets(OGG))
OGG = sys.argv[2] if len(sys.argv) > 2 else None
def main_gateway(c):
    send(c, {"op": 10, "d": {"heartbeat_interval": 30000}})
    seq = 0
    while True:
        f = recv_frame(c)
        if f is None or f[0] == 8: break
        if f[0] != 1: continue
        m = json.loads(f[1]); op = m.get("op"); seq += 1
        if op == 2:
            state["main_identify"] = m["d"].get("intents")
            send(c, {"op": 0, "t": "READY", "s": seq, "d": {"session_id": "MS1", "user": {"id": "B1"}}})
        elif op == 4:
            d = m["d"]; state["op4"].append(d)
            if d.get("channel_id"):
                send(c, {"op": 0, "t": "VOICE_STATE_UPDATE", "s": seq, "d": {"user_id": "OTHER", "session_id": "nope", "guild_id": d["guild_id"]}})
                send(c, {"op": 0, "t": "VOICE_STATE_UPDATE", "s": seq, "d": {"user_id": "B1", "session_id": "VS1", "guild_id": d["guild_id"], "channel_id": d["channel_id"]}})
                send(c, {"op": 0, "t": "VOICE_SERVER_UPDATE", "s": seq, "d": {"token": "VT", "guild_id": d["guild_id"], "endpoint": "127.0.0.1:%d" % WSPORT}})
            dump()
        elif op == 1:
            send(c, {"op": 11})
def client(c):
    req = b""
    while b"\r\n\r\n" not in req: req += c.recv(4096)
    is_main = req.split(b" ")[1].startswith(b"/gw")
    key = [l.split(b":", 1)[1].strip() for l in req.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key")][0]
    acc = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
    c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + b"\r\n\r\n")
    if is_main: return main_gateway(c)
    send(c, {"op": 8, "d": {"heartbeat_interval": 200}})
    seq = 0
    while True:
        f = recv_frame(c)
        if f is None or f[0] == 8: break
        if f[0] != 1: continue
        m = json.loads(f[1]); op = m.get("op")
        if op == 0 and os.environ.get("FAKE_DAVE_REQUIRED") == "1" and m["d"].get("max_dave_protocol_version", 0) < 1:
            state["identify"] = m["d"]; dump()
            c.sendall(b"\x88\x1e" + struct.pack(">H", 4017) + b"E2EE/DAVE protocol required"); break
        if op == 0:
            state["identify"] = m["d"]; seq += 1
            send(c, {"op": 2, "seq": seq, "d": {"ssrc": SSRC, "ip": "127.0.0.1", "port": UPORT, "modes": ["aead_aes256_gcm_rtpsize", "aead_xchacha20_poly1305_rtpsize"]}})
        elif op == 1:
            state["select"] = m["d"]; seq += 1
            send(c, {"op": 4, "seq": seq, "d": {"mode": m["d"]["data"]["mode"], "secret_key": list(KEY)}})
        elif op == 3:
            state["heartbeats"] += 1; send(c, {"op": 6, "d": m["d"]})
        elif op == 5:
            state["speaking"].append(m["d"]["speaking"])
            if m["d"]["speaking"] == 0:
                send_other_audio(c)
                import time; time.sleep(0.2)
                dump(); 
    c.close()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 0)); s.listen(4)
WSPORT = s.getsockname()[1]
print(WSPORT, flush=True)
while True:
    c, _ = s.accept(); threading.Thread(target=client, args=(c,), daemon=True).start()
