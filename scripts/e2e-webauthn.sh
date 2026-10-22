#!/usr/bin/env bash
# WebAuthn passkeys: register (authenticated principal, ES256 + Ed25519 via a software authenticator), passwordless
# login → session cookie with the registering role; replay, wrong origin, bad signature and non-increasing
# signCount are rejected; removing the passkey ends its sessions. No browser needed.
set -euo pipefail
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
python3 -c 'import cryptography' 2>/dev/null || { echo "[e2e-webauthn] SKIP (python cryptography missing)"; exit 0; }
T=$(mktemp -d)
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
export OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_PIDFILE="$T/pid"
echo '{}' > "$OPENCLAW_CONFIG_PATH"
OPENCLAW_GATEWAY_TOKEN=gwtok "$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & GW=$!
trap 'kill $GW 2>/dev/null || true; _reap' EXIT
U="http://127.0.0.1:$PORT"; ORIGIN="$U"; RP=127.0.0.1
for _ in $(seq 80); do curl -s "$U/health" >/dev/null 2>&1 && break; sleep 0.1; done
fail() { echo "[e2e-webauthn] FAIL: $*"; tail -5 "$T/gw.log"; exit 1; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
FA="$ROOT/scripts/fake-webauthn.py"
[[ $(code -X POST "$U/login/webauthn/register/options" -d '{}') == 401 ]] || fail "register options must need auth"
[[ $(code -X POST "$U/login/webauthn/options" -d '{}') == 404 ]] || fail "no passkeys yet → 404"
reg() { # $1 keyfile $2 alg $3 user
  local o c
  o=$(curl -sf -X POST "$U/login/webauthn/register/options" -H 'Authorization: Bearer gwtok' -d "{\"user\":\"$3\"}")
  c=$(printf '%s' "$o" | python3 "$FA" create "$1" "$2" "$ORIGIN" "$RP")
  curl -s -X POST "$U/login/webauthn/register" -H 'Authorization: Bearer gwtok' -d "$c"
}
reg "$T/k1" es256 alice | grep -q '"ok":true' || fail "register es256"
reg "$T/k2" ed25519 bob | grep -q '"alg":-8' || fail "register ed25519"
curl -s "$U/login/methods" | grep -q '"webauthn":true' || fail "/login/methods webauthn"
login() { # $1 keyfile [extra args] ; prints http code, cookie jar in $T/cj
  local o a
  o=$(curl -sf -X POST "$U/login/webauthn/options" -d '{}')
  a=$(printf '%s' "$o" | python3 "$FA" get "$1" "${ORIGIN_OVERRIDE:-$ORIGIN}" "$RP" "${@:2}")
  printf '%s' "$a" > "$T/last_assertion"
  curl -s -c "$T/cj" -o "$T/login.out" -w '%{http_code}' -X POST "$U/login/webauthn" -d "$a"
}
[[ $(login "$T/k1") == 200 ]] || fail "login es256: $(cat "$T/login.out")"
curl -sf -b "$T/cj" "$U/v1/whoami" | grep -q '"role":"admin"' || fail "whoami via passkey session"
[[ $(code -X POST "$U/login/webauthn" -d @"$T/last_assertion") == 401 ]] || fail "replayed assertion must fail"
[[ $(login "$T/k2") == 200 ]] || fail "login ed25519: $(cat "$T/login.out")"
[[ $(login "$T/k1" --bad-sig) == 401 ]] && grep -q "bad signature" "$T/login.out" || fail "bad signature must fail"
[[ $(login "$T/k1" --count 1) == 401 ]] && grep -q "signCount" "$T/login.out" || fail "non-increasing signCount must fail"
[[ $(ORIGIN_OVERRIDE=https://evil.example login "$T/k1") == 401 ]] && grep -q "origin" "$T/login.out" || fail "wrong origin must fail"
[[ $(login "$T/k1") == 200 ]] || fail "login again"
cp "$T/cj" "$T/cj_alice"
CID=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["id"])' "$T/k1")
curl -s -X POST "$U/v1/rpc" -H 'Authorization: Bearer gwtok' -H 'Content-Type: application/json' -d '{"method":"webauthn.list"}' | grep -qF -- "$CID" || fail "webauthn.list"
curl -s -X POST "$U/v1/rpc" -H 'Authorization: Bearer gwtok' -H 'Content-Type: application/json' -d "{\"method\":\"webauthn.remove\",\"params\":{\"id\":\"$CID\"}}" | grep -Eq '"removed": *true' || fail "remove"
[[ $(code -b "$T/cj_alice" "$U/v1/whoami") == 401 ]] || fail "session must end when passkey removed"
echo "[e2e-webauthn] PASS"
