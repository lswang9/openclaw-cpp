#!/usr/bin/env bash
# TOTP MFA on gateway /login: enroll, require otp, accept code, reject replay, accept recovery code once.
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
trap _reap EXIT
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); PORT=$((20000 + RANDOM % 10000))
export OPENCLAW_STATE_DIR="$T/state" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/openclaw.json" HOME="$T"
unset OPENCLAW_MFA_TOTP_SECRET OPENCLAW_MFA_FILE
export OPENCLAW_GATEWAY_TOKEN="gw-$RANDOM-secret"
fail() { echo "[e2e-mfa] FAIL: $*"; cat "$T/gw.log" 2>/dev/null | tail -20; kill $GW 2>/dev/null || true; exit 1; }
EN=$("$BIN" auth mfa enroll tester)
RC=$(printf '%s' "$EN" | python3 -c 'import json,sys;print(json.load(sys.stdin)["recoveryCodes"][0])')
printf '%s' "$EN" | grep -q 'otpauth://totp/OpenClaw:tester' || fail "otpauth uri"
"$BIN" auth mfa status | grep -q '"enabled":true' || fail "status"
"$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 50); do curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break; sleep 0.1; done
U="http://127.0.0.1:$PORT"
post() { curl -s -o "$T/out" -w '%{http_code}' -X POST "$U/login" -H 'Content-Type: application/json' -d "$1"; }
[[ $(post "{\"token\":\"$OPENCLAW_GATEWAY_TOKEN\"}") == 401 ]] || fail "no otp must be 401"
grep -q mfa_required "$T/out" || fail "mfa_required flag"
[[ $(post "{\"token\":\"$OPENCLAW_GATEWAY_TOKEN\",\"otp\":\"000000x\"}") == 401 ]] || fail "bad otp"
[[ $(post "{\"token\":\"wrong\",\"otp\":\"$("$BIN" auth mfa code)\"}") == 401 ]] || fail "bad token w/ otp"
CODE=$("$BIN" auth mfa code)
[[ $(post "{\"token\":\"$OPENCLAW_GATEWAY_TOKEN\",\"otp\":\"$CODE\"}") == 200 ]] || fail "good otp"
grep -q '"mfa":"totp"' "$T/out" || fail "mfa via totp"
[[ $(post "{\"token\":\"$OPENCLAW_GATEWAY_TOKEN\",\"otp\":\"$CODE\"}") == 401 ]] || fail "replay must be 401"
[[ $(post "{\"token\":\"$OPENCLAW_GATEWAY_TOKEN\",\"otp\":\"$RC\"}") == 200 ]] || fail "recovery code"
[[ $(post "{\"token\":\"$OPENCLAW_GATEWAY_TOKEN\",\"otp\":\"$RC\"}") == 401 ]] || fail "recovery reuse"
curl -s "$U/login" | grep -q 'one-time-code' || fail "login ui otp field"
kill $GW; wait $GW 2>/dev/null || true
echo "[e2e-mfa] PASS"
