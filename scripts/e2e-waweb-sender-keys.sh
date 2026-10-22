#!/usr/bin/env bash
# 1.164: WhatsApp Web Sender Keys (offline group cipher + persist).
set -euo pipefail
BIN="${OPENCLAW_BIN:-$(cd "$(dirname "$0")/.." && pwd)/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export OPENCLAW_STATE_DIR="$T/st" OPENCLAW_STATE_STORE=sqlite HOME="$T"
a=$("$BIN" whatsapp-web sender-keys --dir "$T/st/whatsapp-web")
python3 - "$a" <<'PY'
import json,sys
a=json.loads(sys.argv[1])
assert a["ok"] and a["skdmBytes"]>40 and a["skmBytes"]>40 and a.get("saved") is True,a
PY
ls "$T/st/whatsapp-web/"*.sqlite >/dev/null 2>&1 || ls "$OPENCLAW_STATE_DIR"/whatsapp-web/*.sqlite >/dev/null 2>&1 || {
  # state store may put sqlite next to the keys doc path
  find "$T" -name '*.sqlite' | grep -q . || { echo "no sqlite"; find "$T" -type f; exit 1; }
}
echo "PASS e2e-waweb-sender-keys"
