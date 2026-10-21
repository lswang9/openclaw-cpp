#!/usr/bin/env bash
# 1.124: promos, sandbox, dns setup, secrets apply/configure, backup sqlite/git, webhooks gmail, node install, connect,
# hooks relay, mcp serve/configure, acp client, generic `<cmd> --help`.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; CH=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$CH" ]] && kill $CH 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-cli-local5] FAIL: $*"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
mkdir -p "$T/home" "$T/bin"; unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_DOCS_URL
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_STATE_DIR="$T/state" OPENCLAW_CONFIG_PATH="$T/cfg.json" HOME="$T/home" OPENCLAW_GATEWAY_PORT=1
# shims: docker / gcloud / gog record their argv
for b in docker gcloud gog; do printf '#!/bin/sh\necho "%s $*" >> "%s/shim.log"\n' "$b" "$T" > "$T/bin/$b"; chmod +x "$T/bin/$b"; done
printf '#!/bin/sh\necho "docker $*" >> "%s/shim.log"\ncase "$1" in ps) echo %s;; esac\n' "$T" "'{\"ID\":\"c1\",\"Names\":\"oc-sbx-1\",\"Image\":\"debian\",\"State\":\"running\",\"Status\":\"Up\",\"Labels\":\"openclaw.sandbox=1,openclaw.agent=main,openclaw.session=s1\"}'" > "$T/bin/docker"
export PATH="$T/bin:$PATH"
# ---- help
"$BIN" qr --help | grep -q -- "--setup-code-only" || fail "qr --help"
# ---- promos against the fake ClawHub
CP=$(freeport); mkdir -p "$T/ch"; python3 "$ROOT/scripts/fake_clawhub.py" "$CP" "$T/ch" >"$T/ch.log" 2>&1 & CH=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$CP/api/v1/promotions" >/dev/null && break; sleep 0.1; done
export CLAWHUB_REGISTRY="http://127.0.0.1:$CP"
"$BIN" promos list | grep -q "promos claim free-tokens" || fail "promos list"
"$BIN" promos claim nope --api-key k 2>&1 | grep -q "not found" || fail "promos 404"
"$BIN" promos claim free-tokens --api-key sk-acme --set-default | grep -q "2 model(s) registered for acme" || fail "promos claim"
python3 -c "import json;c=json.load(open('$T/cfg.json'));assert c['models']['providers']['acme']['apiKey']=='sk-acme';assert c['agents']['defaults']['model']['primary']=='acme/fast-1';assert c['agents']['defaults']['models']['acme/fast-1']['alias']=='fast'" || fail "promos config"
# ---- sandbox (docker shim)
"$BIN" sandbox list | grep -q "oc-sbx-1  running  agent=main  session=s1" || fail "sandbox list"
"$BIN" sandbox recreate --agent main 2>&1 | grep -q "without --force" || fail "sandbox recreate guard"
"$BIN" sandbox recreate --agent main --force | grep -q "Removed 1" || fail "sandbox recreate"
grep -q "docker rm -f c1" "$T/shim.log" || fail "docker rm"
# ---- dns setup
"$BIN" dns setup --domain OC.Example.com | grep -q "Split DNS): oc.example.com" || fail "dns"
"$BIN" dns setup --domain oc.example.com --apply 2>&1 | grep -q "macOS only" || fail "dns apply"
# ---- secrets configure / apply
"$BIN" secrets configure | grep -q "models.providers.acme.apiKey" || fail "secrets scan"
"$BIN" secrets configure --apply --yes | grep -q "Moved 1 secret" || fail "secrets move"
python3 -c "import json;c=json.load(open('$T/cfg.json'));r=c['models']['providers']['acme']['apiKey'];assert r=={'source':'file','provider':'openclaw-file','id':'/models/providers/acme/apiKey'},r;s=json.load(open('$T/state/secrets/secrets.json'));assert s['models']['providers']['acme']['apiKey']=='sk-acme'" || fail "secrets layout"
[[ "$(stat -c %a "$T/state/secrets/secrets.json")" == 600 ]] || fail "secrets perms"
echo '{"version":1,"protocolVersion":1,"providerUpserts":{"x":{"source":"exec","command":"/bin/true"}},"targets":[]}' > "$T/plan.json"
"$BIN" secrets apply --from "$T/plan.json" 2>&1 | grep -q "allow-exec" || fail "secrets exec guard"
echo '{"version":1,"protocolVersion":1,"providerUpserts":{"envp":{"source":"env"}},"targets":[{"path":"gateway.auth.token","ref":{"source":"env","provider":"envp","id":"GW_TOK"}}]}' > "$T/plan.json"
"$BIN" secrets apply --from "$T/plan.json" --dry-run | grep -q "Dry run: 2" || fail "secrets dry"
"$BIN" secrets apply --from "$T/plan.json" >/dev/null && python3 -c "import json;c=json.load(open('$T/cfg.json'));assert c['gateway']['auth']['token']['id']=='GW_TOK'" || fail "secrets apply"
python3 - "$T/cfg.json" <<'PY'
import json,sys;p=sys.argv[1];c=json.load(open(p));del c['gateway'];c['secrets']['providers'].pop('envp');json.dump(c,open(p,'w'))
PY
# ---- backup sqlite / git
mkdir -p "$T/data/mem"; python3 -c "import sqlite3;d=sqlite3.connect('$T/data/mem/x.db');d.execute('create table t(a,b)');d.execute(\"insert into t values(1,'q''x')\");d.commit()"
"$BIN" backup sqlite create --out b1 | grep -q "Backed up 1 SQLite" || fail "sqlite create"
"$BIN" backup sqlite verify b1 | grep -q "Backup verified" || fail "sqlite verify"
python3 -c "import sqlite3;d=sqlite3.connect('$T/data/mem/x.db');d.execute('delete from t');d.commit()"
"$BIN" backup sqlite restore b1 2>&1 | grep -q "without --yes" || fail "sqlite restore guard"
"$BIN" backup sqlite restore b1 --yes | grep -q "Restored 1" || fail "sqlite restore"
[[ "$(python3 -c "import sqlite3;print(sqlite3.connect('$T/data/mem/x.db').execute('select b from t').fetchone()[0])")" == "q'x" ]] || fail "sqlite restored data"
"$BIN" backup git create | grep -q "Committed backup" || fail "git create"
"$BIN" backup git create | grep -q "No changes" || fail "git no-op"
"$BIN" backup git verify | grep -q "Backup verified" || fail "git verify"
python3 -c "import sqlite3;d=sqlite3.connect('$T/data/mem/x.db');d.execute('drop table t');d.commit()"
"$BIN" backup git restore --yes | grep -q "Restored 1" || fail "git restore"
[[ "$(python3 -c "import sqlite3;print(sqlite3.connect('$T/data/mem/x.db').execute('select count(*) from t').fetchone()[0])")" == 1 ]] || fail "git restored data"
# ---- webhooks gmail
"$BIN" webhooks setup 2>&1 | grep -q "webhooks gmail setup" || fail "webhooks hint"
"$BIN" webhooks gmail setup --account me@example.com --project p1 --push-endpoint https://h.test/gmail-pubsub | grep -q "Gmail hooks configured" || fail "gmail setup"
grep -q "gcloud pubsub subscriptions create gog-gmail-watch-push --project p1 --topic gog-gmail-watch --push-endpoint https://h.test/gmail-pubsub?token=" "$T/shim.log" || fail "gcloud sub"
grep -q "gog gmail watch start --account me@example.com --label INBOX --topic projects/p1/topics/gog-gmail-watch" "$T/shim.log" || fail "gog watch"
python3 -c "import json;c=json.load(open('$T/cfg.json'));g=c['hooks']['gmail'];assert g['account']=='me@example.com' and c['hooks']['enabled'] and 'gmail' in c['hooks']['presets']" || fail "gmail cfg"
# ---- node install
OPENCLAW_SKIP_SYSTEMCTL=1 "$BIN" node install --url http://gw.test:1 --display-name box | grep -q "systemctl skipped" || fail "node install"
grep -q "node run --url http://gw.test:1 --display-name box" "$T/home/.config/systemd/user/openclaw-node.service" 2>/dev/null || grep -q "node run '--url' 'http://gw.test:1'" "$T/home/.config/systemd/user/openclaw-node.service" || fail "unit content"
"$BIN" node install 2>&1 | grep -q -- "--force" || fail "node install guard"
# ---- hooks relay with no gateway
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | "$BIN" hooks relay --pre-tool-use-unavailable=deny >/dev/null 2>&1 && fail "relay deny exit 0" || { [[ $? -eq 2 ]] || fail "relay deny"; }
echo '{"hook_event_name":"PreToolUse"}' | "$BIN" hooks relay >/dev/null 2>&1 || fail "relay fail-open"
# ---- gateway-backed: connect, mcp serve
PORT=$(freeport); export OPENCLAW_GATEWAY_PORT=$PORT
"$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
rpc() { curl -s "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
SC=$(rpc device.pair.setupCode '{"device_name":"n1"}')
CODE=$(python3 -c 'import json,sys;print(json.loads(sys.argv[1])["payload"]["setupCode"])' "$SC") || fail "setupCode: $SC"
LID=$(python3 -c 'import json,sys;print(json.loads(sys.argv[1])["payload"]["link"]["id"])' "$SC")
( sleep 1; rpc pairing.link.approve "{\"id\":\"$LID\"}" >/dev/null ) &
OPENCLAW_SKIP_SYSTEMCTL=1 "$BIN" connect "$CODE" --service --force --timeout 20 2>"$T/conn.err" | grep -q "Node service" || fail "connect: $(cat $T/conn.err)"
grep -q "ocd_" "$T/home/.config/systemd/user/openclaw-node.service" || fail "connect token in unit"
"$BIN" connect "not-a-code" 2>&1 | grep -q "Invalid setup code" || fail "connect bad code"
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}' '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"conversations_list","arguments":{}}}' \
  | "$BIN" mcp serve > "$T/mcp.out" || fail "mcp serve"
python3 - "$T/mcp.out" <<'PY' || fail "mcp serve output: $(cat $T/mcp.out)"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
assert L[0]["result"]["serverInfo"]["name"]=="openclaw"
names=[t["name"] for t in L[1]["result"]["tools"]]; assert len(names)==9 and "permissions_respond" in names, names
assert not L[2]["result"].get("isError"), L[2]
PY
kill $GW; wait $GW 2>/dev/null || true; GW=""
# ---- acp client against the local ACP server (mock provider)
OUT=$(timeout 60 "$BIN" acp client --prompt "hello there" --approve deny 2>"$T/acp.err") || fail "acp client: $(tail -3 $T/acp.err)"
[[ -n "$OUT" ]] || fail "acp client empty output: $(tail -3 $T/acp.err)"
# ---- mcp configure (non-interactive)
"$BIN" mcp configure --name cfgd --command "$BIN" --arg mcp-mock-server </dev/null | grep -q "Connected" || fail "mcp configure"
echo "[e2e-cli-local5] OK"
