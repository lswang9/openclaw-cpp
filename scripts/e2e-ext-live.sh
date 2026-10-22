#!/usr/bin/env bash
# 1.132: live listeners (Matrix /sync, Signal /v1/receive, Mattermost channel polling) and inbound auth
# (Bot Framework + Google Chat RS256 JWTs via JWKS, Matrix hs_token, Signal webhook secret), all offline.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""
trap 'for p in $GW $FK; do kill $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-ext-live] FAIL: $*"; grep -i -E "mattermost|error" "$T/gw.log" | tail -8; [[ -f "$T/fk.log" ]] && tail -4 "$T/fk.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN
cat > "$T/jwt.py" <<'PY'
import base64,json,sys,time
from cryptography.hazmat.primitives.asymmetric import rsa,padding
from cryptography.hazmat.primitives import hashes,serialization
b=lambda x:base64.urlsafe_b64encode(x).rstrip(b"=").decode()
def num(n):return b(n.to_bytes((n.bit_length()+7)//8,"big"))
cmd=sys.argv[1]; d=sys.argv[2]
if cmd=="keys":
    k=rsa.generate_private_key(public_exponent=65537,key_size=2048)
    open(d+"/key.pem","wb").write(k.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    pn=k.public_key().public_numbers()
    json.dump({"keys":[{"kty":"RSA","kid":"k1","use":"sig","n":num(pn.n),"e":num(pn.e)}]},open(d+"/jwks.json","w"))
else:
    k=serialization.load_pem_private_key(open(d+"/key.pem","rb").read(),None)
    iss,aud,exp=sys.argv[3],sys.argv[4],int(sys.argv[5])
    h=b(json.dumps({"alg":"RS256","typ":"JWT","kid":"k1"}).encode()); p=b(json.dumps({"iss":iss,"aud":aud,"exp":int(time.time())+exp,"nbf":int(time.time())-10}).encode())
    print(h+"."+p+"."+b(k.sign((h+"."+p).encode(),padding.PKCS1v15(),hashes.SHA256())))
PY
python3 "$T/jwt.py" keys "$T"
FP=$(freeport); FAKE_JWKS="$T/jwks.json" python3 "$ROOT/scripts/fake_chatops.py" "$FP" "$T/fk.log" & FK=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$FP/x" >/dev/null 2>&1 && break; sleep 0.1; done
F="http://127.0.0.1:$FP"
python3 -c 'import json;json.dump([{"content":"ok"}]*20,open("'"$T"'/script.json","w"))'
cat > "$T/cfg.json" <<JSON
{"channels":{
 "signal":{"apiBase":"$F","number":"+15550000","webhookSecret":"sig-1","dmPolicy":"open","groupPolicy":"open"},
 "matrix":{"homeserver":"$F","accessToken":"mx-tok","userId":"@bot:x","hsToken":"hs-1","dmPolicy":"open","groupPolicy":"open"},
 "msteams":{"appId":"app-1","appPassword":"pw-1","tokenUrl":"$F/botframework.com/oauth2/v2.0/token","openIdMetadataUrl":"$F/openid","dmPolicy":"open","groupPolicy":"open"},
 "googlechat":{"audience":"1234","jwksUrl":"$F/jwks","dmPolicy":"open","groupPolicy":"open"},
 "mattermost":{"baseUrl":"$F","botToken":"mm-tok","channels":["ch9"],"dmPolicy":"open","groupPolicy":"open"}}}
JSON
PORT=$(freeport)
OPENCLAW_MATRIX_SYNC_TIMEOUT_MS=1000 OPENCLAW_SIGNAL_RECEIVE_TIMEOUT=0 OPENCLAW_MATTERMOST_POLL_MS=200 \
OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" \
  OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
code() { curl -s -o "$T/resp" -w '%{http_code}' -m 30 "http://127.0.0.1:$PORT/api/channels/$1/inbound${4:-}" -H 'Content-Type: application/json' ${3:+-H "${3:-}"} -d "$2"; }
has() { for i in $(seq 1 80); do python3 - "$T/fk.log" "$1" <<'PY' && return 0
import json,sys
for l in open(sys.argv[1]):
    e=json.loads(l)
    if eval(sys.argv[2]): sys.exit(0)
sys.exit(1)
PY
sleep 0.1; done; fail "fake log has no request matching: $1"; }
push() { curl -s -X POST "$F/__push" -d "$1" >/dev/null; }
tok() { python3 "$T/jwt.py" sign "$T" "$@"; }
# --- Teams Bot Framework JWT
ACT='{"type":"message","id":"a1","text":"hi","from":{"id":"29:u"},"conversation":{"id":"tc1","conversationType":"personal"},"serviceUrl":"'"$F"'/teams/"}'
[[ $(code msteams "$ACT") == 401 ]] || fail "teams without token must be 401"
[[ $(code msteams "$ACT" "Authorization: Bearer $(tok https://api.botframework.com other-app 300)") == 401 ]] || fail "teams wrong audience"
[[ $(code msteams "$ACT" "Authorization: Bearer $(tok https://api.botframework.com app-1 -900)") == 401 ]] || fail "teams expired"
[[ $(code msteams "$ACT" "Authorization: Bearer $(tok https://evil.example app-1 300)") == 401 ]] || fail "teams wrong issuer"
[[ $(code msteams "$ACT" "Authorization: Bearer $(tok https://api.botframework.com app-1 300)") == 200 ]] || fail "teams valid token: $(cat $T/resp)"
has 'e["path"]=="/teams/v3/conversations/tc1/activities" and e["auth"]=="Bearer teams-tok"'
# --- Google Chat JWT
GC='{"type":"MESSAGE","message":{"text":"hi","sender":{"name":"users/1"}},"space":{"name":"spaces/Q","type":"DM"}}'
[[ $(code googlechat "$GC") == 401 ]] || fail "gchat without token"
[[ $(code googlechat "$GC" "Authorization: Bearer $(tok someone@else 1234 300)") == 401 ]] || fail "gchat wrong issuer"
[[ $(code googlechat "$GC" "Authorization: Bearer $(tok chat@system.gserviceaccount.com 1234 300)") == 200 ]] || fail "gchat valid"
grep -q '"text":"ok"' "$T/resp" || fail "gchat sync reply: $(cat $T/resp)"
# --- Matrix app-service hs_token, Signal webhook secret
MX='{"type":"m.room.message","room_id":"!as:x","sender":"@ann:x","content":{"body":"via appservice"}}'
[[ $(code matrix "$MX") == 401 ]] || fail "matrix push without hs_token"
[[ $(code matrix "$MX" "" "?access_token=hs-1") == 200 ]] || fail "matrix push with hs_token"
[[ $(code signal '{"envelope":{"sourceNumber":"+1","dataMessage":{"message":"x"}}}') == 401 ]] || fail "signal webhook without secret"
[[ $(code signal '{"envelope":{"sourceNumber":"+1999","dataMessage":{"message":"x"}}}' "X-Webhook-Secret: sig-1") == 200 ]] || fail "signal webhook with secret"
# --- Live listeners
for i in $(seq 1 50); do grep -q "matrix: /sync long polling" "$T/gw.log" && grep -q "mattermost: polling 1" "$T/gw.log" && break; sleep 0.1; done
grep -q "signal: polling" "$T/gw.log" || fail "signal poller not started"
sleep 0.5
push '{"kind":"matrix","item":{"room_id":"!live:x","type":"m.room.message","sender":"@ann:x","content":{"msgtype":"m.text","body":"live hello"}}}'
has 'e["method"]=="PUT" and "/rooms/%21live%3Ax/send/m.room.message/" in e["path"]'
push '{"kind":"matrix","item":{"room_id":"!live:x","type":"m.room.message","sender":"@bot:x","content":{"body":"own"}}}'
push '{"kind":"matrix_invite","item":"!inv:x"}'
has 'e["method"]=="POST" and e["path"]=="/_matrix/client/v3/join/%21inv%3Ax" and e["auth"]=="Bearer mx-tok"'
python3 - "$T/fk.log" <<'PY' || fail "matrix backlog / own messages must not be answered"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
assert not any("%21old%3Ax" in e["path"] for e in L)
assert sum(1 for e in L if "%21live%3Ax" in e["path"])==1
PY
python3 -c "import json;assert json.load(open('$T/d/matrix/sync.json'))['since'].startswith('s')" || fail "matrix since token persisted"
push '{"kind":"signal","item":{"envelope":{"sourceNumber":"+15553333","dataMessage":{"message":"polled"}}}}'
has 'e["path"]=="/v2/send" and e["body"]["recipients"]==["+15553333"]'
push '{"kind":"mattermost","item":{"id":"pp1","user_id":"u7","message":"hello mm","channel_id":"ch9","type":""}}'
has 'e["path"]=="/api/v4/posts" and e["body"]["channel_id"]=="ch9" and e["body"]["root_id"]=="pp1"'
push '{"kind":"mattermost","item":{"id":"pp2","user_id":"botid","message":"mine","channel_id":"ch9","type":""}}'
sleep 1
[[ $(grep -c '"root_id": "pp2"' "$T/fk.log" || true) == 0 ]] || fail "mattermost must ignore the bot's own posts"
echo "[e2e-ext-live] OK"
