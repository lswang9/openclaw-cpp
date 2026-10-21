#!/usr/bin/env bash
# Prove config.reload (deps lock) does not race concurrent HTTP inbound; bounded workers avoid OOM.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_PORT4:-$((19000 + RANDOM % 500))}"
T="${TMPDIR:-/tmp}/oc-e2e-lock-$$"
mkdir -p "$T/sessions" "$T/data" "$T/jobs" "$T/home"
echo '{ logging: { level: "info" }, channels: { slack: { enabled: true } } }' > "$T/data/openclaw.json"

# Isolate from parent smoke env (skills/agents/data paths, tokens, etc.).
export HOME="$T/home"
export OPENCLAW_DEFAULT_PROVIDER=mock
export OPENCLAW_GATEWAY_PORT="$PORT"
export OPENCLAW_GATEWAY_HOST=127.0.0.1
export OPENCLAW_SESSION_DIR="$T/sessions"
export OPENCLAW_DATA_DIR="$T/data"
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json"
export OPENCLAW_PIDFILE="$T/pid"
export OPENCLAW_TOKENIZER_PATH="$T/none"
export SLACK_FORCE_MOCK=1
unset OPENCLAW_GATEWAY_TOKEN OPENCLAW_SKILLS_DIR OPENCLAW_AGENTS_DIR OPENCLAW_JS_FORCE_MOCK || true
unset OPENCLAW_WORKSPACE OPENCLAW_STATE_DIR OPENCLAW_PLUGIN_PATH || true

"$BIN" gateway >"$T/gw.log" 2>&1 &
GW=$!
trap 'kill $GW 2>/dev/null || true; wait $GW 2>/dev/null || true; rm -rf "$T"' EXIT
ok_health=0
for _ in $(seq 1 80); do
  if grep -q "http+ws://127.0.0.1:$PORT" "$T/gw.log" 2>/dev/null && curl -sf "http://127.0.0.1:$PORT/health" >/dev/null; then
    ok_health=1
    break
  fi
  if ! kill -0 "$GW" 2>/dev/null; then
    echo "[e2e-lock] gateway exited early" >&2
    tail -50 "$T/gw.log" >&2
    exit 1
  fi
  sleep 0.1
done
[[ "$ok_health" -eq 1 ]] || { echo "[e2e-lock] gateway health failed on port $PORT" >&2; tail -50 "$T/gw.log" >&2; exit 1; }

check_json() {
  python3 - "$1" "$2" <<'PY'
import json, sys
path, expr = sys.argv[1], sys.argv[2]
try:
    with open(path) as f:
        data = json.load(f)
except Exception as e:
    print(f"json-error:{e}", file=sys.stderr)
    sys.exit(1)
ok = eval(expr, {"data": data})
sys.exit(0 if ok else 1)
PY
}

# Modest overlap: enough to exercise reload vs inbound, avoid thread/memory storms beside smoke's gateway.
fail=0
for wave in $(seq 1 6); do
  pids=()
  for j in 1 2; do
    i=$(( (wave - 1) * 2 + j ))
    if (( i % 2 == 0 )); then
      echo '{ logging: { level: "debug" }, channels: { slack: { enabled: true } } }' > "$T/data/openclaw.json"
    else
      echo '{ logging: { level: "info" }, channels: { slack: { enabled: true } } }' > "$T/data/openclaw.json"
    fi
    (
      curl -sS -m 30 -o "$T/jobs/reload-$i.json" -X POST "http://127.0.0.1:$PORT/v1/rpc" \
        -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":1,"method":"config.reload","params":{}}' \
        || echo fail > "$T/jobs/reload-$i.bad"
      if [[ -f "$T/jobs/reload-$i.bad" ]] || ! check_json "$T/jobs/reload-$i.json" 'data.get("ok") is True'; then
        echo "reload#$i" > "$T/jobs/reload-$i.bad"
      fi
    ) &
    pids+=($!)
    (
      curl -sS -m 30 -o "$T/jobs/in-$i.json" -X POST "http://127.0.0.1:$PORT/api/channels/slack/inbound" \
        -H 'Content-Type: application/json' \
        -d "{\"type\":\"event_callback\",\"event\":{\"type\":\"message\",\"text\":\"msg-$i\",\"user\":\"U1\",\"channel\":\"C1\",\"channel_type\":\"channel\"}}" \
        || echo fail > "$T/jobs/in-$i.bad"
      if [[ -f "$T/jobs/in-$i.bad" ]] || ! check_json "$T/jobs/in-$i.json" 'data.get("ok") is True'; then
        echo "inbound#$i" > "$T/jobs/in-$i.bad"
      fi
    ) &
    pids+=($!)
    (
      curl -sS -m 30 -o "$T/jobs/tools-$i.json" -X POST "http://127.0.0.1:$PORT/v1/rpc" \
        -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":2,"method":"tools.list","params":{}}' \
        || echo fail > "$T/jobs/tools-$i.bad"
      if [[ -f "$T/jobs/tools-$i.bad" ]] || ! check_json "$T/jobs/tools-$i.json" 'data.get("ok") is True and isinstance(data.get("payload",{}).get("tools"), list)'; then
        echo "tools#$i" > "$T/jobs/tools-$i.bad"
      fi
    ) &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" || true; done
done

bads=( "$T"/jobs/*.bad )
if [[ -e "${bads[0]:-}" ]]; then
  echo "[e2e-lock] concurrent reload/inbound failed:" >&2
  for b in "$T"/jobs/*.bad; do
    echo " - $(cat "$b")" >&2
    base=${b%.bad}
    [[ -f "$base.json" ]] && python3 -c "print(open('$base.json').read()[:300])" >&2 || true
  done
  tail -40 "$T/gw.log" >&2
  exit 1
fi

curl -sf "http://127.0.0.1:$PORT/health" >/dev/null
sleep 1.1
echo '{ logging: { level: "warn" }, channels: { slack: { enabled: true } } }' > "$T/data/openclaw.json"
curl -sS -m 30 -o "$T/jobs/final.json" -X POST "http://127.0.0.1:$PORT/v1/rpc" \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"config.reload","params":{}}'
check_json "$T/jobs/final.json" 'data.get("ok") is True and data.get("payload",{}).get("reloaded") is True' \
  || { echo "[e2e-lock] final reload bad: $(cat "$T/jobs/final.json")" >&2; exit 1; }
curl -sS -m 30 -o "$T/jobs/final-tools.json" -X POST "http://127.0.0.1:$PORT/v1/rpc" \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools.list","params":{}}'
check_json "$T/jobs/final-tools.json" 'data.get("ok") is True and isinstance(data.get("payload",{}).get("tools"), list)'
curl -sf "http://127.0.0.1:$PORT/health" >/dev/null
echo "[e2e-lock] concurrent config.reload + inbound/tools.list under exclusive deps lock + worker pool: PASS"
