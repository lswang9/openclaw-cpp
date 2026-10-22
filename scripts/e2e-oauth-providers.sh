#!/usr/bin/env bash
# 1.141: GitHub Copilot + OpenAI Codex OAuth against an offline fake: native device-code logins
# (`models auth login --provider github-copilot|openai-codex`), Copilot copilot_internal/user → CAPI with IDE headers,
# Codex authorization_code exchange → expired access refreshed (refresh_token grant) → chatgpt-account-id on /responses.
set -eu
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY COPILOT_GITHUB_TOKEN GH_TOKEN GITHUB_TOKEN 2>/dev/null || true
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FK" ]] && kill $FK 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-oauth-providers] FAIL: $*"; echo "T=$T"; [[ -f "$T/gw.log" ]] && tail -15 "$T/gw.log"; [[ -f "$T/f.log" ]] && tail -4 "$T/f.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_DEFAULT_PROVIDER
FP=$(freeport); python3 "$ROOT/scripts/fake_oauth_llm.py" "$FP" "$T/f.log" & FK=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$FP/" >/dev/null 2>&1 && break; sleep 0.1; done
B="http://127.0.0.1:$FP"
export HOME="$T" OPENCLAW_STATE_DIR="$T/st" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d"
export OPENCLAW_GITHUB_BASE="$B" OPENCLAW_GITHUB_API_BASE="$B" OPENCLAW_OPENAI_AUTH_BASE="$B" OPENCLAW_OAUTH_POLL_MS=50
python3 - "$T/cfg.json" "$B" <<'PY'
import json,sys
B=sys.argv[2]
json.dump({"models":{"providers":{"openai-codex":{"baseUrl":B+"/codex","models":[{"id":"gpt-5.5"}]}}},
  "agents":{"list":[{"id":"main","default":True},{"id":"cp","model":"github-copilot/gpt-4.1"},{"id":"cx","model":"openai-codex/gpt-5.5"}]}}, open(sys.argv[1],"w"))
PY
# device-code logins
"$BIN" models auth login --provider github-copilot </dev/null >"$T/l1.json" 2>"$T/l1.err" || fail "copilot login: $(cat $T/l1.err)"
grep -q 'ABCD-1234' "$T/l1.err" || fail "copilot user code shown"
python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); assert j["ok"] and j["profileId"]=="github-copilot:default" and j["type"]=="token", j' "$T/l1.json" || fail "copilot login result"
grep -c '/login/oauth/access_token' "$T/f.log" | grep -q '^2$' || fail "copilot polled through authorization_pending"
"$BIN" models auth login --provider openai-codex </dev/null >"$T/l2.json" 2>"$T/l2.err" || fail "codex login: $(cat $T/l2.err)"
grep -q 'CODX-9' "$T/l2.err" || fail "codex user code shown"
python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); assert j["ok"] and j["type"]=="oauth" and j["accountId"]=="acct-1", j' "$T/l2.json" || fail "codex login result"
grep -q '"gho_test"' "$T/st/agents/main/agent/auth-profiles.json" || fail "copilot token stored"
PORT=$(freeport)
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; kill -0 $GW 2>/dev/null || fail "gateway died"; sleep 0.2; done
ask() {
  curl -s -m 60 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' \
    -d "{\"id\":1,\"method\":\"chat.send\",\"params\":{\"agentId\":\"$1\",\"sessionKey\":\"agent:$1:t\",\"message\":\"hello\"}}" > "$T/r.json"
  grep -q "$2" "$T/r.json" || fail "$1: expected $2: $(head -c 500 $T/r.json)"
}
ask cp 'copilot-'
grep -q '"path": "/copilot_internal/user"' "$T/f.log" || fail "copilot user endpoint consulted"
python3 - "$T/f.log" <<'PY' || fail "copilot CAPI headers"
import json,sys
e=[json.loads(l) for l in open(sys.argv[1]) if '/capi/chat/completions' in l][-1]
h=e["headers"]; assert h["authorization"]=="Bearer gho_test" and h["copilot-integration-id"]=="copilot-developer-cli" and h["openai-organization"]=="github-copilot" and e["body"]["model"]=="gpt-4.1", h
PY
ask cx 'codex-'
python3 - "$T/f.log" "$T/st/agents/main/agent/auth-profiles.json" <<'PY' || fail "codex refresh + headers"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
assert any(e["path"]=="/oauth/token" and e["body"].get("grant_type")=="refresh_token" and e["body"].get("refresh_token")=="rt-1" for e in L), "no refresh grant"
r=[e for e in L if e["path"]=="/codex/responses"][-1]
assert r["headers"]["chatgpt-account-id"]=="acct-1" and r["headers"]["originator"]=="openclaw", r["headers"]
prof=json.load(open(sys.argv[2]))["profiles"]["openai-codex:default"]
assert prof["refresh"]=="rt-2" and prof["expires"]>0, prof
PY
echo "[e2e-oauth-providers] OK"
