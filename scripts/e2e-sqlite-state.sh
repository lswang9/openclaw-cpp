#!/usr/bin/env bash
# 1.144: gateway state documents in SQLite (OPENCLAW_STATE_STORE=sqlite, or implied by session.store=sqlite):
# legacy JSON imported once on first read, writes land in <dir>/openclaw-state.sqlite, legacy files untouched,
# data survives restart.
set -eu
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-sqlite-state] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_SESSION_STORE OPENCLAW_STATE_STORE
PORT=$(freeport)
start() { env $1 OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" \
  OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!
  for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done; }
stop() { kill $GW; wait $GW 2>/dev/null || true; GW=""; }
rpc() { curl -s -m 30 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
P() { python3 -c "import json,sys;d=json.loads(sys.stdin.read());r=d.get('result',d);r=r.get('payload',r);$1"; }
echo '{}' > "$T/cfg.json"; mkdir -p "$T/d"
echo '{"jobs":[{"id":"legacy","cron":"0 * * * *","payload":"old","enabled":true}]}' > "$T/d/cron.json"
cp "$T/d/cron.json" "$T/legacy.json"
start OPENCLAW_STATE_STORE=sqlite
rpc cron.list '{}' | P 'assert "legacy" in json.dumps(r["jobs"]), r' || fail "legacy import"
rpc cron.add '{"id":"fresh","cron":"5 * * * *","text":"new"}' >/dev/null; rpc cron.list "{}" | P 'assert "fresh" in json.dumps(r["jobs"]), r' || fail "cron.add"
stop
cmp -s "$T/d/cron.json" "$T/legacy.json" || fail "legacy cron.json was rewritten"
python3 - "$T/d/openclaw-state.sqlite" <<'PY' || fail "state db contents"
import sqlite3,sys,json,os,stat
c=sqlite3.connect(sys.argv[1])
row=c.execute("select value_json, migrated_from from state_documents where store_key='cron.json'").fetchone()
ids=[j["id"] for j in json.loads(row[0])["jobs"]]
assert ids==["legacy","fresh"] or set(ids)=={"legacy","fresh"}, ids
assert row[1] and row[1].endswith("cron.json"), row
assert stat.S_IMODE(os.stat(sys.argv[1]).st_mode)==0o600
PY
start OPENCLAW_STATE_STORE=sqlite
rpc cron.list '{}' | P 's=json.dumps(r["jobs"]);assert "fresh" in s and "legacy" in s, r' || fail "restart persistence"
stop
# session.store=sqlite implies the state DB; OPENCLAW_STATE_STORE=json opts back out to files
echo '{"session":{"store":"sqlite"}}' > "$T/cfg.json"
start OPENCLAW_X=1
rpc cron.list '{}' | P 's=json.dumps(r["jobs"]);assert "fresh" in s, r' || fail "implied by session.store"
stop
start OPENCLAW_STATE_STORE=json
rpc cron.list '{}' | P 's=json.dumps(r["jobs"]);assert "fresh" not in s and "legacy" in s, r' || fail "json opt-out"
stop
echo "[e2e-sqlite-state] OK"
