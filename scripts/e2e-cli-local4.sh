#!/usr/bin/env bash
# 1.123: top-level guard (upstream names never reach the chat model), mcp list/show/add/set/unset/tools/status/probe/doctor/login/logout,
# reset, uninstall, docs search, clawbot/acp/resume aliases.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FO=""; DS=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FO" ]] && kill $FO 2>/dev/null; [[ -n "$DS" ]] && kill $DS 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true; _reap' EXIT
fail() { echo "[e2e-cli-local4] FAIL: $*"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; exit 1; }
mkdir -p "$T/home"; unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_DOCS_URL
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/cfg.json" HOME="$T/home" OPENCLAW_GATEWAY_PORT=1
# ---- guard: upstream names with no C++ implementation must not fall through to chat
out=$("$BIN" connect </dev/null 2>&1) && fail "connect exit 0"; grep -q "devices join-code" <<<"$out" || fail "connect: $out"; grep -q mock-default <<<"$out" && fail "connect fell through to chat"
"$BIN" mcp --help | grep -q "login <name>" || fail "mcp help"
"$BIN" reset --help | grep -q -- "--scope" || fail "reset help"
# ---- mcp (stdio, against the built-in mock MCP server)
"$BIN" mcp list | grep -q "No OpenClaw-managed MCP servers" || fail "empty list"
"$BIN" mcp add m1 --command "$BIN" --arg mcp-mock-server --env A=1 | grep -q "Connected: 1 tool" || fail "add probe"
"$BIN" mcp add bad --command /nonexistent/x >/dev/null 2>&1 && fail "bad add saved"
"$BIN" mcp add bad --command /nonexistent/x --no-probe >/dev/null || fail "no-probe add"
"$BIN" mcp set j '{"url":"ftp://x"}' 2>&1 | grep -q "http(s) URL" || fail "set validation"
"$BIN" mcp set j '{"command":"true","url":"http://x"}' 2>&1 | grep -q "exactly one" || fail "set exclusivity"
"$BIN" mcp show m1 | grep -q '"A": "1"' || fail "show env"
"$BIN" mcp tools m1 --include mock_echo,other | grep -q '"mock_echo","other"' || fail "tools"
"$BIN" mcp probe m1 --json | python3 -c 'import json,sys;d=json.load(sys.stdin);assert d["servers"][0]["probe"]["tools"]==["mock_echo"],d' || fail "probe"
"$BIN" mcp doctor >/dev/null 2>&1 && fail "doctor should flag bad"
"$BIN" mcp unset bad >/dev/null && "$BIN" mcp doctor | grep -q "m1  stdio  ok (1 tools)" || fail "doctor ok"
"$BIN" mcp unset ghost 2>&1 | grep -q "No MCP server" || fail "unset ghost"
"$BIN" mcp login m1 2>&1 | grep -q "gateway" || fail "login w/o gateway"
# ---- mcp login/logout through the gateway OAuth flow (fake auth server)
python3 -u "$ROOT/scripts/fake-oauth.py" >"$T/fo.port" 2>"$T/fo.log" & FO=$!
for i in $(seq 1 50); do [[ -s "$T/fo.port" ]] && break; sleep 0.1; done
OP=$(cat "$T/fo.port"); PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
"$BIN" mcp set demo "{\"url\":\"http://127.0.0.1:$OP/mcp\"}" >/dev/null
export OPENCLAW_GATEWAY_PORT=$PORT
"$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
"$BIN" mcp probe demo 2>&1 | grep -q "mcp login demo" || fail "probe 401 hint"
"$BIN" mcp login demo --json >"$T/login.json" 2>&1 || fail "login start: $(cat $T/login.json)"
URL=$(python3 -c "import json;print(json.load(open('$T/login.json'))['url'])")
LOC=$(curl -s -o /dev/null -w '%{redirect_url}' "$URL"); [[ "$LOC" == *code=* ]] || fail "authorize: $LOC"
CODE=$(python3 -c 'import sys,urllib.parse as u;print(u.parse_qs(u.urlparse(sys.argv[1]).query)["code"][0])' "$LOC")
"$BIN" mcp login demo --code "$CODE" | grep -q 'Authorized MCP server "demo" (verified)' || fail "login code"
"$BIN" mcp status | grep -q "demo  streamable-http  oauth" || "$BIN" mcp status | grep -q "demo" || fail "status"
"$BIN" mcp probe demo --json | python3 -c 'import json,sys;d=json.load(sys.stdin);p=d["servers"][0]["probe"];assert p["ok"] and p["authorized"],d' || fail "authorized probe"
"$BIN" mcp login demo --code x 2>&1 | grep -q "No pending login" || fail "stale code"
"$BIN" mcp logout demo | grep -q "Cleared OAuth" || fail "logout"
"$BIN" mcp probe demo 2>&1 | grep -q "401" || fail "probe after logout"
kill $GW; wait $GW 2>/dev/null || true; GW=""; export OPENCLAW_GATEWAY_PORT=1
# ---- docs search against a local llms.txt
mkdir -p "$T/docs"; printf '# OpenClaw\n- [Gateway configuration](/gateway/config.md): ports, auth and bind\n- [MCP servers](/cli/mcp.md): add and authorize MCP servers\n- [Channels](https://x.test/channels): Discord and Slack\n' > "$T/docs/llms.txt"
DP=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
(cd "$T/docs" && exec python3 -m http.server "$DP" --bind 127.0.0.1 >/dev/null 2>&1) & DS=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$DP/llms.txt" >/dev/null && break; sleep 0.1; done
export OPENCLAW_DOCS_URL="http://127.0.0.1:$DP"
"$BIN" docs mcp authorize --json | python3 -c 'import json,sys;d=json.load(sys.stdin);r=d["results"];assert r[0]["title"]=="MCP servers" and r[0]["url"].endswith("/cli/mcp.md"),d' || fail "docs"
"$BIN" docs zzzqqq | grep -q "No docs matched" || fail "docs miss"
"$BIN" docs gateway --limit 1 | grep -c "^  http" | grep -q '^1$' || fail "docs limit"
# ---- aliases
"$BIN" clawbot qr --json 2>&1 | grep -qv "not implemented" || fail "clawbot qr"
"$BIN" resume work 2>&1 | grep -q "tui: gateway not reachable" || fail "resume -> tui"
"$BIN" resume --handoff x 2>&1 | grep -q "not supported" || fail "resume handoff"
printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1}}\n' | timeout 10 "$BIN" acp | grep -q '"id":1' || fail "acp stdio"
# ---- reset / uninstall
echo '{}' > "$T/cfg.json"; mkdir -p "$T/home/.openclaw/agents/main" "$T/sess"; touch "$T/sess/s.jsonl"
"$BIN" reset --non-interactive 2>&1 | grep -q "without --yes" || fail "reset guard"
"$BIN" reset --dry-run | grep -q "$T/sess" || fail "reset dry"
"$BIN" reset --yes --json | python3 -c 'import json,sys;d=json.load(sys.stdin);assert len(d["moved"])==3,d' || fail "reset"
[[ ! -e "$T/cfg.json" && ! -e "$T/sess" && -n "$(ls "$T/home/.openclaw-trash")" ]] || fail "reset moved"
mkdir -p "$T/home/.config/systemd/user" "$T/home/.openclaw" "$T/data"; touch "$T/home/.config/systemd/user/openclaw-gateway.service" "$T/home/.config/systemd/user/other.service"
"$BIN" uninstall --non-interactive 2>&1 | grep -q "Choose what" || fail "uninstall needs scope"
"$BIN" uninstall --all --dry-run | grep -q "\[service\].*openclaw-gateway.service" || fail "uninstall dry"
"$BIN" uninstall --all --yes --json >/dev/null || fail "uninstall"
[[ ! -e "$T/home/.config/systemd/user/openclaw-gateway.service" && -e "$T/home/.config/systemd/user/other.service" && ! -e "$T/home/.openclaw" && ! -e "$T/data" ]] || fail "uninstall effects"
echo "[e2e-cli-local4] guard + mcp (stdio probe, validation, tools filters, OAuth login/logout via gateway) + docs + reset + uninstall + clawbot/acp/resume aliases: PASS"
