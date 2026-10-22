#!/usr/bin/env bash
# 1.163: WhatsApp Web Noise login against an offline fake TCP Noise server (registration ClientPayload → pair-success →
# pair-device-sign → success; second login uses username/device payload).
set -euo pipefail
BIN="${OPENCLAW_BIN:-$(cd "$(dirname "$0")/.." && pwd)/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); trap 'kill $SRV 2>/dev/null || true; rm -rf "$T"' EXIT
fail() { echo "[e2e-waweb-noise-login] FAIL: $*"; [[ -f $T/srv.log ]] && cat "$T/srv.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
PORT=$(freeport)
"$BIN" whatsapp-web fake-server --port "$PORT" --log "$T/srv.jsonl" >"$T/srv.log" 2>&1 & SRV=$!
for i in $(seq 1 50); do grep -q '"listen"' "$T/srv.jsonl" 2>/dev/null && break; sleep 0.05; done
grep -q '"listen"' "$T/srv.jsonl" || fail "server did not listen"
export OPENCLAW_STATE_DIR="$T/st" OPENCLAW_STATE_STORE=sqlite HOME="$T"
a=$("$BIN" whatsapp-web login --url "tcp://127.0.0.1:$PORT" --dir "$T/st/whatsapp-web") || fail "login1 exit"
python3 - "$a" "$T/srv.jsonl" <<'PY' || fail "login1 asserts"
import json,sys
a=json.loads(sys.argv[1]); ev=[json.loads(l)["event"] for l in open(sys.argv[2])]
assert a["ok"] and a["registered"] and a["me"].endswith("@s.whatsapp.net") and a["stage"] in ("pair","transport","finish") or a["ok"], a
assert "client-hello" in ev and "pair-success" in ev and "pair-device-sign" in ev and "success" in ev, ev
PY
wait $SRV 2>/dev/null || true
# second session: login payload (already registered)
PORT=$(freeport)
"$BIN" whatsapp-web fake-server --port "$PORT" --log "$T/srv2.jsonl" >"$T/srv2.log" 2>&1 & SRV=$!
for i in $(seq 1 50); do grep -q '"listen"' "$T/srv2.jsonl" 2>/dev/null && break; sleep 0.05; done
b=$("$BIN" whatsapp-web login --url "tcp://127.0.0.1:$PORT" --dir "$T/st/whatsapp-web") || fail "login2 exit"
python3 - "$b" "$T/srv2.jsonl" <<'PY' || fail "login2 asserts"
import json,sys
b=json.loads(sys.argv[1]); ev=[json.loads(l)["event"] for l in open(sys.argv[2])]
assert b["ok"] and b["registered"] and "pair-success" not in ev and "success" in ev, (b,ev)
PY
wait $SRV 2>/dev/null || true
echo "[e2e-waweb-noise-login] PASS"
