#!/usr/bin/env bash
# 1.138: 16 misc channel text transports (LINE/Zalo/SMS/Twitch/X/Synology/Nextcloud/IRC/Nostr/a2a/…).
set -eu
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""
trap 'for p in $GW $FK; do kill $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-misc-channels] FAIL: $*"; [[ -f "$T/gw.log" ]] && tail -12 "$T/gw.log"; [[ -f "$T/fk.log" ]] && tail -6 "$T/fk.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN

FP=$(freeport); python3 "$ROOT/scripts/fake_misc_channels.py" "$FP" "$T/fk.log" & FK=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$FP/" >/dev/null 2>&1 && break; sleep 0.1; done
F="http://127.0.0.1:$FP"

# 16 scripted replies
python3 - "$T/script.json" <<'PY'
import json,sys
ids=["line","zalo","sms","twitch","x","synology-chat","nextcloud-talk","irc","nostr","a2a","buzz","clickclack","raft","reef","tlon","zalouser"]
json.dump([{"content":f"{i}-ok"} for i in ids], open(sys.argv[1],"w"))
PY

python3 - "$T/cfg.json" "$F" <<'PY'
import json,sys
F=sys.argv[2]
cfg={"channels":{
 "line":{"channelAccessToken":"line-tok","apiBase":F,"dmPolicy":"open","groupPolicy":"open"},
 "zalo":{"botToken":"ztok","apiBase":F,"dmPolicy":"open"},
 "sms":{"accountSid":"ACxxx","authToken":"authtok","from":"+15550001","apiBase":F,"dmPolicy":"open"},
 "twitch":{"accessToken":"tw-tok","clientId":"tw-cid","senderId":"bot1","apiBase":F,"channel":"b1","dmPolicy":"open","groupPolicy":"open"},
 "x":{"accessToken":"x-tok","apiBase":F,"dmPolicy":"open","consumerSecret":"consumer-secret"},
 "synology-chat":{"incomingUrl":F+"/incoming","dmPolicy":"open"},
 "nextcloud-talk":{"baseUrl":F,"user":"u","appPassword":"p","dmPolicy":"open","groupPolicy":"open"},
 "irc":{"apiBase":F,"token":"bridge-tok","dmPolicy":"open","groupPolicy":"open"},
 "nostr":{"apiBase":F,"token":"bridge-tok","dmPolicy":"open","privateKey":"0000000000000000000000000000000000000000000000000000000000000001"},
 "a2a":{"apiBase":F,"token":"bridge-tok","dmPolicy":"open"},
 "buzz":{"apiBase":F,"token":"bridge-tok","dmPolicy":"open"},
 "clickclack":{"apiBase":F,"token":"bridge-tok","dmPolicy":"open"},
 "raft":{"apiBase":F,"token":"bridge-tok","dmPolicy":"open"},
 "reef":{"apiBase":F,"token":"bridge-tok","dmPolicy":"open"},
 "tlon":{"apiBase":F,"token":"bridge-tok","dmPolicy":"open"},
 "zalouser":{"apiBase":F,"token":"bridge-tok","dmPolicy":"open"},
}}
json.dump(cfg, open(sys.argv[1],"w"))
PY

PORT=$(freeport)
OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_CONFIG_PATH="$T/cfg.json" \
  OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" \
  OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; kill -0 $GW 2>/dev/null || fail "gateway died"; sleep 0.2; done

inb() { curl -s -m 30 "http://127.0.0.1:$PORT/api/channels/$1/inbound" -H 'Content-Type: application/json' -d "$2"; }
has() { python3 - "$T/fk.log" "$1" <<'PY' || fail "fake log missing: $1"
import json,sys
for l in open(sys.argv[1]):
    e=json.loads(l)
    if eval(sys.argv[2]): sys.exit(0)
sys.exit(1)
PY
}

# LINE
inb line '{"events":[{"type":"message","replyToken":"rt1","source":{"type":"user","userId":"U1"},"message":{"type":"text","text":"hi"}}]}' >/dev/null
has 'e["path"].endswith("/v2/bot/message/reply") and e["auth"]=="Bearer line-tok" and e["body"]["replyToken"]=="rt1" and e["body"]["messages"][0]["text"]=="line-ok"'

# Zalo
inb zalo '{"message":{"chat":{"id":"c1"},"from":{"id":"u1"},"text":"yo"}}' >/dev/null
has '"/botztok/sendMessage" in e["path"] and e["body"]["chat_id"]=="c1" and e["body"]["text"]=="zalo-ok"'

# SMS (Twilio)
inb sms '{"From":"+15551111","To":"+15550001","Body":"ping"}' >/dev/null
has '"/Messages.json" in e["path"] and e["form"]["To"]=="+15551111" and e["form"]["Body"]=="sms-ok" and e["form"]["From"]=="+15550001"'

# Twitch
inb twitch '{"message":"hi","user_id":"u9","broadcaster_user_id":"b1"}' >/dev/null
has 'e["path"].endswith("/helix/chat/messages") and e["body"]["message"]=="twitch-ok" and e["body"]["broadcaster_id"]=="b1"'

# X
inb x '{"message_create":{"sender_id":"x1","message_data":{"text":"hi"}}}' >/dev/null
has '"/2/dm_conversations/with/x1/messages" in e["path"] and e["body"]["text"]=="x-ok"'
# 1.152: X Account Activity CRC challenge
python3 - "$PORT" <<'XP' || fail "x CRC"
import json,sys,urllib.request,hmac,hashlib,base64
port=sys.argv[1]
want="sha256="+base64.b64encode(hmac.new(b"consumer-secret", b"challenge-token", hashlib.sha256).digest()).decode()
with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/channels/x/inbound?crc_token=challenge-token", timeout=10) as r:
    body=json.load(r)
assert body.get("response_token")==want, body
req=urllib.request.Request(f"http://127.0.0.1:{port}/api/channels/x/inbound",
    data=json.dumps({"crc_token":"challenge-token"}).encode(),
    headers={"Content-Type":"application/json"}, method="POST")
with urllib.request.urlopen(req, timeout=10) as r:
    body=json.load(r)
# POST inbound wraps adapter result; accept top-level or nested response_token
tok=body.get("response_token") or (body.get("result") or {}).get("response_token")
if tok is None and body.get("ok") and "response_token" in body: tok=body["response_token"]
# OutboundRecordAdapter returns the MiscChannelCrcGet result directly as the HTTP JSON body
assert body.get("response_token")==want or tok==want, body
XP

# Synology
inb synology-chat '{"text":"hi","user_id":"42"}' >/dev/null
has 'e["path"].endswith("/incoming") and e["body"]["text"]=="synology-chat-ok" and e["body"]["user_ids"]==["42"]'

# Nextcloud Talk
inb nextcloud-talk '{"message":"hi","actorId":"ann","token":"ROOM1"}' >/dev/null
has '"/ocs/v2.php/apps/spreed/api/v1/chat/ROOM1" in e["path"] and e["body"]["message"]=="nextcloud-talk-ok"'

# IRC bridge
inb irc '{"nick":"alice","target":"#lobby","message":"hi"}' >/dev/null
has 'e["path"].endswith("/privmsg") and e["body"]["target"]=="#lobby" and e["body"]["message"]=="irc-ok"'

# Nostr NIP-04 (1.151): privateKey set → decrypt inbound, encrypt outbound
python3 - "$T/nip04_in.json" <<'NP'
import json,sys,base64
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import padding
from cryptography.hazmat.backends import default_backend
peer_priv=bytes.fromhex("00"*31+"02")
our_priv=bytes.fromhex("00"*31+"01")
kb=ec.derive_private_key(int.from_bytes(peer_priv,"big"), ec.SECP256K1(), default_backend())
ka=ec.derive_private_key(int.from_bytes(our_priv,"big"), ec.SECP256K1(), default_backend())
shared=kb.exchange(ec.ECDH(), ka.public_key())
iv=bytes([7]*16)
padder=padding.PKCS7(128).padder(); pt=padder.update(b"hi")+padder.finalize()
enc=Cipher(algorithms.AES(shared), modes.CBC(iv), backend=default_backend()).encryptor()
ct=enc.update(pt)+enc.finalize()
content=base64.b64encode(ct).decode()+"?iv="+base64.b64encode(iv).decode()
peer_pub=kb.public_key().public_numbers().x.to_bytes(32,"big").hex()
json.dump({"pubkey":peer_pub,"content":content}, open(sys.argv[1],"w"))
NP
inb nostr "$(cat "$T/nip04_in.json")" >/dev/null
python3 - "$T/fk.log" <<'NP' || fail "nostr NIP-04 round-trip"
import json,sys,base64
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import padding
from cryptography.hazmat.backends import default_backend
peer_priv=bytes.fromhex("00"*31+"02")
our_pub=bytes.fromhex("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798")
kb=ec.derive_private_key(int.from_bytes(peer_priv,"big"), ec.SECP256K1(), default_backend())
def pub_from_x(x):
    for prefix in (b"\x02", b"\x03"):
        try: return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256K1(), prefix+x)
        except Exception: continue
    raise SystemExit("bad pub")
shared=kb.exchange(ec.ECDH(), pub_from_x(our_pub))
peer_pub=kb.public_key().public_numbers().x.to_bytes(32,"big").hex()
found=False
for l in open(sys.argv[1]):
    e=json.loads(l)
    if not str(e.get("path","")).endswith("/dm"): continue
    b=e.get("body") or {}
    if b.get("pubkey")!=peer_pub: continue
    c=b.get("content","")
    if "?iv=" not in c: continue
    ct_b64, iv_b64=c.split("?iv=",1)
    ct=base64.b64decode(ct_b64); iv=base64.b64decode(iv_b64)
    dec=Cipher(algorithms.AES(shared), modes.CBC(iv), backend=default_backend()).decryptor()
    raw=dec.update(ct)+dec.finalize()
    unpadder=padding.PKCS7(128).unpadder()
    pt=unpadder.update(raw)+unpadder.finalize()
    assert pt==b"nostr-ok", pt
    assert b.get("kind")==4 and b.get("from")==our_pub.hex()
    found=True; break
assert found, "encrypted nostr outbound missing"
NP

# Generic bridges
for ch in a2a buzz clickclack raft reef tlon zalouser; do
  inb "$ch" "{\"from\":\"u\",\"text\":\"hi\",\"to\":\"c\"}" >/dev/null
done
has 'e["path"].endswith("/message") and e["body"].get("text")=="a2a-ok"'
has 'e["path"].endswith("/v1/messages") and e["body"].get("text")=="buzz-ok"'
# simpler checks for remaining
python3 - "$T/fk.log" <<'PY' || fail "generic bridges"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
def has(pred):
    assert any(pred(e) for e in L), pred
has(lambda e: e["path"].endswith("/send") and e["body"].get("text")=="clickclack-ok")
has(lambda e: e["path"].endswith("/send") and e["body"].get("text")=="raft-ok")
has(lambda e: e["path"].endswith("/send") and e["body"].get("text")=="reef-ok")
has(lambda e: e["path"].endswith("/dm") and e["body"].get("text")=="tlon-ok")
has(lambda e: e["path"].endswith("/send") and e["body"].get("text")=="zalouser-ok")
PY

# channels.status lists all 16
curl -s "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' \
  -d '{"id":1,"method":"channels.status","params":{}}' > "$T/st.json"
python3 - "$T/st.json" <<'PY' || fail "catalog"
import json,sys
d=json.load(open(sys.argv[1])); payload=d.get("payload",d.get("result",d))
ids={c["id"] for c in payload["channels"]}
need={"a2a","buzz","clickclack","irc","line","nextcloud-talk","nostr","raft","reef","sms","synology-chat","tlon","twitch","x","zalo","zalouser"}
assert need <= ids, need-ids
PY

echo "[e2e-misc-channels] OK"
