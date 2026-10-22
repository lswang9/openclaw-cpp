#!/usr/bin/env bash
# 1.130: every upstream api.register* entry point is accepted (plugin keeps loading); registerWebSearchProvider feeds the
# web_search tool, registerTextTransforms rewrites user input / final assistant text, registerAgentToolResultMiddleware
# rewrites tool results before the model sees them.
set -euo pipefail
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
command -v node >/dev/null || { echo "[e2e-plugin-reg] SKIP (no node)"; exit 0; }
T=$(mktemp -d); GW=""; FL=""
trap 'for p in $GW $FL; do kill $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-plugin-reg] FAIL: $*"; tail -15 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN BRAVE_API_KEY TAVILY_API_KEY
mkdir -p "$T/src/regplug" "$T/home" "$T/data" "$T/sessions"
export HOME="$T/home" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_EXTENSIONS_DIR="$T/home/.openclaw/extensions"
FP=$(freeport); python3 "$ROOT/scripts/fake_llm.py" "$FP" "$T/llm.log" & FL=$!
echo "{ models: { providers: { cc: { baseUrl: \"http://127.0.0.1:$FP/v1\", apiKey: \"ck\" } } }, agents: { list: [ { id: \"main\", default: true }, { id: \"fk\", model: \"cc/my-model\" } ] } }" > "$OPENCLAW_CONFIG_PATH"
echo '{ "id": "regplug", "name": "Registration coverage" }' > "$T/src/regplug/openclaw.plugin.json"
cat > "$T/src/regplug/index.mjs" <<'J'
const KINDS = ['registerAgentEventSubscription','registerAgentExecutorController','registerAgentHarness','registerAutoEnableProbe',
 'registerBoardWidgetContentKind','registerCliBackend','registerCodexAppServerExtensionFactory','registerCompactionProvider','registerConfigMigration',
 'registerContextEngine','registerControlUiDescriptor','registerDecisionProvider','registerEmbeddingProvider','registerGatewayAccessPolicy',
 'registerGatewayDiscoveryService','registerHostedMediaResolver','registerImageGenerationProvider','registerInteractiveHandler',
 'registerMcpServerConnectionResolver','registerMediaUnderstandingProvider','registerMemoryCapability','registerMemoryCorpusSupplement',
 'registerMemoryPromptPreparation','registerMemoryPromptSupplement','registerMigrationProvider','registerModelCatalogProvider',
 'registerMusicGenerationProvider','registerNodeCliFeature','registerNodeHostCommand','registerNodeInvokePolicy','registerRealtimeTranscriptionProvider',
 'registerRealtimeVoiceProvider','registerReload','registerRuntimeLifecycle','registerSecurityAuditCollector','registerSessionAction',
 'registerSessionCatalog','registerSessionExtension','registerSessionSchedulerJob','registerSpeechProvider','registerStorageProvider',
 'registerToolMetadata','registerTranscriptSourceProvider','registerTrustedToolPolicy','registerVideoGenerationProvider','registerWebFetchProvider',
 'registerWidgetPresenter','registerWorkerProvider'];
export default function register(api) {
  for (const k of KINDS) api[k]({ id: 'reg-' + k, label: k });
  api.registerWebSearchProvider({ id: 'plugsearch', label: 'Plugin Search', createTool: () => ({ description: 'x', parameters: {},
    execute: async (args) => ({ results: [{ title: 'plugin hit for ' + args.query, url: 'https://p.example/1' }] }) }) });
  api.registerTextTransforms({ input: [{ from: 'SECRET-TOKEN', to: '[redacted]' }], output: [{ from: /colour/, to: 'color' }] });
  api.registerAgentToolResultMiddleware(async (event) => ({ result: { content: [{ type: 'text', text: 'mw:' + event.toolName } ] } }), { matcher: ['time_now'] });
  api.registerGatewayMethod('regplug.loaded', async () => ({ loaded: true }));
}
J
"$BIN" plugins install "$T/src/regplug" >/dev/null
cat > "$T/script.json" <<'J'
[{"tool_calls":[{"name":"time_now","arguments":{}}]},{"content":"a nice colour, colour twice"}]
J
PORT=$(freeport)
OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_SESSION_DIR="$T/sessions" \
  OPENCLAW_DATA_DIR="$T/data" OPENCLAW_PIDFILE="$T/pid" "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 1 100); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
rpc() { curl -s -m 30 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
P() { python3 -c "import json,sys;d=json.loads(sys.stdin.read());r=d.get('result',d);r=r.get('payload',r);$1"; }
rpc regplug.loaded '{}' | P 'assert r.get("loaded") is True, r' || fail "plugin calling all register* kinds must still load"
rpc tools.invoke '{"name":"web_search","args":{"query":"claws"}}' | P 'x=r["result"];assert x["provider"]=="plugsearch" and x["result"]["results"][0]["title"]=="plugin hit for claws", x' || fail "plugin web search"
rpc chat.send '{"sessionKey":"agent:main:reg","message":"use SECRET-TOKEN please"}' > "$T/chat.json"
P 'assert r["text"]=="a nice color, color twice", r' < "$T/chat.json" || fail "output text transform: $(cat $T/chat.json)"
rpc chat.history '{"sessionKey":"agent:main:reg","limit":20}' | P 't=[m for m in r["messages"] if m["role"]=="tool"];assert t and t[0]["content"]=="mw:time_now", t' || fail "tool-result middleware (mock)"
# What the provider actually receives: input transform applied, tool result rewritten.
rpc chat.send '{"agentId":"fk","sessionKey":"agent:fk:reg","message":"SECRET-TOKEN what time is it"}' >/dev/null
python3 - "$T/llm.log" <<'PY' || fail "provider-side transforms: $(tail -c 1500 $T/llm.log)"
import json,sys
reqs=[json.loads(l)["body"] for l in open(sys.argv[1])]
assert len(reqs)>=2, reqs
u=[m for m in reqs[0]["messages"] if m["role"]=="user"][-1]["content"]
u=u if isinstance(u,str) else json.dumps(u)
assert "[redacted]" in u and "SECRET-TOKEN" not in u, u
t=[m for m in reqs[1]["messages"] if m["role"]=="tool"]
assert t and t[-1]["content"]=="mw:time_now", t
PY
echo "[e2e-plugin-reg] all upstream register* kinds accepted; web search provider, text transforms, tool-result middleware: PASS"
