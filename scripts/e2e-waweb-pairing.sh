#!/usr/bin/env bash
# 1.162 e2e: WhatsApp Web ClientPayload + ADV pair-success (offline; pair fabricates a throwaway account key).
set -euo pipefail
BIN="${OPENCLAW_BIN:-$(dirname "$0")/../build/build/Release/openclaw-cpp}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export OPENCLAW_STATE_DIR="$T/state" OPENCLAW_STATE_STORE=sqlite HOME="$T"
"$BIN" whatsapp-web init >/dev/null
a=$("$BIN" whatsapp-web payload)
python3 - "$a" <<'PY'
import json,sys
a=json.loads(sys.argv[1])
assert a["kind"]=="registration" and a["bytes"]>80 and len(a["payload"])==a["bytes"]*2,a
PY
b=$("$BIN" whatsapp-web pair)
python3 - "$b" <<'PY'
import json,sys
b=json.loads(sys.argv[1])
assert b["ok"] is True and b["me"].endswith("@s.whatsapp.net") and b["platform"]=="smba" and "reply" in b,b
PY
c=$("$BIN" whatsapp-web show); d=$("$BIN" whatsapp-web payload)
python3 - "$c" "$d" <<'PY'
import json,sys
c,d=json.loads(sys.argv[1]),json.loads(sys.argv[2])
assert c["created"] is False and d["kind"]=="login" and d["bytes"]>40, (c,d)
PY
echo "PASS e2e-waweb-pairing"
