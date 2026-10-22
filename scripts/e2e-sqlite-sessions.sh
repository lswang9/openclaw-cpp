#!/usr/bin/env bash
# 1.131: session storage in the upstream agent database (openclaw-agent.sqlite, schema v25): one-time import from
# the JSON/JSONL store, transcript persistence across restarts, reset windows kept as history, JSON files untouched.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-sqlite] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_SESSION_STORE
echo '[{"content":"one"},{"content":"two"},{"content":"three"},{"content":"four"},{"content":"five"}]' > "$T/script.json"
PORT=$(freeport)
start() { env $1 OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" \
  OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!
  for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done; }
stop() { kill $GW; wait $GW 2>/dev/null || true; GW=""; }
rpc() { curl -s -m 30 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
P() { python3 -c "import json,sys;d=json.loads(sys.stdin.read());r=d.get('result',d);r=r.get('payload',r);$1"; }
echo '{}' > "$T/cfg.json"
start OPENCLAW_X=1
rpc chat.send '{"sessionKey":"alpha","message":"hi a"}' | P 'assert r["text"]=="one", r' || fail "json chat a"
rpc chat.send '{"sessionKey":"beta","message":"hi b"}' | P 'assert r["text"]=="two", r' || fail "json chat b"
stop
ls "$T/s"/*.jsonl >/dev/null || fail "json store wrote no transcripts"
# Switch to SQLite via config (session.store) → one-time import
echo '{"session":{"store":"sqlite"}}' > "$T/cfg.json"
start OPENCLAW_X=1
rpc status "{}" | P 'x=r["sessionStore"];assert x["backend"]=="sqlite" and x["migration"]["importedSessions"]==2, x' || fail "migration status: $(curl -s http://127.0.0.1:$PORT/v1/status | head -c 600)"
rpc chat.history '{"sessionKey":"alpha"}' | P 'c=[m["content"] for m in r["messages"]];assert "one" in c and any("hi a" in x for x in c), c' || fail "imported history"
rpc chat.send '{"sessionKey":"alpha","message":"again"}' | P 'assert r["text"]=="one", r' || fail "sqlite chat"
rpc sessions.reset '{"key":"beta"}' >/dev/null
rpc chat.send '{"sessionKey":"beta","message":"fresh"}' | P 'assert r["text"]=="two", r' || fail "chat after reset"
stop
DB="$T/s/openclaw-agent.sqlite"; [[ -f "$DB" ]] || fail "no $DB: $(find $T -name '*.sqlite')"
python3 - "$DB" "$T/s" <<'PY' || fail "database contents"
import sqlite3,sys,json,os
c=sqlite3.connect(sys.argv[1])
assert c.execute("pragma user_version").fetchone()[0]==25
assert c.execute("select role,schema_version from schema_meta where meta_key='primary'").fetchone()==("agent",25)
tables={r[0] for r in c.execute("select name from sqlite_master where type='table'")}
for t in ["session_nodes","session_windows","transcript_events","conversations","memory_index_chunks","standing_intents","session_pending_inputs"]: assert t in tables, t
assert len(tables)>=57, len(tables)
keys=dict(c.execute("select session_key,current_session_id from session_nodes"))
assert set(keys)>={"alpha","beta"}, keys
ev=[json.loads(r[0]) for r in c.execute("select event_json from transcript_events where session_id=? order by seq",(keys["alpha"],))]
assert ev[0]["type"]=="session" and [e["message"]["content"] for e in ev[1:]][-1]=="one" and "again" in ev[-2]["message"]["content"], ev
w=c.execute("select count(*), sum(reason='reset') from session_windows where session_key='beta'").fetchone()
assert w==(2,1), w
old=c.execute("select count(*) from transcript_events t join session_windows w on w.session_id=t.session_id where w.session_key='beta' and w.reason='initial'").fetchone()[0]
assert old>=3, old   # previous window's transcript kept as history
assert c.execute("pragma foreign_key_check").fetchall()==[]
assert os.path.exists(os.path.join(sys.argv[2],"sessions.json")), "json index left in place"
PY
# Restart: data persists, no second import
start OPENCLAW_X=1
rpc status "{}" | P 'x=r["sessionStore"];assert "migration" not in x and x["sqlite"]["session_nodes"]>=2, x' || fail "restart status"
rpc chat.history '{"sessionKey":"beta"}' | P 'c=[m["content"] for m in r["messages"]];assert c[-1]=="two" and any("fresh" in x for x in c) and not any("hi b" in x for x in c), c' || fail "beta after reset+restart"
stop
echo "[e2e-sqlite] OK"
