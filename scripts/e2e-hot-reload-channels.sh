#!/usr/bin/env bash
# Offline e2e: config hot reload restarts only the channel adapters whose channels.<id> subtree changed,
# restarts the JS plugin host when plugins.* changes (plugin channels/routes re-registered), and leaves
# everything else running.
set -euo pipefail
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_PORT3:-18799}"
T="${TMPDIR:-/tmp}/oc-e2e-reload-$$"
mkdir -p "$T/sessions" "$T/data"
echo '{ channels: { slack: { enabled: true, dmPolicy: "open" }, discord: { enabled: true } } }' > "$T/data/openclaw.json"
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_GATEWAY_PORT="$PORT" SLACK_FORCE_MOCK=1 DISCORD_FORCE_MOCK=1
export OPENCLAW_GATEWAY_HOST=127.0.0.1 OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data"
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$T"; _reap' EXIT
for _ in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
rpc() { curl -sf -X POST "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
reload() { sleep 0.05; rpc '{"jsonrpc":"2.0","id":1,"method":"config.reload","params":{}}'; }
field() { python3 -c "import json,sys;print(json.dumps(json.load(sys.stdin)['payload'].get('$1')))"; }

# 1) change only channels.slack → only slack restarted
echo '{ channels: { slack: { enabled: true, dmPolicy: "pairing" }, discord: { enabled: true } } }' > "$T/data/openclaw.json"
R=$(reload)
[[ "$(echo "$R" | field channelsRestarted)" == '["slack"]' ]] || { echo "[e2e-reload] expected only slack restarted: $R" >&2; exit 1; }
# 2) change only discord
echo '{ channels: { slack: { enabled: true, dmPolicy: "pairing" }, discord: { enabled: true, guilds: { G1: {} } } } }' > "$T/data/openclaw.json"
R=$(reload)
[[ "$(echo "$R" | field channelsRestarted)" == '["discord"]' ]] || { echo "[e2e-reload] expected only discord restarted: $R" >&2; exit 1; }
# 3) unrelated change → nothing restarted
echo '{ logging: { level: "debug" }, channels: { slack: { enabled: true, dmPolicy: "pairing" }, discord: { enabled: true, guilds: { G1: {} } } } }' > "$T/data/openclaw.json"
R=$(reload)
[[ "$(echo "$R" | field channelsRestarted)" == '[]' && "$(echo "$R" | field pluginsRestarted)" == 'false' ]] || { echo "[e2e-reload] unrelated change restarted something: $R" >&2; exit 1; }
# adapters still serve after restart
curl -sf -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"type":"event_callback","event":{"type":"message","text":"after reload","user":"U1","channel":"C1","channel_type":"channel"}}' | grep -q '"ok": *true'
if command -v node >/dev/null 2>&1; then
  # 4) plugins subtree change → JS host restarted, plugin config applied, plugin channel re-registered
  echo '{ plugins: { entries: { "sample-upstream": { config: { greeting: "hey" } } } }, logging: { level: "debug" }, channels: { slack: { enabled: true, dmPolicy: "pairing" }, discord: { enabled: true, guilds: { G1: {} } } } }' > "$T/data/openclaw.json"
  R=$(reload)
  [[ "$(echo "$R" | field pluginsRestarted)" == 'true' ]] || { echo "[e2e-reload] plugins not restarted: $R" >&2; exit 1; }
  rpc '{"jsonrpc":"2.0","id":2,"method":"tools.invoke","params":{"name":"js.upstream_greet","arguments":{"name":"R"}}}' | grep -q 'hey, R!' \
    || { echo "[e2e-reload] new plugin config not applied" >&2; exit 1; }
  curl -sf -X POST "http://127.0.0.1:$PORT/api/channels/echochan/inbound" -H 'Content-Type: application/json' -d '{"user":"u","msg":"x"}' | grep -q '"ok": *true' \
    || { echo "[e2e-reload] plugin channel not re-registered" >&2; exit 1; }
fi
echo "[e2e-reload] hot reload restarts only changed channels; plugin host restart re-applies config + plugin channels: PASS"
