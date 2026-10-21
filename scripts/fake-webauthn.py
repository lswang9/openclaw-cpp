#!/usr/bin/env python3
"""Software authenticator for e2e-webauthn.sh (ES256 / Ed25519). Usage:
  fake-webauthn.py create <keyfile> <alg es256|ed25519> <origin> <rp_id>   (reads creation options JSON on stdin)
  fake-webauthn.py get <keyfile> <origin> <rp_id> [--count N] [--bad-sig]  (reads request options JSON on stdin)"""
import base64, hashlib, json, os, struct, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, ed25519
def b64u(b): return base64.urlsafe_b64encode(b).rstrip(b"=").decode()
def cbor(v):
    def head(major, n):
        if n < 24: return bytes([major << 5 | n])
        if n < 256: return bytes([major << 5 | 24, n])
        if n < 65536: return bytes([major << 5 | 25]) + struct.pack(">H", n)
        return bytes([major << 5 | 26]) + struct.pack(">I", n)
    if isinstance(v, bool): return b"\xf5" if v else b"\xf4"
    if isinstance(v, int): return head(0, v) if v >= 0 else head(1, -1 - v)
    if isinstance(v, bytes): return head(2, len(v)) + v
    if isinstance(v, str): b = v.encode(); return head(3, len(b)) + b
    if isinstance(v, list): return head(4, len(v)) + b"".join(cbor(x) for x in v)
    if isinstance(v, dict): return head(5, len(v)) + b"".join(cbor(k) + cbor(x) for k, x in v.items())
    raise TypeError(v)
mode = sys.argv[1]; keyfile = sys.argv[2]; opts = json.load(sys.stdin)
if mode == "create":
    alg, origin, rp_id = sys.argv[3:6]
    if alg == "es256":
        k = ec.generate_private_key(ec.SECP256R1()); n = k.public_key().public_numbers()
        cose = {1: 2, 3: -7, -1: 1, -2: n.x.to_bytes(32, "big"), -3: n.y.to_bytes(32, "big")}
    else:
        k = ed25519.Ed25519PrivateKey.generate()
        cose = {1: 1, 3: -8, -1: 6, -2: k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)}
    cred_id = os.urandom(16)
    open(keyfile, "wb").write(json.dumps({"alg": alg, "id": b64u(cred_id), "count": 0,
        "pem": k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()).decode()}).encode())
    cd = json.dumps({"type": "webauthn.create", "challenge": opts["challenge"], "origin": origin, "crossOrigin": False}).encode()
    auth = hashlib.sha256(rp_id.encode()).digest() + bytes([0x45]) + struct.pack(">I", 0) + b"\0" * 16 + struct.pack(">H", 16) + cred_id + cbor(cose)
    att = cbor({"fmt": "none", "attStmt": {}, "authData": auth})
    print(json.dumps({"id": b64u(cred_id), "rawId": b64u(cred_id), "type": "public-key",
                      "response": {"clientDataJSON": b64u(cd), "attestationObject": b64u(att)}}))
else:
    origin, rp_id = sys.argv[3:5]; st = json.load(open(keyfile))
    count = st["count"] + 1
    if "--count" in sys.argv: count = int(sys.argv[sys.argv.index("--count") + 1])
    st["count"] = count; open(keyfile, "w").write(json.dumps(st))
    k = serialization.load_pem_private_key(st["pem"].encode(), None)
    cd = json.dumps({"type": "webauthn.get", "challenge": opts["challenge"], "origin": origin}).encode()
    auth = hashlib.sha256(rp_id.encode()).digest() + bytes([0x05]) + struct.pack(">I", count)
    msg = auth + hashlib.sha256(cd).digest()
    sig = k.sign(msg, ec.ECDSA(hashes.SHA256())) if st["alg"] == "es256" else k.sign(msg)
    if "--bad-sig" in sys.argv: sig = sig[:-1] + bytes([sig[-1] ^ 1])
    print(json.dumps({"id": st["id"], "rawId": st["id"], "type": "public-key",
                      "response": {"clientDataJSON": b64u(cd), "authenticatorData": b64u(auth), "signature": b64u(sig)}}))
