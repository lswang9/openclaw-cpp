#!/usr/bin/env bash
# ClawHub client against a local fake registry: search (429 retry), skills install/update/sync/uninstall + lock,
# moderation block, plugins install clawhub:<pkg> (npm-pack sha256 verify, blocked release, tampered artifact).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_PORT9:-$((20000 + RANDOM % 300))}"
T="${TMPDIR:-/tmp}/oc-e2e-hub-$$"
mkdir -p "$T/home" "$T/wd" "$T/srv"
python3 "$ROOT/scripts/fake_clawhub.py" "$PORT" "$T/srv" & SRV=$!
trap 'kill $SRV 2>/dev/null || true; rm -rf "$T"' EXIT
export HOME="$T/home" CLAWHUB_REGISTRY="http://127.0.0.1:$PORT" CLAWHUB_WORKDIR="$T/wd"
export OPENCLAW_CONFIG_PATH="$T/home/openclaw.json" OPENCLAW_EXTENSIONS_DIR="$T/home/.openclaw/extensions"
echo '{}' > "$OPENCLAW_CONFIG_PATH"
for _ in $(seq 1 50); do curl -s "http://127.0.0.1:$PORT/x" >/dev/null 2>&1 && break; sleep 0.1; done
fail() { echo "[e2e-clawhub] $*" >&2; exit 1; }
"$BIN" skills search demo | grep -q '"slug": "demo"' || fail "search (after 429 retry) failed"
"$BIN" skills install @openclaw/demo >/dev/null || fail "install failed"
grep -q 'Say hello (1.0.0)' "$T/wd/skills/demo/SKILL.md" || fail "skill not extracted"
python3 -c "import json;l=json.load(open('$T/wd/.clawhub/lock.json'));assert l['skills']['demo']['version']=='1.0.0',l" || fail "lock"
"$BIN" skills install demo >/dev/null 2>&1 && fail "reinstall without --force should fail"
"$BIN" skills install evil >/dev/null 2>&1 && fail "moderation-blocked skill installed"
"$BIN" skills sync | python3 -c 'import json,sys;s=json.load(sys.stdin)["skills"][0];assert s["status"]=="up-to-date" and s["match"]=="1.0.0",s' || fail "sync"
"$BIN" skills update --all | grep -q up-to-date || fail "update no-op"
curl -s -X POST "http://127.0.0.1:$PORT/__bump" >/dev/null
"$BIN" skills update --all | grep -q '"to": "1.1.0"' || fail "update to 1.1.0"
grep -q 'Say hello (1.1.0)' "$T/wd/skills/demo/SKILL.md" || fail "update not applied"
echo "local edit" >> "$T/wd/skills/demo/SKILL.md"
"$BIN" skills sync | grep -q modified-or-unpublished || fail "sync should flag local edit"
"$BIN" skills uninstall demo >/dev/null && [[ ! -d "$T/wd/skills/demo" ]] || fail "uninstall"
# plugins
"$BIN" plugins install clawhub:@acme/hubplug | grep -q '"source": "clawhub"' || fail "plugin clawhub install"
[[ -f "$OPENCLAW_EXTENSIONS_DIR/hubplug/index.mjs" ]] || fail "plugin files missing"
python3 -c "import json;r=json.load(open('$OPENCLAW_CONFIG_PATH'))['plugins']['installs']['hubplug'];assert r['clawhub']['artifact']['verified'] and r['version']=='2.1.0',r" || fail "install record"
{ "$BIN" plugins install clawhub:@acme/bad 2>&1 || true; } | grep -q 'blocked' || fail "blocked release not refused"
{ "$BIN" plugins install clawhub:@acme/tampered 2>&1 || true; } | grep -q 'sha256 mismatch' || fail "tampered artifact not refused"
"$BIN" plugins update hubplug --force | grep -q '"ok": true' || fail "plugin update via clawhub spec"
echo "[e2e-clawhub] search/install/update/sync/uninstall + plugins clawhub: (verify, blocked, tampered): PASS"
