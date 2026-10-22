#!/usr/bin/env bash
# 1.156 e2e: WhatsApp Web auth state persistence + pre-key upload policy (offline; the "server" pre-key count is faked).
set -euo pipefail
BIN="${OPENCLAW_BIN:-$(dirname "$0")/../build/build/Release/openclaw-cpp}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export OPENCLAW_STATE_DIR="$T/state" OPENCLAW_STATE_STORE=sqlite
a=$("$BIN" whatsapp-web init); b=$("$BIN" whatsapp-web show)
python3 - "$a" "$b" <<'PY'
import json,sys
a,b=json.loads(sys.argv[1]),json.loads(sys.argv[2])
assert a["created"] and not b["created"], (a,b)
assert a["identityKey"]==b["identityKey"] and a["registrationId"]==b["registrationId"]
assert b["signedPreKeyValid"] is True
PY
c=$("$BIN" whatsapp-web prekeys --server-count 2); d=$("$BIN" whatsapp-web prekeys --server-count 30); e=$("$BIN" whatsapp-web show)
python3 - "$c" "$d" "$e" <<'PY'
import json,sys
c,d,e=(json.loads(x) for x in sys.argv[1:])
assert c["upload"]==30 and c["preKeys"]==30 and c["nextPreKeyId"]==31, c
iq=bytes.fromhex(c["iq"]); assert iq[0]==0 and len(iq)>30*36, len(iq)
assert d["upload"]==0 and "iq" not in d
assert e["preKeys"]==30 and e["nextPreKeyId"]==31
PY
ls "$OPENCLAW_STATE_DIR/whatsapp-web/"*.sqlite >/dev/null
echo "PASS e2e-waweb-auth"
