#!/usr/bin/env bash
# 1.165: WhatsApp Web app-state primitives (LTHash + mute mutation + persist).
set -euo pipefail
BIN="${OPENCLAW_BIN:-$(cd "$(dirname "$0")/.." && pwd)/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export OPENCLAW_STATE_DIR="$T/st" OPENCLAW_STATE_STORE=sqlite HOME="$T"
a=$("$BIN" whatsapp-web app-state --dir "$T/st/whatsapp-web")
python3 - "$a" <<'PY'
import json,sys
a=json.loads(sys.argv[1])
assert a["ok"] and a["version"]==1 and a.get("saved") and a["hash"] and a["snapshotMac"],a
PY
echo "PASS e2e-waweb-app-state"
