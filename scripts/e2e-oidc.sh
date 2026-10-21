#!/usr/bin/env bash
# OIDC auth-code + PKCE login against a fake IdP: allowed user gets an operator session; disallowed user and
# bad signature are rejected; state cookie binding enforced.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
python3 -c 'import cryptography' 2>/dev/null || { echo "[e2e-oidc] SKIP (python cryptography missing)"; exit 0; }
T=$(mktemp -d); PORT=$((20000 + RANDOM % 10000)); IP=$((30000 + RANDOM % 10000))
export OPENCLAW_STATE_DIR="$T/state" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/openclaw.json" HOME="$T"
unset OPENCLAW_MFA_TOTP_SECRET
export OPENCLAW_GATEWAY_TOKEN="gw-$RANDOM" OPENCLAW_OIDC_ISSUER="http://127.0.0.1:$IP" OPENCLAW_OIDC_CLIENT_ID="oc-test"
export OPENCLAW_OIDC_ALLOWED_EMAILS="alice@example.com"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
fail() { echo "[e2e-oidc] FAIL: $*"; tail -20 "$T/gw.log" 2>/dev/null; exit 1; }
idp() { IDP_PORT=$IP IDP_EMAIL="$1" IDP_BAD_SIG="${2:-0}" IDP_ALG="${3:-RS256}" IDP_GROUPS="${4:-}" IDP_REFRESH_DENY_FILE="$T/deny" python3 "$ROOT/scripts/fake-oidc-idp.py" >"$T/idp.log" 2>&1 & IDPPID=$!; PIDS+=($IDPPID)
  for _ in $(seq 50); do curl -s "http://127.0.0.1:$IP/jwks" >/dev/null 2>&1 && break; sleep 0.1; done; }
"$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & PIDS+=($!)
for _ in $(seq 50); do curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break; sleep 0.1; done
U="http://127.0.0.1:$PORT"
flow() { # prints final callback http code; cookie jar $T/cj
  rm -f "$T/cj"
  local loc idploc
  loc=$(curl -s -c "$T/cj" -o /dev/null -w '%{redirect_url}' "$U/login/oidc")
  [[ "$loc" == "$OPENCLAW_OIDC_ISSUER/authorize?"* ]] || { echo "noredirect:$loc"; return; }
  idploc=$(curl -s -o /dev/null -w '%{redirect_url}' "$loc")
  curl -s -b "$T/cj" -c "$T/cj" -o "$T/cb" -w '%{http_code}' "$idploc"
}
idp alice@example.com
curl -s -H "Authorization: Bearer $OPENCLAW_GATEWAY_TOKEN" "$U/v1/whoami" | grep -q "\"oidc\":true" || fail "whoami oidc flag"
[[ $(flow) == 302 ]] || { cat "$T/cb"; fail "allowed user login"; }
grep -q oc_session "$T/cj" || fail "session cookie"
curl -s -b "$T/cj" "$U/v1/whoami" | grep -q '"role":"operator"' || fail "operator role via oidc session"
# state binding: callback without the state cookie
loc=$(curl -s -o /dev/null -w '%{redirect_url}' "$U/login/oidc"); idploc=$(curl -s -o /dev/null -w '%{redirect_url}' "$loc")
[[ $(curl -s -o /dev/null -w '%{http_code}' "$idploc") == 401 ]] || fail "missing state cookie must be 401"
kill $IDPPID; wait $IDPPID 2>/dev/null || true
idp mallory@example.com
[[ $(flow) == 403 ]] || fail "disallowed email must be 403"
grep -q "not allowed" "$T/cb" || fail "not allowed message"
kill $IDPPID; wait $IDPPID 2>/dev/null || true
idp alice@example.com 1
[[ $(flow) == 403 ]] || fail "bad signature must be 403"
grep -q "signature invalid" "$T/cb" || fail "sig message"
curl -s "$U/login/methods" | grep -q "\"oidc\":true" || fail "login methods"
kill $IDPPID; wait $IDPPID 2>/dev/null || true
# 1.74: ES256 + group→role mapping + refresh-based re-validation
restart_gw() { kill ${PIDS[0]} 2>/dev/null; wait ${PIDS[0]} 2>/dev/null || true
  "$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & PIDS[0]=$!
  for _ in $(seq 80); do curl -s "$U/health" >/dev/null 2>&1 && break; sleep 0.1; done; }
export OPENCLAW_OIDC_ROLE_MAP="oc-admins=admin,oc-ops=operator" OPENCLAW_OIDC_RECHECK_SEC=0
restart_gw
idp alice@example.com 0 ES256 "oc-admins,other"
[[ $(flow) == 302 ]] || { cat "$T/cb"; fail "ES256 login"; }
curl -s -b "$T/cj" "$U/v1/whoami" | grep -q '"role":"admin"' || fail "group mapped to admin (after refresh recheck)"
touch "$T/deny"
[[ $(curl -s -o /dev/null -w '%{http_code}' -b "$T/cj" "$U/v1/status") == 401 ]] || fail "failed refresh must end session"
rm -f "$T/deny"
kill $IDPPID; wait $IDPPID 2>/dev/null || true
export OPENCLAW_OIDC_REQUIRE_GROUP=1
restart_gw
idp alice@example.com 0 ES256 "nobody"
[[ $(flow) == 403 ]] || fail "require_group must deny"
grep -q "no mapped group" "$T/cb" || fail "require_group message"
echo "[e2e-oidc] PASS"
