#!/usr/bin/env bash
# Offline: provider plugin auth flows — `models auth login` (OAuth run() with prompter/openUrl over host calls,
# API-key prompt), auth-profiles.json (0600, masked list), configPatch, credential injection into complete(),
# OAuth refresh before expiry, paste-token, remove.
set -euo pipefail
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
command -v node >/dev/null || { echo "[e2e-pauth] SKIP (no node)"; exit 0; }
PORT="${OPENCLAW_E2E_PAUTH:-$((21200 + RANDOM % 200))}"
T="${TMPDIR:-/tmp}/oc-e2e-pauth-$$"
mkdir -p "$T/src/authprov" "$T/home" "$T/data" "$T/sessions" "$T/state"
export HOME="$T/home" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_EXTENSIONS_DIR="$T/home/.openclaw/extensions" OPENCLAW_STATE_DIR="$T/state"
echo '{ agents: { list: [ { id: "main", default: true, model: "authprov/m1" } ] } }' > "$OPENCLAW_CONFIG_PATH"
cat > "$T/src/authprov/openclaw.plugin.json" <<'J'
{ "id": "authprov", "name": "Auth provider test" }
J
cat > "$T/src/authprov/index.mjs" <<'J'
export default function register(api) {
  api.registerProvider({
    id: 'authprov', label: 'Auth Prov', models: ['m1'],
    auth: [
      { id: 'oauth', label: 'Sign in', kind: 'oauth', async run(ctx) {
          await ctx.openUrl('https://auth.example/authorize?client_id=x');
          const code = await ctx.prompter.text({ message: 'Paste the authorization code' });
          if (code !== 'good-code') throw new Error('bad code');
          const region = await ctx.prompter.select({ message: 'Region', options: ['us', 'eu'] });
          return { profiles: [{ profileId: 'authprov:oauth', credential: { type: 'oauth', access: 'acc1', refresh: 'ref1', expires: Date.now() - 1000 } }],
                   configPatch: { plugins: { entries: { authprov: { config: { region } } } } } };
        } },
      { id: 'key', label: 'API key', kind: 'api_key' },
    ],
    async refreshOAuth(cred) {
      if (cred.refresh !== 'ref1') throw new Error('bad refresh token');
      return { access: 'acc2', refresh: 'ref2', expires: Date.now() + 3600e3 };
    },
    complete: async ({ auth }) => ({ content: auth ? `auth:${auth.type}:${auth.accessToken || auth.apiKey}` : 'auth:none' }),
  });
}
J
"$BIN" plugins install "$T/src/authprov" >/dev/null
fail() { echo "[e2e-pauth] $*" >&2; exit 1; }
printf 'good-code\n2\n' | "$BIN" models auth login --provider authprov --method oauth 2>"$T/login.err" | grep -q '"profileId": "authprov:oauth"' || { cat "$T/login.err" >&2; fail "oauth login"; }
grep -q 'https://auth.example/authorize' "$T/login.err" || fail "openUrl not surfaced"
P="$T/state/agents/main/agent/auth-profiles.json"
[[ "$(stat -c %a "$P")" == "600" ]] || fail "profiles file not 0600"
grep -q '"region": "eu"' "$OPENCLAW_CONFIG_PATH" || grep -q '"region":"eu"' "$OPENCLAW_CONFIG_PATH" || fail "configPatch not applied"
"$BIN" models auth list | grep -q 'acc1' && fail "list must mask secrets"
# gateway: expired oauth → refreshed before complete()
cat > "$T/script.json" <<'J'
[]
J
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_GATEWAY_PORT="$PORT" OPENCLAW_GATEWAY_HOST=127.0.0.1
export OPENCLAW_SESSION_DIR="$T/sessions" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_TOKENIZER_PATH="$T/none"
"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$T"; _reap' EXIT
for _ in $(seq 1 100); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
post() { curl -sf -m 30 -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" -H 'Content-Type: application/json' \
  -d "{\"type\":\"event_callback\",\"event\":{\"type\":\"message\",\"text\":\"$1\",\"user\":\"U1\",\"channel\":\"$2\",\"channel_type\":\"channel\"}}"; }
post "hi" Ca | grep -q 'auth:oauth:acc2' || { tail -20 "$T/gw.log" >&2; fail "oauth refresh + injection"; }
python3 -c "import json;p=json.load(open('$P'))['profiles']['authprov:oauth'];assert p['access']=='acc2' and p['refresh']=='ref2',p" || fail "refreshed token not persisted"
# api key (built-in prompt) takes precedence as <provider>:default
printf 'sk-test-123\n' | "$BIN" models auth login --provider authprov --method key 2>/dev/null | grep -q '"authprov:default"' || fail "api key login"
post "again" Cb | grep -q 'auth:api_key:sk-test-123' || fail "api key injection"
printf 'sk-other\n' | "$BIN" models auth paste-token --provider other 2>/dev/null | grep -q '"other:default"' || fail "paste-token"
"$BIN" models auth remove authprov:default | grep -q '"ok": true' || fail "remove"
post "third" Cc | grep -q 'auth:oauth:acc2' || fail "fallback to oauth profile after remove"
echo "[e2e-pauth] provider OAuth/API-key login, profiles store, refresh, injection, paste-token: PASS"
