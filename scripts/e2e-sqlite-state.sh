#!/usr/bin/env bash
# 1.144/1.145: gateway state documents in SQLite (OPENCLAW_STATE_STORE=sqlite, or implied by session.store=sqlite):
# cron + canvas/a2ui (+ auth profiles when written). Legacy JSON imported once; files left untouched.
set -eu
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-sqlite-state] FAIL: $*"; tail -8 "$T/gw.log" 2>/dev/null; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_SESSION_STORE OPENCLAW_STATE_STORE
PORT=$(freeport)
start() { env $1 OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" \
  OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!
  for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done; }
stop() { kill $GW; wait $GW 2>/dev/null || true; GW=""; }
rpc() { curl -s -m 30 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
P() { python3 -c "import json,sys;d=json.loads(sys.stdin.read());r=d.get('result',d);r=r.get('payload',r);$1"; }
echo '{}' > "$T/cfg.json"; mkdir -p "$T/d" "$T/st/agents/main/agent" "$T/s/canvas"
echo '{"jobs":[{"id":"legacy","cron":"0 * * * *","payload":"old","enabled":true}]}' > "$T/d/cron.json"
cp "$T/d/cron.json" "$T/legacy.json"
echo '{"version":1,"active_id":"","docs":[]}' > "$T/s/canvas/index.json"
echo '{"version":1,"profiles":{"mock:default":{"type":"api_key","provider":"mock","key":"legacy-key"}}}' > "$T/st/agents/main/agent/auth-profiles.json"
cp "$T/st/agents/main/agent/auth-profiles.json" "$T/legacy-profiles.json"

start OPENCLAW_STATE_STORE=sqlite
rpc cron.list '{}' | P 'assert "legacy" in json.dumps(r["jobs"]), r' || fail "legacy import"
rpc cron.add '{"id":"fresh","cron":"5 * * * *","text":"new"}' >/dev/null
rpc cron.list '{}' | P 'assert "fresh" in json.dumps(r["jobs"]), r' || fail "cron.add"
rpc canvas.create '{"title":"sq"}' | P 'assert "id" in (r.get("document") or r.get("doc") or r), r' || fail "canvas.create"
rpc canvas.a2ui.push '{"text":"hello state db"}' >/dev/null
rpc canvas.a2ui.state '{}' | P 'assert "surfaces" in r, r' || fail "a2ui state"
stop

cmp -s "$T/d/cron.json" "$T/legacy.json" || fail "legacy cron.json was rewritten"
cmp -s "$T/st/agents/main/agent/auth-profiles.json" "$T/legacy-profiles.json" || fail "legacy auth-profiles rewritten"

python3 - "$T/d/openclaw-state.sqlite" "$T/s/canvas/openclaw-state.sqlite" <<'PY' || fail "state db contents"
import sqlite3,sys,json,os,stat
c=sqlite3.connect(sys.argv[1])
row=c.execute("select value_json, migrated_from from state_documents where store_key='cron.json'").fetchone()
ids={j["id"] for j in json.loads(row[0])["jobs"]}
assert ids=={"legacy","fresh"}, ids
assert row[1] and row[1].endswith("cron.json"), row
assert stat.S_IMODE(os.stat(sys.argv[1]).st_mode)==0o600
c2=sqlite3.connect(sys.argv[2])
keys={r[0] for r in c2.execute("select store_key from state_documents")}
assert "index.json" in keys and "a2ui-state.json" in keys, keys
assert any(k.startswith("canvas-") and k.endswith(".json") for k in keys), keys
idx=json.loads(c2.execute("select value_json from state_documents where store_key='index.json'").fetchone()[0])
assert idx.get("docs"), idx
PY

OPENCLAW_STATE_STORE=sqlite OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_CONFIG_PATH="$T/cfg.json" \
  "$BIN" models auth set --provider mock --api-key sk-state-test --agent main >"$T/auth.log" 2>&1 || true
if [[ -f "$T/st/agents/main/agent/openclaw-state.sqlite" ]]; then
  python3 - "$T/st/agents/main/agent/openclaw-state.sqlite" <<'PY' || fail "auth-profiles row"
import sqlite3,sys,json
c=sqlite3.connect(sys.argv[1])
row=c.execute("select value_json from state_documents where store_key='auth-profiles.json'").fetchone()
assert row, "missing auth-profiles.json"
doc=json.loads(row[0])
assert "profiles" in doc and doc["profiles"], doc
PY
  cmp -s "$T/st/agents/main/agent/auth-profiles.json" "$T/legacy-profiles.json" || fail "auth-profiles file mutated after sqlite write"
fi

start OPENCLAW_STATE_STORE=sqlite
rpc cron.list '{}' | P 's=json.dumps(r["jobs"]);assert "fresh" in s and "legacy" in s, r' || fail "restart persistence"
rpc canvas.list '{}' | P 'docs=r.get("docs") or r.get("canvases") or r.get("documents") or []; assert docs, r' || fail "canvas restart"
stop

echo '{"session":{"store":"sqlite"}}' > "$T/cfg.json"
start OPENCLAW_X=1
rpc cron.list '{}' | P 's=json.dumps(r["jobs"]);assert "fresh" in s, r' || fail "implied by session.store"
stop
start OPENCLAW_STATE_STORE=json
rpc cron.list '{}' | P 's=json.dumps(r["jobs"]);assert "fresh" not in s and "legacy" in s, r' || fail "json opt-out"
stop
echo "[e2e-sqlite-state] OK"
