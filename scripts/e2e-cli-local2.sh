#!/usr/bin/env bash
# 1.113: upstream local-only CLI commands now implemented in openclaw-cpp.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-cli-local2] FAIL: $*"; exit 1; }
mkdir -p "$T/home/.openclaw"
export HOME="$T/home" OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/home/.openclaw/openclaw.json" OPENCLAW_DEFAULT_PROVIDER=mock
echo '{"gateway":{"auth":{"token":"plain123"}},"channels":{"telegram":{"dmPolicy":"open"}}}' > "$OPENCLAW_CONFIG_PATH"; chmod 644 "$OPENCLAW_CONFIG_PATH"
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
curl -s "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -H 'Authorization: Bearer plain123' -d '{"id":1,"method":"chat.send","params":{"sessionKey":"t1","message":"hi"}}' >/dev/null
"$BIN" transcripts path --json | grep -q "$T/sess" || fail "transcripts path"
"$BIN" sessions export-trajectory --session-key t1 --output "$T/tr.json" >/dev/null && grep -q '"steps"' "$T/tr.json" || fail "export-trajectory"
"$BIN" sessions import "$T/tr.json" | grep -q "imported:t1" || fail "sessions import"
"$BIN" agents bind --agent main --bind telegram:bot1/group:42 >/dev/null
"$BIN" agents bindings --json | grep -q '"accountId": "bot1"' || fail "agents bind"
"$BIN" agents unbind --agent main --all | grep -q "removed 1" || fail "agents unbind"
"$BIN" backup init --repository "$T/bk" >/dev/null && "$BIN" backup create --json | grep -q '"output"' || fail "backup create"
"$BIN" backup list --json | grep -q 'openclaw-backup-' || fail "backup list"
"$BIN" backup enable >/dev/null && grep -q '"enabled": true' "$OPENCLAW_CONFIG_PATH" || fail "backup enable"
out=$("$BIN" security audit --json || true); echo "$out" | grep -q config-perms && echo "$out" | grep -q dm-open && echo "$out" | grep -q plaintext-secret || fail "security audit: $out"
"$BIN" security audit --fix >/dev/null || true; [ "$(stat -c %a "$OPENCLAW_CONFIG_PATH")" = 600 ] || fail "audit --fix"
"$BIN" secrets audit --json | grep -q '"plaintext"' || fail "secrets audit"
! "$BIN" secrets audit --check >/dev/null || fail "secrets audit --check should fail on plaintext"
"$BIN" telemetry show | grep -q "sends none" || fail "telemetry show"
id1=$("$BIN" node identity --json | python3 -c 'import json,sys;print(json.load(sys.stdin)["nodeId"])'); id2=$("$BIN" node identity --json | python3 -c 'import json,sys;print(json.load(sys.stdin)["nodeId"])'); [ "$id1" = "$id2" ] || fail "node identity stable"
OPENCLAW_GATEWAY_TOKEN=plain123 "$BIN" health probe --url "http://127.0.0.1:$PORT" --json | grep -q '"ok": true' || fail "health probe"
! "$BIN" health probe --url "http://127.0.0.1:1" >/dev/null 2>&1 || fail "health probe unreachable"
OPENCLAW_GATEWAY_TOKEN=plain123 "$BIN" health export --url "http://127.0.0.1:$PORT" --output "$T/h.json" >/dev/null && grep -q '"health"' "$T/h.json" || fail "health export"
"$BIN" claws create "$T/myclaw" --agent-id myclaw >/dev/null && "$BIN" claws validate "$T/myclaw" | grep -q " — valid" || fail "claws create/validate"
"$BIN" claws inspect "$T/myclaw" --json | grep -q '"manifest"' || fail "claws inspect"
! "$BIN" claws validate "$T/nope" >/dev/null 2>&1 || fail "claws validate bad"
"$BIN" claws add "$T/myclaw" --yes >/dev/null 2>&1 && "$BIN" claws export myclaw --out "$T/x.claw.json" >/dev/null && grep -q "claw-export" "$T/x.claw.json" || fail "claws export"
"$BIN" sandbox explain --json | grep -q sandboxMode || fail "sandbox explain"
"$BIN" database status | grep -q "JSON files" || fail "database status"
echo "[e2e-cli-local2] transcripts path, sessions export/import, agents bind/unbind, backup, security/secrets audit, telemetry, node identity, health probe/export, claws create/validate/inspect/export, sandbox explain, database status: PASS"
