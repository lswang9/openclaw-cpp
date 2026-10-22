#!/usr/bin/env bash
# Offline: installed plugin with ChannelPlugin gateway.startAccount (inbound via api.runtime queue → agent → outbound.sendText),
# api.runtime.system/channel, provider catalog models routable as "<provider>/<model>".
set -euo pipefail
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
command -v node >/dev/null || { echo "[e2e-pcontract] SKIP (no node)"; exit 0; }
PORT="${OPENCLAW_E2E_PORT7:-$((19500 + RANDOM % 300))}"
T="${TMPDIR:-/tmp}/oc-e2e-pc-$$"
mkdir -p "$T/src/chanplug" "$T/home" "$T/data" "$T/sessions"
export HOME="$T/home" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_EXTENSIONS_DIR="$T/home/.openclaw/extensions"
export OUTBOX="$T/outbox.jsonl"
echo '{ agents: { list: [ { id: "main", default: true } ] } }' > "$OPENCLAW_CONFIG_PATH"
cat > "$T/src/chanplug/openclaw.plugin.json" <<'J'
{ "id": "chanplug", "name": "Channel contract test" }
J
cat > "$T/src/chanplug/index.mjs" <<'J'
import fs from 'fs';
export default function register(api) {
  api.registerChannel({ plugin: {
    id: 'fakechat', meta: { label: 'Fake Chat' }, capabilities: { chatTypes: ['direct'], media: false },
    config: { listAccountIds: () => ['acct1'], resolveAccount: (_cfg, id) => ({ id, token: 'from-config' }) },
    gateway: {
      async startAccount(ctx) {
        ctx.setStatus({ running: true, account: ctx.account.id });
        setTimeout(() => ctx.onMessage({ from: 'alice', text: 'hello from fakechat' }), 150);
        await new Promise((res) => ctx.abortSignal.addEventListener('abort', res));
      },
    },
    outbound: { sendText: async ({ to, text }) => { fs.appendFileSync(process.env.OUTBOX, JSON.stringify({ to, text }) + '\n'); return { ok: true }; } },
  } });
  api.registerProvider({
    id: 'catprov', label: 'Catalog Provider', auth: [{ id: 'api-key', label: 'API key', kind: 'api_key' }],
    catalog: { run: async () => ({ provider: { baseUrl: 'http://x', models: [{ id: 'cat-small', name: 'Small' }, { id: 'cat-big' }] } }) },
    complete: async ({ model }) => ({ content: 'catprov:' + model }),
  });
  api.registerCommand({ name: 'sysev', handler: () => { api.runtime.system.enqueueSystemEvent('hello-system', { sessionKey: 'x' });
    api.runtime.channel.sendMessage({ channel: 'fakechat', to: 'bob', text: 'runtime-out' }); return { text: 'queued' }; } });
}
J
"$BIN" plugins install "$T/src/chanplug" >/dev/null
cat > "$T/script.json" <<'J'
[{"content": "agent-reply-to-fakechat"}]
J
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_GATEWAY_HOST=127.0.0.1
export OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$T"; _reap' EXIT
for _ in $(seq 1 100); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
rpc() { curl -sf -X POST "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
for _ in $(seq 1 60); do grep -q agent-reply-to-fakechat "$OUTBOX" 2>/dev/null && break; sleep 0.1; done
grep -q '"to":"alice","text":"agent-reply-to-fakechat"' "$OUTBOX" || { echo "[e2e-pcontract] gateway.startAccount inbound→agent→sendText failed" >&2; cat "$T/gw.log" >&2; exit 1; }
rpc '{"jsonrpc":"2.0","id":1,"method":"plugins.channels","params":{}}' | python3 -c 'import json,sys;c=[x for x in json.load(sys.stdin)["payload"]["channels"] if x["id"]=="fakechat"][0];assert c["gateway"] and c["accounts"]==["acct1"] and c["running"]==["acct1"],c'
rpc '{"jsonrpc":"2.0","id":2,"method":"plugins.providers","params":{}}' | python3 -c 'import json,sys;p=[x for x in json.load(sys.stdin)["payload"]["providers"] if x["id"]=="catprov"][0];assert p["models"]==["cat-small","cat-big"] and p["auth"][0]["kind"]=="api_key",p'
# route "catprov/cat-big" via agent model binding
cat > "$OPENCLAW_CONFIG_PATH" <<'J'
{ agents: { list: [ { id: "main", default: true, model: "catprov/cat-big" } ] } }
J
sleep 1.1
curl -sf -m 30 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"type":"event_callback","event":{"type":"message","text":"which model","user":"U1","channel":"Cm","channel_type":"channel"}}' \
  | grep -q 'catprov:cat-big' || { echo "[e2e-pcontract] catalog model route failed" >&2; tail -20 "$T/gw.log" >&2; exit 1; }
# runtime.system + runtime.channel.sendMessage via plugin command
curl -sf -m 30 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d '{"type":"event_callback","event":{"type":"message","text":"/sysev","user":"U1","channel":"Cm","channel_type":"channel"}}' | grep -q queued
for _ in $(seq 1 40); do grep -q runtime-out "$OUTBOX" && break; sleep 0.1; done
grep -q '"to":"bob","text":"runtime-out"' "$OUTBOX" || { echo "[e2e-pcontract] runtime.channel.sendMessage not delivered" >&2; exit 1; }
echo "[e2e-pcontract] ChannelPlugin gateway/accounts/outbound + api.runtime queue + provider catalog routes: PASS"
