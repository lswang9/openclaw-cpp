#!/usr/bin/env bash
# WebAuthn attestation verification (1.92, 1.99): none, packed self/x5c, fido-u2f, android-key, apple, tpm, android-safetynet;
# anchors) / fido-u2f / none, under OPENCLAW_WEBAUTHN_ATTESTATION=none (default) and =trusted. Software authenticator.
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
python3 -c 'import cryptography' 2>/dev/null || { echo "[e2e-webauthn-att] SKIP (python cryptography missing)"; exit 0; }
T=$(mktemp -d); mkdir -p "$T/ca"
FA="$ROOT/scripts/fake-webauthn.py"
GW=
trap 'kill ${GW:-} 2>/dev/null || true; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-webauthn-att] FAIL: $*"; tail -5 "$T/gw.log"; exit 1; }
# pre-create the CAs so the anchor file exists before the gateway starts
echo '{"challenge":"x"}' | python3 "$FA" create "$T/tmpk" es256 http://x x --att packed-x5c --ca-dir "$T/ca" >/dev/null
echo '{"challenge":"x"}' | python3 "$FA" create "$T/tmpk" es256 http://x x --att packed-x5c-rogue --ca-dir "$T/ca" >/dev/null
start() { # $1 policy
  [[ -n "$GW" ]] && kill $GW 2>/dev/null && wait $GW 2>/dev/null || true
  PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
  rm -rf "$T/s"; U="http://127.0.0.1:$PORT"
  OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_PIDFILE="$T/pid" OPENCLAW_GATEWAY_TOKEN=gwtok \
  OPENCLAW_WEBAUTHN_ATTESTATION="$1" OPENCLAW_WEBAUTHN_TRUST_ANCHORS="$T/ca/root.pem" \
    "$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & GW=$!
  for _ in $(seq 80); do curl -s "$U/health" >/dev/null 2>&1 && break; sleep 0.1; done
}
echo '{}' > "$T/cfg.json"
n=0
reg() { # $1 alg $2 att → response body
  n=$((n+1)); local o
  o=$(curl -sf -X POST "$U/login/webauthn/register/options" -H 'Authorization: Bearer gwtok' -d "{\"user\":\"u$n\"}")
  printf '%s' "$o" > "$T/opts.json"
  printf '%s' "$o" | python3 "$FA" create "$T/k$n" "$1" "$U" 127.0.0.1 --att "$2" --ca-dir "$T/ca" \
    | curl -s -X POST "$U/login/webauthn/register" -H 'Authorization: Bearer gwtok' -d @-
}
start none
reg es256 none | grep -q '"type":"none"' || fail "none accepted"
reg es256 packed-self | grep -q '"type":"self"' || fail "packed self (es256)"
reg ed25519 packed-self | grep -q '"type":"self"' || fail "packed self (ed25519)"
R=$(reg es256 packed-x5c); grep -q '"type":"basic"' <<<"$R" && grep -q '"trusted":true' <<<"$R" || fail "packed x5c trusted: $R"
R=$(reg es256 packed-x5c-rogue); grep -q '"type":"basic"' <<<"$R" && grep -q '"trusted":false' <<<"$R" || fail "rogue x5c accepted untrusted under none: $R"
reg es256 unknown | grep -q '"type":"unverified"' || fail "unsupported fmt recorded as unverified"
R=$(reg es256 android-key); grep -q '"fmt":"android-key"' <<<"$R" && grep -q '"trusted":true' <<<"$R" || fail "android-key: $R"
reg es256 android-key-badchal | grep -q 'attestationChallenge' || fail "android-key challenge mismatch must be rejected"
R=$(reg es256 apple); grep -q '"type":"anonca"' <<<"$R" && grep -q '"trusted":true' <<<"$R" || fail "apple: $R"
reg es256 apple-badnonce | grep -q 'nonce does not match' || fail "apple nonce mismatch must be rejected"
R=$(reg es256 tpm); grep -q '"type":"attca"' <<<"$R" && grep -q '"trusted":true' <<<"$R" || fail "tpm: $R"
reg es256 tpm-badextra | grep -q 'extraData' || fail "tpm extraData mismatch must be rejected"
R=$(reg es256 safetynet); grep -q '"fmt":"android-safetynet"' <<<"$R" && grep -q '"trusted":true' <<<"$R" || fail "safetynet: $R"
reg es256 safetynet-wronghost | grep -q 'attest.android.com' || fail "safetynet wrong host must be rejected"
reg es256 packed-x5c-badsig | grep -q 'does not verify' || fail "bad x5c signature must be rejected"
reg es256 packed-x5c-wrongou | grep -q "OU must be" || fail "wrong OU must be rejected"
reg es256 packed-x5c-aaguid | grep -q "aaguid" || fail "aaguid mismatch must be rejected"
R=$(reg es256 fido-u2f); grep -q '"fmt":"fido-u2f"' <<<"$R" && grep -q '"trusted":true' <<<"$R" || fail "fido-u2f: $R"
grep -q '"attestation":"none"' "$T/opts.json" || fail "policy none requests attestation none"
start trusted
reg es256 none | grep -q 'trusted attestation required' || fail "trusted policy must reject none"
reg es256 packed-self | grep -q 'trusted attestation required' || fail "trusted policy must reject self"
reg es256 packed-x5c-rogue | grep -q 'trusted attestation required' || fail "trusted policy must reject rogue CA"
reg es256 unknown | grep -q 'not supported' || fail "trusted policy must reject unsupported fmt"
reg es256 tpm | grep -q '"ok":true' || fail "trusted policy accepts anchored tpm"
reg es256 packed-x5c | grep -q '"ok":true' || fail "trusted policy accepts anchored x5c"
grep -q '"attestation":"direct"' "$T/opts.json" || fail "policy trusted requests direct attestation"
start required
reg es256 none | grep -q 'attestation required' || fail "required policy must reject none"
reg es256 packed-self | grep -q '"ok":true' || fail "required policy accepts self"
echo "[e2e-webauthn-att] PASS"
