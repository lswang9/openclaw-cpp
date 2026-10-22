#!/usr/bin/env bash
# Feishu long connection with binary pbbp2 frames: endpoint discovery (AppID/AppSecret), ping with service_id,
# pong ClientConfig applied (PingInterval), multi-part event reassembly (out-of-order parts), ack frame (code 200,
# biz_rt), gzip card.action.trigger answered with base64 card JSON, server drop → reconnect per ClientConfig, and the agent reply posted via im/v1/messages with the tenant token. Fake server only.
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d)
python3 "$ROOT/scripts/fake-feishu-ws.py" "$T/report.json" > "$T/fp" 2>"$T/fake.log" & FP=$!
GW=
trap 'kill $FP ${GW:-} 2>/dev/null || true; _reap' EXIT
for _ in $(seq 50); do [[ -s "$T/fp" ]] && break; sleep 0.1; done
fail() { echo "[e2e-feishu-ws] FAIL: $*"; cat "$T/fake.log"; tail -5 "$T/gw.log"; [[ -f "$T/report.json" ]] && cat "$T/report.json"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
export OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_CONFIG_PATH="$T/cfg.json"
echo '{}' > "$OPENCLAW_CONFIG_PATH"
FEISHU_LINK_PREVIEW_FETCH=1 OPENCLAW_MEDIA_ALLOW_PRIVATE=1 FEISHU_APP_ID=cli_a FEISHU_APP_SECRET=sec FEISHU_API_BASE="http://127.0.0.1:$(cat "$T/fp")" \
  "$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & GW=$!
for _ in $(seq 80); do curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break; sleep 0.1; done
curl -s -X POST "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d '{"method":"feishu.longconn.start"}' >/dev/null
for _ in $(seq 100); do
  [[ -s "$T/report.json" ]] && python3 -c 'import json,sys;r=json.load(open(sys.argv[1]));sys.exit(0 if len(r["pings"])>=2 and len(r["acks"])>=3 and r["replies"] and r["connections"]>=2 else 1)' "$T/report.json" && break
  sleep 0.1
done
python3 - "$T/report.json" <<'PY' || fail "assertions"
import json, sys
r = json.load(open(sys.argv[1]))
assert r["endpoint_body"] == {"AppID": "cli_a", "AppSecret": "sec"} and r["endpoint_locale"] == "zh", r["endpoint_body"]
assert r["pings"][0]["service"] == 33, r["pings"]
assert len(r["pings"]) >= 2 and r["pings"][1]["t"] - r["pings"][0]["t"] < 2.5, "pong PingInterval=1 not applied"
a = r["acks"][0]
assert a["headers"]["message_id"] == "M1" and a["payload"]["code"] == 200 and "biz_rt" in a["headers"] and a["seq"] == 7, a
rep = r["replies"][0]
assert rep["auth"] == "Bearer t-123" and "receive_id_type=chat_id" in rep["path"] and rep["body"]["receive_id"] == "oc_1", rep
assert json.loads(rep["body"]["content"])["text"], rep
assert not r["errors"], r["errors"]
import base64
card = [x for x in r["acks"] if x["headers"].get("message_id") == "C1"][0]
assert card["payload"]["code"] == 200, card
resp = json.loads(base64.b64decode(card["payload"]["data"]))
assert resp["toast"]["type"] == "info" and resp["toast"]["content"], resp
assert resp["card"]["data"]["schema"] == "2.0", resp
pv = [x for x in r["acks"] if x["headers"].get("message_id") == "P1"][0]
pr = json.loads(base64.b64decode(pv["payload"]["data"]))
assert pr["inline"]["title"] == "Fake Page Title" and pr["inline"]["i18n_title"]["en_us"] == "Fake Page Title", pr
assert r["connections"] >= 2 and r["endpoint_calls"] >= 2, ("no reconnect", r["connections"], r["endpoint_calls"])
PY
curl -s -X POST "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d '{"method":"feishu.longconn.status"}' | tee "$T/st.json" | grep -Eq '"binary_events": *1' || fail "status binary_events"
grep -Eq '"card_callbacks": *1' "$T/st.json" && grep -Eq '"gzip_frames": *1' "$T/st.json" && grep -Eq '"reconnects": *[1-9]' "$T/st.json" || { cat "$T/st.json"; fail "status card/gzip/reconnect"; }
echo "[e2e-feishu-ws] PASS"
