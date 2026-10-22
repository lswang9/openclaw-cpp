#!/usr/bin/env bash
# 1.139: previously inert api.register* kinds are consumed (invoke_registration + decision/web_fetch/media hooks).
set -euo pipefail
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
command -v node >/dev/null || { echo "[e2e-plugin-caps] SKIP (no node)"; exit 0; }
T=$(mktemp -d); GW=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-plugin-caps] FAIL: $*"; tail -20 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN
mkdir -p "$T/src/capplug" "$T/home" "$T/data" "$T/sessions"
export HOME="$T/home" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_EXTENSIONS_DIR="$T/home/.openclaw/extensions"
echo '{ agents: { list: [ { id: "main", default: true } ] } }' > "$OPENCLAW_CONFIG_PATH"
echo '{ "id": "capplug", "name": "Capability wiring" }' > "$T/src/capplug/openclaw.plugin.json"
cat > "$T/src/capplug/index.mjs" <<'J'
const log = [];
function mark(kind, method, args) { log.push({ kind, method, args }); return { ok: true, kind, method, echo: args }; }
export default function register(api) {
  api.registerDecisionProvider({ id: 'cap-decision', label: 'Cap Decision',
    evaluate: async (args) => ({ result: Object.fromEntries(Object.keys(args.questions || {}).map((q) => [q, { type: 'boolean', probabilityTrue: 0.75 }])) }) });
  api.registerWebFetchProvider({ id: 'cap-fetch', label: 'Cap Fetch',
    fetch: async (args) => ({ text: 'fetched:' + (args.url || '') }) });
  api.registerImageGenerationProvider({ id: 'cap-img', generate: async (args) => ({ files: ['/tmp/cap-img.png'], prompt: args.prompt }) });
  api.registerVideoGenerationProvider({ id: 'cap-vid', generate: async (args) => ({ files: ['/tmp/cap-vid.mp4'], prompt: args.prompt }) });
  api.registerMusicGenerationProvider({ id: 'cap-mus', generate: async (args) => ({ files: ['/tmp/cap-mus.wav'], prompt: args.prompt }) });
  api.registerEmbeddingProvider({ id: 'cap-emb', execute: async (args) => ({ vectors: [[0.1, 0.2, 0.3]], input: args }) });
  api.registerSpeechProvider({ id: 'cap-speech', execute: async (args) => ({ audio: 'b64', text: args.text || '' }) });
  api.registerCompactionProvider({ id: 'cap-comp', execute: async (args) => ({ summary: 'compacted', n: (args.messages || []).length }) });
  api.registerMemoryPromptSupplement({ id: 'cap-mem', execute: async () => ({ text: 'mem-supplement' }) });
  api.registerSecurityAuditCollector({ id: 'cap-audit', collect: async () => ({ findings: [{ id: 'x', severity: 'info' }] }) });
  api.registerNodeHostCommand({ id: 'cap.node.ping', execute: async () => ({ pong: true }) });
  api.registerSessionCatalog({ id: 'cap-sessions', execute: async () => ({ sessions: [{ id: 's1' }] }) });
  api.registerGatewayAccessPolicy({ id: 'cap-gap', execute: async (args) => ({ allow: true, method: args.method }) });
  api.registerTrustedToolPolicy({ id: 'cap-ttp', execute: async (args) => ({ allow: true, tool: args.tool }) });
  api.registerReload({ id: 'cap-reload', reload: async (args) => mark('registerReload', 'reload', args) });
  api.registerRuntimeLifecycle({ id: 'cap-life',
    start: async () => mark('registerRuntimeLifecycle', 'start', {}),
    stop: async () => mark('registerRuntimeLifecycle', 'stop', {}) });
  // Remaining inert kinds: still callable via plugins.invokeRegistration
  for (const k of ['registerAgentEventSubscription','registerAgentExecutorController','registerAgentHarness','registerAutoEnableProbe',
    'registerBoardWidgetContentKind','registerCliBackend','registerCodexAppServerExtensionFactory','registerConfigMigration',
    'registerContextEngine','registerControlUiDescriptor','registerGatewayDiscoveryService','registerHostedMediaResolver',
    'registerInteractiveHandler','registerMcpServerConnectionResolver','registerMediaUnderstandingProvider','registerMemoryCapability',
    'registerMemoryCorpusSupplement','registerMemoryPromptPreparation','registerMigrationProvider','registerModelCatalogProvider',
    'registerNodeCliFeature','registerNodeInvokePolicy','registerRealtimeTranscriptionProvider','registerRealtimeVoiceProvider',
    'registerSessionAction','registerSessionExtension','registerSessionSchedulerJob','registerStorageProvider','registerToolMetadata',
    'registerTranscriptSourceProvider','registerWidgetPresenter','registerWorkerProvider']) {
    api[k]({ id: 'cap-' + k, execute: async (args) => mark(k, 'execute', args) });
  }
  api.registerGatewayMethod('capplug.log', async () => ({ log }));
}
J
"$BIN" plugins install "$T/src/capplug" >/dev/null
PORT=$(freeport)
OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_SESSION_DIR="$T/sessions" \
  OPENCLAW_DATA_DIR="$T/data" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_MEDIA_PROVIDER=mock "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 1 100); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
rpc() { curl -s -m 30 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
unwrap() {
  python3 -c '
import json,sys
raw=sys.stdin.read()
if not raw.strip():
    raise SystemExit("empty rpc response")
d=json.loads(raw)
r=d.get("result",d)
if isinstance(r,dict) and "payload" in r: r=r["payload"]
if isinstance(r,dict) and "tool" in r and "result" in r: r=r["result"]
if isinstance(r,str):
    try: r=json.loads(r)
    except Exception: pass
print(json.dumps(r))
'
}
P() {
  local code="$1"
  unwrap | OPENCLAW_ASSERT="$code" python3 -c 'import json,sys,os; r=json.load(sys.stdin); exec(os.environ["OPENCLAW_ASSERT"])'
}

rpc plugins.listCapabilities '{}' | P 'regs=r.get("registrations") or {}; prov=r.get("providers") or {}; assert ("registerDecisionProvider" in regs and regs["registerDecisionProvider"]) or ("registerDecisionProvider" in prov and prov["registerDecisionProvider"]), r' || fail "listCapabilities"

rpc tools.invoke '{"name":"decision_evaluate","args":{"state":{"x":1},"questions":{"q1":{"type":"boolean"}}}}' | P 'assert r.get("status")=="ok" and r["result"]["q1"]["probabilityTrue"]==0.75, r' || fail "decision plugin"

rpc tools.invoke '{"name":"web_fetch","args":{"url":"https://example.test/a"}}' | P 'assert "fetched:https://example.test/a" in str(r), r' || fail "web_fetch plugin"

rpc tools.invoke '{"name":"image_generate","args":{"prompt":"cat"}}' | P 'assert r.get("provider")=="plugin" and r["files"]==["/tmp/cap-img.png"], r' || fail "image plugin"

rpc plugins.invokeRegistration '{"kind":"registerContextEngine","args":{"op":"ping"}}' | P 'assert r.get("ok") and (r.get("result") or {}).get("kind")=="registerContextEngine", r' || fail "invoke inert"

rpc plugins.invokeRegistration '{"kind":"registerReload","method":"reload","args":{"reason":"e2e"}}' | P 'assert r.get("ok") and (r.get("result") or {}).get("kind")=="registerReload", r' || fail "reload invoke"
rpc plugins.invokeRegistration '{"kind":"registerEmbeddingProvider","args":{"texts":["hi"]}}' | P 'assert r.get("ok") and "vectors" in (r.get("result") or {}), r' || fail "embedding invoke"

echo "[e2e-plugin-caps] PASS"
