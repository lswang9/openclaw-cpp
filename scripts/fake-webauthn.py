#!/usr/bin/env python3
"""Software authenticator for e2e-webauthn.sh (ES256 / Ed25519). Usage:
  fake-webauthn.py create <keyfile> <alg es256|ed25519> <origin> <rp_id> [--att FMT --ca-dir DIR]   (creation options on stdin)
     FMT: none | packed-self | packed-x5c | packed-x5c-badsig | packed-x5c-wrongou | packed-x5c-aaguid | packed-x5c-rogue | fido-u2f | tpm
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
    AAGUID = bytes(range(1, 17))
    auth = hashlib.sha256(rp_id.encode()).digest() + bytes([0x45]) + struct.pack(">I", 0) + AAGUID + struct.pack(">H", 16) + cred_id + cbor(cose)
    fmt = sys.argv[sys.argv.index("--att") + 1] if "--att" in sys.argv else "none"
    cdh = hashlib.sha256(cd).digest()
    def sign_es(key, data): return key.sign(data, ec.ECDSA(hashes.SHA256()))
    def ca(dirn, name):
        from cryptography import x509
        from cryptography.x509.oid import NameOID
        import datetime
        kp, cp = os.path.join(dirn, name + ".key"), os.path.join(dirn, name + ".pem")
        if os.path.exists(kp):
            return serialization.load_pem_private_key(open(kp, "rb").read(), None), x509.load_pem_x509_certificate(open(cp, "rb").read())
        k2 = ec.generate_private_key(ec.SECP256R1()); nm = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Fake Attestation Root " + name)])
        now = datetime.datetime.now(datetime.timezone.utc)
        c2 = (x509.CertificateBuilder().subject_name(nm).issuer_name(nm).public_key(k2.public_key()).serial_number(x509.random_serial_number())
              .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=3650))
              .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True).sign(k2, hashes.SHA256()))
        open(kp, "wb").write(k2.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        open(cp, "wb").write(c2.public_bytes(serialization.Encoding.PEM)); return k2, c2
    def leaf(ca_key, ca_cert, ou="Authenticator Attestation", aaguid=AAGUID, u2f=False):
        from cryptography import x509
        from cryptography.x509.oid import NameOID
        import datetime
        lk = ec.generate_private_key(ec.SECP256R1())
        nm = x509.Name([x509.NameAttribute(NameOID.COUNTRY_NAME, "US"), x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Fake Vendor"),
                        x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, ou), x509.NameAttribute(NameOID.COMMON_NAME, "Fake Authenticator")])
        now = datetime.datetime.now(datetime.timezone.utc)
        b = (x509.CertificateBuilder().subject_name(nm).issuer_name(ca_cert.subject).public_key(lk.public_key()).serial_number(x509.random_serial_number())
             .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=365))
             .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True))
        if not u2f: b = b.add_extension(x509.UnrecognizedExtension(x509.ObjectIdentifier("1.3.6.1.4.1.45724.1.1.4"), b"\x04\x10" + aaguid), critical=False)
        return lk, b.sign(ca_key, hashes.SHA256()).public_bytes(serialization.Encoding.DER)
    cadir = sys.argv[sys.argv.index("--ca-dir") + 1] if "--ca-dir" in sys.argv else "/tmp"
    if fmt == "none": att = cbor({"fmt": "none", "attStmt": {}, "authData": auth})
    elif fmt == "tpm": att = cbor({"fmt": "tpm", "attStmt": {"ver": "2.0"}, "authData": auth})
    elif fmt == "packed-self":
        sig = sign_es(k, auth + cdh) if alg == "es256" else k.sign(auth + cdh)
        att = cbor({"fmt": "packed", "attStmt": {"alg": -7 if alg == "es256" else -8, "sig": sig}, "authData": auth})
    elif fmt.startswith("packed-x5c"):
        ck, cc = ca(cadir, "rogue" if fmt.endswith("rogue") else "root")
        lk, der = leaf(ck, cc, ou="Wrong OU" if fmt.endswith("wrongou") else "Authenticator Attestation",
                       aaguid=bytes(16) if fmt.endswith("aaguid") else AAGUID)
        sig = sign_es(lk, auth + cdh)
        if fmt.endswith("badsig"): sig = sign_es(lk, auth + cdh + b"x")
        att = cbor({"fmt": "packed", "attStmt": {"alg": -7, "sig": sig, "x5c": [der]}, "authData": auth})
    elif fmt == "fido-u2f":
        ck, cc = ca(cadir, "root"); lk, der = leaf(ck, cc, u2f=True)
        n = k.public_key().public_numbers()
        vd = b"\0" + hashlib.sha256(rp_id.encode()).digest() + cdh + cred_id + b"\x04" + n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")
        att = cbor({"fmt": "fido-u2f", "attStmt": {"sig": sign_es(lk, vd), "x5c": [der]}, "authData": auth})
    else: raise SystemExit("unknown --att " + fmt)
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
