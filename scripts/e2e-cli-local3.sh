#!/usr/bin/env bash
# 1.122: local CLI batch 3 — directory (fake Discord/Slack/Telegram), proxy (debug proxy capture/run/query/validate),
# database (SQLite preflight via the built-in reader, ownership), update (wizard/cleanup/recover/migration-plan/admit/adopt-immutable).
set -eu
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); FK=""; UP=""; PX=""
trap '[[ -n "$FK" ]] && kill $FK 2>/dev/null; [[ -n "$UP" ]] && kill $UP 2>/dev/null; [[ -n "$PX" ]] && kill $PX 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-cli-local3] FAIL: $*"; exit 1; }
python3 -u "$ROOT/scripts/fake-chat-apis.py" >"$T/fk.port" 2>"$T/fk.log" & FK=$!
mkdir -p "$T/www"; echo "hello-upstream" > "$T/www/x.txt"
UPORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
(cd "$T/www" && exec python3 -u -m http.server "$UPORT" --bind 127.0.0.1 >"$T/up.log" 2>&1) & UP=$!
for i in $(seq 1 50); do [[ -s "$T/fk.port" ]] && curl -s "http://127.0.0.1:$UPORT/x.txt" >/dev/null && break; sleep 0.1; done
FP=$(cat "$T/fk.port"); mkdir -p "$T/home"
export HOME="$T/home" OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_STATE_DIR="$T/state" OPENCLAW_SESSION_DIR="$T/sess"
unset HTTP_PROXY HTTPS_PROXY http_proxy https_proxy ALL_PROXY all_proxy NO_PROXY no_proxy DISCORD_BOT_TOKEN SLACK_BOT_TOKEN TELEGRAM_BOT_TOKEN 2>/dev/null || true
oc() { "$BIN" "$@"; }
# ---------------- directory
oc directory self 2>&1 | grep -q "No directory-capable channel" || fail "directory without channels"
cat > "$T/cfg.json" <<C
{"channels": {"discord": {"token": "dtok", "apiBase": "http://127.0.0.1:$FP/discord"}, "slack": {"botToken": "stok", "apiBase": "http://127.0.0.1:$FP/slack"},
              "telegram": {"botToken": "ttok", "apiBase": "http://127.0.0.1:$FP/telegram"}}}
C
oc directory self 2>&1 | grep -q "Multiple channels configured" || fail "ambiguous channel"
oc directory self --channel discord --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["id"]=="b1" and j["handle"]=="clawbot", j' || fail "discord self"
oc directory groups list --channel discord --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert [g["name"] for g in j]==["#general","#dev-chat"], j' || fail "discord groups"
oc directory groups list --channel discord --query dev | grep -q "c2  #dev-chat" || fail "discord groups query (human)"
oc directory groups members --channel discord --group-id c1 --limit 2 --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert [m["name"] for m in j]==["Alice A","Bobby"], j' || fail "discord members"
oc directory peers list --channel discord --query ali --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert [m["id"] for m in j]==["u1"], j' || fail "discord peers search"
oc directory groups members --channel discord 2>&1 | grep -q "Missing --group-id" || fail "members needs group id"
oc directory peers list --channel slack --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert [m["handle"] for m in j]==["alice","carol"], j' || fail "slack peers (paged, deleted skipped)"
oc directory groups members --channel slack --group-id C1 --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert [m["name"] for m in j]==["Alice Slack","Carol C"], j' || fail "slack members"
oc directory groups members --channel slack --group-id CX 2>&1 | grep -q "channel_not_found" || fail "slack error surfaced"
oc directory self --channel slack | grep -q "clawbot  (UBOT)" || fail "slack self"
oc directory self --channel telegram --json | grep -q '"handle": "claw_bot"' || fail "telegram self"
oc directory peers list --channel telegram 2>&1 | grep -q "does not support peer listing" || fail "telegram unsupported"
oc directory peers list --channel discord --limit 0 2>&1 | grep -q "positive integer" || fail "limit validation"
# ---------------- proxy
U="http://127.0.0.1:$UPORT/x.txt"
out=$(oc proxy run -- sh -c "curl -s $U; curl -s $U; curl -s $U; curl -s -o /dev/null http://127.0.0.1:1/dead || true" 2>"$T/run.err")
[[ "$(echo "$out" | grep -c hello-upstream)" == 3 ]] || fail "proxy run child output: $out"
grep -q "requests captured" "$T/run.err" || fail "proxy run summary"
SID=$(oc proxy sessions --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j[0]["requests"]==4 and j[0]["endedAt"], j; print(j[0]["id"])') || fail "proxy sessions"
oc proxy query --preset double-sends --session "$SID" --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["rows"] and j["rows"][0]["count"]==3, j' || fail "double-sends"
oc proxy query --preset retry-storms --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["rows"][0]["count"]==3, j' || fail "retry-storms"
oc proxy query --preset missing-ack --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert len(j["rows"])==1 and "127.0.0.1:1" in j["rows"][0]["url"], j' || fail "missing-ack"
oc proxy query --preset bogus 2>&1 | grep -q "unknown preset" || fail "bad preset"
BLOB=$(python3 -c 'import json,os; ev=[json.loads(l) for l in open(os.environ["OPENCLAW_STATE_DIR"]+"/debug-proxy/events.jsonl")]; print(ev[0]["responseBlob"])')
[[ "$(oc proxy blob --id "$BLOB")" == "hello-upstream" ]] || fail "proxy blob"
oc proxy blob --id ../../etc/passwd 2>&1 | grep -q "invalid blob id" || fail "blob id validation"
oc proxy coverage --json | grep -q '"transport": "https"' || fail "coverage"
PPORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
"$BIN" proxy start --port "$PPORT" >"$T/px.log" 2>&1 & PX=$!
for i in $(seq 1 50); do grep -q "listening" "$T/px.log" && break; sleep 0.1; done
oc proxy validate --proxy-url "http://127.0.0.1:$PPORT" --allowed-url "$U" --denied-url "http://127.0.0.1:1/" --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["ok"] and [c["ok"] for c in j["checks"]]==[True,True,True], j' || fail "proxy validate ok"
if oc proxy validate --proxy-url "http://127.0.0.1:$PPORT" --denied-url "$U" >/dev/null; then fail "validate should fail when a denied URL is reachable"; fi
HTTPS_PROXY="http://u:secret@127.0.0.1:$PPORT" oc proxy validate --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["source"]=="env HTTPS_PROXY" and "secret" not in j["proxyUrl"], j' || fail "validate env + redaction"
kill $PX; wait $PX 2>/dev/null || true; PX=""
grep -q "debug proxy stopped" "$T/px.log" || fail "proxy start graceful stop"
oc proxy purge | grep -q Purged || fail "purge"; [[ "$(oc proxy sessions --json)" == "[]" ]] || fail "purge cleared sessions"
# ---------------- database
python3 - "$T" <<'PY'
import sqlite3, sys, json
T = sys.argv[1]
db = sqlite3.connect(T + "/state.sqlite"); db.execute("PRAGMA user_version=20"); db.execute("CREATE TABLE config_machine_state (state_key TEXT PRIMARY KEY, value_json TEXT NOT NULL)")
db.execute("INSERT INTO config_machine_state VALUES ('state-ownership', ?)", (json.dumps({"managerId": "fleet-1", "mode": "external"}),))
for i in range(300): db.execute("CREATE TABLE t%03d (id INTEGER PRIMARY KEY, body TEXT, %s TEXT)" % (i, "c" * 40))
db.commit(); db.close()
db = sqlite3.connect(T + "/old.sqlite"); db.execute("PRAGMA user_version=17"); db.execute("CREATE TABLE sessions (id TEXT)"); db.commit(); db.close()
db = sqlite3.connect(T + "/new.sqlite"); db.execute("PRAGMA user_version=99"); db.execute("CREATE TABLE x (id TEXT)"); db.commit(); db.close()
db = sqlite3.connect(T + "/agent.sqlite"); db.execute("PRAGMA user_version=25")
db.execute("CREATE TABLE schema_meta (meta_key TEXT PRIMARY KEY, role TEXT, schema_version INTEGER, agent_id TEXT, " + ", ".join("pad%d TEXT" % i for i in range(5)) + ")")
db.execute("INSERT INTO schema_meta (meta_key, role, schema_version, agent_id) VALUES ('primary', 'agent', 25, 'ops')"); db.commit(); db.close()
open(T + "/junk.sqlite", "w").write("not a database")
PY
oc database preflight "$T/state.sqlite" --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["status"]=="exact" and j["foundVersion"]==20 and j["ownership"]["managerId"]=="fleet-1" and len(j["tables"])==301, (j["status"], len(j["tables"]))' || fail "preflight exact (multi-page schema)"
oc database preflight "$T/old.sqlite" | grep -q "migration-required (found 17, target 20)" || fail "preflight migration-required"
if oc database preflight "$T/new.sqlite" >/dev/null; then fail "newer schema must exit 1"; fi
oc database preflight "$T/junk.sqlite" --json | grep -q '"status": "indeterminate"' || fail "junk indeterminate"
touch "$T/old.sqlite-wal"; oc database preflight "$T/old.sqlite" --json | grep -q "no sidecars" || fail "sidecar check"
oc database preflight-agent "$T/agent.sqlite" --agent-id ops --json | grep -q '"status": "exact"' || fail "agent exact"
oc database preflight-agent "$T/agent.sqlite" --agent-id main --json | grep -q "belongs to agent ops" || fail "agent owner mismatch"
oc database preflight-agent "$T/agent.sqlite" --agent-id OPS --json | grep -q "canonical agent ID" || fail "agent id canonical"
oc database ownership status | grep -q "not externally owned" || fail "ownership unowned"
oc database ownership claim --manager fleet-1 --json | grep -q '"managerId": "fleet-1"' || fail "claim"
oc database ownership claim --manager other 2>&1 | grep -q "already externally owned by fleet-1" || fail "claim conflict"
oc database ownership --json | grep -q '"status": "external"' || fail "ownership external"
# ---------------- update
printf 'beta\nn\ny\n' | oc update wizard >/dev/null && python3 -c 'import json,os; j=json.load(open(os.environ["OPENCLAW_CONFIG_PATH"])); assert j["update"]=={"channel":"beta","checkOnStart":False}, j["update"]' || fail "update wizard"
printf 'dev\n\nn\n' | oc update wizard >/dev/null && fail "wizard decline should exit 1" || true
mkdir -p "$T/state/update/staging/1.2.3" "$T/state/update/downloads"; echo x > "$T/state/update/staging/1.2.3/bin"; echo yy > "$T/state/update/downloads/pkg.tgz"
oc update cleanup --dry-run --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert len(j["items"])==2 and not j["removed"], j' || fail "cleanup dry-run"
oc update cleanup --yes --json | grep -q '"removed": true' && [[ ! -e "$T/state/update/downloads/pkg.tgz" ]] || fail "cleanup --yes"
oc update recover --json | grep -q '"status": "clean"' || fail "recover clean"
echo new > "$T/target-bin"; echo old > "$T/prev-bin"
echo "{\"phase\":\"swap\",\"target\":\"$T/target-bin\",\"previous\":\"$T/prev-bin\",\"fromVersion\":\"1.0\",\"toVersion\":\"1.1\"}" > "$T/state/update/journal.json"
echo '{"toVersion":"1.2"}' > "$T/ctx.json"
oc update admit --context "$(cat "$T/ctx.json")" | grep -q "another update is in progress" || fail "admit blocked by journal"
oc update recover | grep -q "Rolled back" && [[ "$(cat "$T/target-bin")" == old ]] || fail "recover rollback"
oc update admit --context '{"toVersion":"1.2","managerId":"fleet-1"}' | grep -q '"admitted":true' || fail "admit ok"
oc update admit --context '{"toVersion":"1.2"}' | grep -q "externally owned by fleet-1" || fail "admit blocked by ownership"
echo '{"agents":{"defaults":{}},"bogusTop":1}' > "$T/snap.json"
oc update migration-plan --snapshot-config "$T/snap.json" --snapshot-state "$T/nostate" --json | python3 -c 'import json,sys; j=json.load(sys.stdin); assert any("bogusTop" in s["detail"] for s in j["steps"]) and not j["blocking"], j' || fail "migration-plan"
mkdir -p "$T/imm"; oc update adopt-immutable --root "$T/imm" --enable-activation 2>&1 | grep -q "previous-updater-stopped" || fail "adopt needs stop"
oc update adopt-immutable --root "$T/imm" --service openclaw.service --json | grep -q '"service": "openclaw.service"' || fail "adopt"
rm -f "$OPENCLAW_DATA_DIR/state-ownership.json"; oc update admit --context '{}' | grep -q "managed immutably" || fail "admit immutable"
echo "[e2e-cli-local3] directory (discord/slack/telegram), proxy (run/sessions/query/blob/validate/start/purge), database preflight+ownership, update wizard/cleanup/recover/migration-plan/admit/adopt-immutable: PASS"
