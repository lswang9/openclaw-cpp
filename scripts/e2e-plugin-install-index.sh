#!/usr/bin/env bash
# 1.159: plugin install records live in the installed-plugin index (<state>/plugins/installs.json state document; SQLite
# with OPENCLAW_STATE_STORE=sqlite), not openclaw.json. Legacy plugins.installs config entries migrate once.
set -euo pipefail
BIN="${OPENCLAW_BIN:-$(cd "$(dirname "$0")/.." && pwd)/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail() { echo "[e2e-plugin-install-index] FAIL: $*"; exit 1; }
export HOME="$T" OPENCLAW_STATE_DIR="$T/st" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_EXTENSIONS_DIR="$T/ext" OPENCLAW_STATE_STORE=sqlite
mkdir -p "$T/ext/legacy" "$T/src/p1"
echo '{"plugins":{"installs":{"legacy":{"source":"path","spec":"/x","installPath":"'"$T"'/ext/legacy","version":"0.1.0"}},"entries":{"legacy":{"enabled":true}}}}' > "$T/cfg.json"
"$BIN" plugins installs | python3 -c "import json,sys;r=json.load(sys.stdin);assert r['installs']['legacy']['version']=='0.1.0',r" || fail "legacy record not migrated"
python3 -c "import json;c=json.load(open('$T/cfg.json'));assert 'installs' not in c['plugins'] and c['plugins']['entries']['legacy']['enabled'],c" || fail "config still holds installs"
echo '{"id":"p1","name":"P1","version":"1.2.3","main":"index.mjs"}' > "$T/src/p1/openclaw.plugin.json"
echo 'export default {}' > "$T/src/p1/index.mjs"
"$BIN" plugins install "$T/src/p1" | grep -q '"ok": true' || fail "install"
python3 -c "import json;c=json.load(open('$T/cfg.json'));assert 'installs' not in c['plugins'] and c['plugins']['entries']['p1']['enabled'],c" || fail "install wrote installs into config"
"$BIN" plugins installs | python3 -c "import json,sys;r=json.load(sys.stdin);assert set(r['installs'])=={'legacy','p1'} and r['index'].endswith('plugins/installs.json'),r" || fail "index listing"
ls "$T/st/plugins/"*.sqlite >/dev/null 2>&1 || fail "index not in sqlite state store"
[[ ! -f "$T/st/plugins/installs.json" ]] || fail "index written as plain json despite sqlite store"
"$BIN" plugins uninstall p1 | grep -q '"ok": true' || fail "uninstall"
"$BIN" plugins installs | python3 -c "import json,sys;r=json.load(sys.stdin);assert set(r['installs'])=={'legacy'},r" || fail "record not removed"
echo "[e2e-plugin-install-index] PASS"
