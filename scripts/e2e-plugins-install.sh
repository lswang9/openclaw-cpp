#!/usr/bin/env bash
# Offline `plugins install`: local dir, npm-pack tarball, npm `file:` spec; tool loads; update/uninstall.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
command -v npm >/dev/null || { echo "[e2e-pinst] SKIP (no npm)"; exit 0; }
T="${TMPDIR:-/tmp}/oc-e2e-pinst-$$"
mkdir -p "$T/src/demo" "$T/home" "$T/data"
trap 'rm -rf "$T"' EXIT
export HOME="$T/home" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_EXTENSIONS_DIR="$T/home/.openclaw/extensions"
export npm_config_cache="$T/npmcache" npm_config_offline=true
echo '{}' > "$OPENCLAW_CONFIG_PATH"
cat > "$T/src/demo/package.json" <<'J'
{ "name": "oc-demo-plugin", "version": "1.0.0", "type": "module", "main": "index.mjs", "openclaw": { "extensions": ["./index.mjs"] } }
J
cat > "$T/src/demo/openclaw.plugin.json" <<'J'
{ "id": "demo-installed", "name": "Demo Installed", "configSchema": { "type": "object" } }
J
cat > "$T/src/demo/index.mjs" <<'J'
export default function register(api) {
  api.registerTool({ name: 'demo_installed_v', description: 'v', parameters: { type: 'object', properties: {} },
    async execute() { return { content: [{ type: 'text', text: 'v1' }] }; } });
}
J
# 1) directory
"$BIN" plugins install "$T/src/demo" | grep -q '"ok": true'
[[ -f "$OPENCLAW_EXTENSIONS_DIR/demo-installed/index.mjs" ]]
grep -q '"demo-installed"' "$OPENCLAW_CONFIG_PATH"
"$BIN" plugins install "$T/src/demo" >/dev/null && { echo "[e2e-pinst] duplicate install should fail" >&2; exit 1; } || true
"$BIN" plugins info 2>/dev/null | grep -q 'demo_installed_v' || { echo "[e2e-pinst] installed tool not loaded" >&2; "$BIN" plugins info >&2; exit 1; }
# 2) tarball (npm pack, offline)
(cd "$T/src/demo" && npm pack --silent --pack-destination "$T" >/dev/null)
"$BIN" plugins install "$T/oc-demo-plugin-1.0.0.tgz" --force | grep -q '"source": "archive"'
# 3) npm spec (file:) → npm pack path
sed -i 's/"1.0.0"/"1.1.0"/' "$T/src/demo/package.json"
"$BIN" plugins install "file:$T/src/demo" --force | grep -q '"version": "1.1.0"'
python3 -c "import json,sys;d=json.load(open('$OPENCLAW_CONFIG_PATH'));r=d['plugins']['installs']['demo-installed'];assert r['source']=='npm' and r['version']=='1.1.0',r"
# 4) update re-installs from recorded spec
sed -i 's/"1.1.0"/"1.2.0"/' "$T/src/demo/package.json"
"$BIN" plugins update demo-installed | grep -q '"version": "1.2.0"'
# 5) non-plugin package rejected
mkdir -p "$T/src/notplugin"; echo '{"name":"x","version":"1.0.0"}' > "$T/src/notplugin/package.json"
"$BIN" plugins install "$T/src/notplugin" >/dev/null && { echo "[e2e-pinst] non-plugin accepted" >&2; exit 1; } || true
# 6) uninstall
"$BIN" plugins uninstall demo-installed | grep -q '"ok": true'
[[ ! -e "$OPENCLAW_EXTENSIONS_DIR/demo-installed" ]]
"$BIN" plugins installs | grep -q '"installs": {}'
echo "[e2e-pinst] plugins install dir/tgz/npm(file:)/update/uninstall + tool loads: PASS"
