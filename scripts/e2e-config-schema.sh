#!/usr/bin/env bash
# 1.111: full upstream config schema (generated from upstream computeBaseConfigSchemaResponse), lookup with
# uiHints, schema validation, and the upstream agents.entries shape.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-config-schema] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home"
echo '{"agents":{"entries":{"helper":{"name":"Helper","model":"mock/x"}}}}' > "$T/cfg.json"
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/cfg.json" HOME="$T/home"
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" CFG="$T/cfg.json"
python3 - <<'P' || fail "config schema"
import json, os, urllib.request
URL = os.environ["URL"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=30).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
s = rpc("config.schema", {})["payload"]
assert s["version"] == "2026.10.5" and len(s["schema"]["properties"]) >= 40 and len(s["uiHints"]) > 1000
l = rpc("config.schema.lookup", {"path": "gateway.port"})["payload"]; assert l["hint"]["label"] == "Gateway Port" and l["schema"]["type"] == "integer"
root = rpc("config.schema.lookup", {"path": "."})["payload"]; assert any(c["key"] == "channels" for c in root["children"])
assert rpc("config.schema.lookup", {"path": "agents.entries.helper.model"})["ok"]
assert not rpc("config.schema.lookup", {"path": "nope.nothing"})["ok"]
v = rpc("config.schema.validate", {"config": {"gateway": {"port": "x", "bogus": 1}}})["payload"]
codes = {(i["path"], i["code"]) for i in v["issues"]}
assert ("gateway.port", "invalid_type") in codes and ("gateway.bogus", "unrecognized_key") in codes and not v["valid"] and v["schemaPaths"] > 3000
assert rpc("config.schema.validate", {"config": {"gateway": {"port": 18789}}})["payload"]["valid"]
ag = rpc("agents.list", {})["payload"]; assert "helper" in json.dumps(ag), ag
assert rpc("agents.create", {"id": "writer", "name": "Writer"})["ok"]
assert rpc("agents.update", {"id": "helper", "model": "mock/y"})["payload"]["agent"]["model"] == "mock/y"
doc = json.load(open(os.environ["CFG"]))
assert doc["agents"]["entries"]["helper"]["model"] == "mock/y" and doc["agents"]["entries"]["writer"]["name"] == "Writer", doc
assert "list" not in doc["agents"] and "__fromEntries" not in json.dumps(doc), doc
assert rpc("agents.delete", {"id": "writer"})["ok"]
assert "writer" not in json.load(open(os.environ["CFG"]))["agents"]["entries"]
r = rpc("agents.route", {"channel": "telegram", "peerId": "7"})["payload"]; assert r["agentId"] in ("main", "helper")
assert rpc("config.schema.validate", {})["payload"]["valid"], rpc("config.schema.validate", {})
print("ok")
P
echo "[e2e-config-schema] upstream schema (3238 paths, uiHints), lookup, validation, agents.entries: PASS"
