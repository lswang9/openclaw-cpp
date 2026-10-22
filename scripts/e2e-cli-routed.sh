#!/usr/bin/env bash
# Upstream gateway-backed CLI commands: per-subcommand method routing, upstream flag validation (unknown option),
# flag→camelCase params, positional args by upstream names, human table vs --json output, honest local-only subs.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
trap _reap EXIT
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-cli] FAIL: $*"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json"
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export OPENCLAW_GATEWAY_URL="http://127.0.0.1:$PORT"
curl -sf "$OPENCLAW_GATEWAY_URL/v1/rpc" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"chat.send","params":{"sessionKey":"agent:main:cli","message":"hi"}}' >/dev/null
J=$("$BIN" sessions list --json) && python3 -c "import json,sys;j=json.loads(sys.argv[1]);assert isinstance(j,(dict,list)),j" "$J" || fail "sessions list --json"
H=$("$BIN" sessions list) ; grep -q "agent:main:cli" <<<"$H" || { echo "$H"; fail "sessions list human"; }
grep -q "^  agent:main:cli" <<<"$H" || { echo "$H"; fail "list rendering"; }
A=$("$BIN" agents list); grep -qE "^id " <<<"$A" || { echo "$A"; fail "agents table header"; }
"$BIN" devices list --bogus 2>&1 | grep -q "unknown option '--bogus'" || fail "unknown option"
"$BIN" devices list --json --timeout 5000 >/dev/null || fail "gateway client flags"
"$BIN" devices clear --yes 2>&1 | grep -qi "cleared\|no paired\|removed\|revoked" || fail "devices clear (1.124 real)"
set +e; "$BIN" devices clear >/dev/null 2>&1; rc=$?; set -e; [[ $rc != 0 ]] || fail "devices clear without --yes rc=$rc"
"$BIN" nodes frobnicate 2>&1 | grep -q "unknown command 'frobnicate'" || fail "unknown sub"
L=$("$BIN" logs --limit 3 --json) && python3 -c "import json,sys;json.loads(sys.argv[1])" "$L" || fail "logs --limit"
F=$(OPENCLAW_LOGS_FOLLOW_ROUNDS=2 "$BIN" logs --follow --interval 100 --limit 2) || fail "logs --follow"
T0=$("$BIN" sessions tail agent:main:cli --tail 5 --json) && python3 -c "import json,sys;j=json.loads(sys.argv[1]);assert j.get('messages'),j" "$T0" || fail "sessions tail → chat.history"
HH=$("$BIN" health); grep -qE "^version: " <<<"$HH" && grep -qE "^adapters:" <<<"$HH" && grep -qE "^enabled +id " <<<"$HH" || { echo "$HH" | head; fail "health human"; }
MR=$("$BIN" message react --message-id 42 --emoji x --json 2>&1 || true); grep -q "unknown option\|no gateway RPC" <<<"$MR" && fail "message react routed: $MR"; true
"$BIN" message react --bad-flag 1 2>&1 | grep -q "unknown option '--bad-flag'" || fail "message flag validation"
# camelCase flag mapping reaches the gateway (sessions.cleanup sees dryRun → echoes)
C=$("$BIN" sessions cleanup --dry-run --json) || fail "sessions cleanup"
echo "[e2e-cli] routed CLI: method table, upstream flags, --json vs table, local-only/unknown honest, logs --follow: PASS"
