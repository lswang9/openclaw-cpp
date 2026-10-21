#!/usr/bin/env bash
# Upstream-shaped local commands: completion, setup --baseline, onboard (non-interactive + scripted prompts),
# configure --section, migrate claude (plan/apply/conflicts/backup), tui (line mode against a live gateway),
# crestodian alias. All offline in a temp HOME.
set -eu  # no pipefail: `cmd | grep -q` may SIGPIPE the producer
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; rm -rf "$T"' EXIT
export HOME="$T/home" OPENCLAW_STATE_DIR="$T/home/.openclaw" OPENCLAW_CONFIG_PATH="$T/home/.openclaw/openclaw.json"
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN CLAUDE_CONFIG_DIR || true
mkdir -p "$HOME"
fail() { echo "[e2e-local] FAIL: $*"; exit 1; }
# completion
"$BIN" completion -s bash > "$T/c.bash"; bash -n "$T/c.bash" || fail "bash completion syntax"
grep -q "complete -F _openclaw_cpp_completion openclaw-cpp" "$T/c.bash" || fail "bash complete line"
grep -q "devices) COMPREPLY" "$T/c.bash" && grep -q "approve" "$T/c.bash" || fail "devices subcommands"
"$BIN" completion --shell zsh | grep -q "^#compdef openclaw-cpp" || fail zsh
"$BIN" completion -s fish | grep -q "__fish_seen_subcommand_from migrate' -a 'apply'" || fail fish
"$BIN" completion -s powershell | grep -q "Register-ArgumentCompleter" || fail pwsh
"$BIN" completion -s tcsh 2>&1 | grep -q "Allowed choices are zsh, bash, powershell, fish" || fail "bad shell"
"$BIN" completion --bogus 2>&1 | grep -q "unknown option '--bogus'" || fail "unknown option"
"$BIN" completion --write-state 2>/dev/null; for e in bash zsh fish ps1; do [[ -s "$OPENCLAW_STATE_DIR/completions/openclaw-cpp.$e" ]] || fail "write-state $e"; done
SHELL=/bin/bash "$BIN" completion --install --yes 2>/dev/null; grep -q "# OpenClaw Completion" "$HOME/.bashrc" || fail install
SHELL=/bin/bash "$BIN" completion --install --yes 2>&1 | grep -q "already installed" || fail "install idempotent"
echo "[e2e-local] completion bash/zsh/fish/powershell + write-state + install: PASS"
# setup --baseline
J=$("$BIN" setup --baseline --workspace "$T/ws1" --json)
python3 - "$J" "$T" <<'PY' || fail "setup baseline"
import json,sys,os
j=json.loads(sys.argv[1]); t=sys.argv[2]
assert j["ok"] and j["created"] and "agents.defaults.workspace" in j["changed"] and "gateway.mode" in j["changed"], j
c=json.load(open(t+"/home/.openclaw/openclaw.json"))
assert c["agents"]["defaults"]["workspace"]==t+"/ws1" and c["gateway"]["mode"]=="local"
for f in ["AGENTS.md","SOUL.md","TOOLS.md","IDENTITY.md","USER.md","HEARTBEAT.md","BOOTSTRAP.md"]: assert os.path.exists(t+"/ws1/"+f), f
assert "SOUL.md - Who You Are" in open(t+"/ws1/SOUL.md").read()
assert not open(t+"/ws1/SOUL.md").read().startswith("---")
PY
"$BIN" setup --baseline | grep -q "^Config OK:" || fail "setup idempotent"
"$BIN" setup --baseline --message hi 2>&1 | grep -q "cannot be combined with: --message" || fail "baseline combo"
"$BIN" crestodian --json 2>&1 | grep -q "deprecated" || fail crestodian
echo "[e2e-local] setup --baseline (+templates, idempotent, crestodian alias): PASS"
# onboard non-interactive
rm -f "$OPENCLAW_CONFIG_PATH"
"$BIN" onboard --non-interactive --openai-api-key sk-x 2>&1 | grep -q "requires explicit risk" || fail "accept-risk gate"
J=$("$BIN" onboard --non-interactive --accept-risk --workspace "$T/ws2" --openai-api-key sk-test --gateway-port 19999 --gateway-bind lan --gateway-token tok123 --skip-health --agent-name "Ada Bot" --team --json)
python3 - "$J" "$T" <<'PY' || fail "onboard ni"
import json,sys
j=json.loads(sys.argv[1]); t=sys.argv[2]
assert j["ok"] and j["model"]=="openai/gpt-4.1" and j["gateway"]=={"port":19999,"bind":"lan","auth":"token"}, j
c=json.load(open(t+"/home/.openclaw/openclaw.json"))
assert c["models"]["providers"]["openai"]["apiKey"]=="sk-test" and c["gateway"]["auth"]["token"]=="tok123"
assert [a["id"] for a in c["agents"]["list"]]==["ada-bot","researcher","writer","reviewer"], c["agents"]["list"]
PY
J=$("$BIN" onboard --non-interactive --accept-risk --auth-choice custom-api-key --custom-base-url http://127.0.0.1:11434/v1 --custom-model-id llama3 --skip-health --json)
python3 -c "import json,sys;j=json.loads(sys.argv[1]);assert j['model']=='custom-127-0-0-1-11434-v1/llama3',j" "$J" || fail "custom provider"
"$BIN" onboard --non-interactive --accept-risk --gateway-bind moon --skip-health 2>&1 | grep -q "Invalid --gateway-bind" || fail "bind validation"
# interactive onboard via scripted stdin: accept, advanced flow, local, workspace, deepseek, key, model, port, bind, auth, token, channels done, web off
rm -f "$OPENCLAW_CONFIG_PATH"
printf 'y\nadvanced\nlocal\n%s\ndeepseek\nsk-ds\n\n18800\nloopback\ntoken\n\ntelegram\n123:abc\npairing\ndone\noff\n' "$T/ws3" | "$BIN" onboard --skip-health 2>"$T/onb.err" || { cat "$T/onb.err"; fail "interactive onboard"; }
python3 - "$T" <<'PY' || { cat "$T/onb.err"; fail "interactive onboard config"; }
import json,sys; t=sys.argv[1]
c=json.load(open(t+"/home/.openclaw/openclaw.json"))
assert c["agents"]["defaults"]["workspace"]==t+"/ws3" and c["agents"]["defaults"]["model"]["primary"]=="deepseek/deepseek-chat", c
assert c["gateway"]["port"]==18800 and len(c["gateway"]["auth"]["token"])==48
assert c["channels"]["telegram"]["botToken"]=="123:abc" and c["channels"]["telegram"]["dmPolicy"]=="pairing"
assert c["tools"]["web"]["search"]["enabled"] is False
PY
echo "[e2e-local] onboard non-interactive (+risk gate, custom provider, team) + scripted interactive: PASS"
# configure --section
printf 'brave\nBSA-key\ny\n' | "$BIN" configure --section web 2>/dev/null
printf 'gateway\n18801\nlan\npassword\npw1\ncontinue\n' | "$BIN" configure 2>/dev/null
python3 - "$T" <<'PY' || fail configure
import json,sys; t=sys.argv[1]
c=json.load(open(t+"/home/.openclaw/openclaw.json"))
assert c["tools"]["web"]["search"]=={"enabled":True,"provider":"brave","apiKey":"BSA-key"} and c["tools"]["web"]["fetch"]["enabled"] is True, c["tools"]
assert c["gateway"]["port"]==18801 and c["gateway"]["auth"]=={"mode":"password","token":c["gateway"]["auth"].get("token"),"password":"pw1"} or c["gateway"]["auth"]["password"]=="pw1"
assert c["channels"]["telegram"]["botToken"]=="123:abc"
PY
"$BIN" configure --section nope 2>&1 | grep -q "Invalid --section: nope" || fail "configure section validation"
echo "[e2e-local] configure --section web + menu gateway: PASS"
# migrate claude
mkdir -p "$HOME/.claude/skills/pdf-tool" "$HOME/.claude/commands/git" "$T/proj/.claude"
echo "user prefs: be terse" > "$HOME/.claude/CLAUDE.md"
printf -- '---\nname: pdf-tool\ndescription: pdf\n---\nUse pdftotext.\n' > "$HOME/.claude/skills/pdf-tool/SKILL.md"
echo "Write a conventional commit message" > "$HOME/.claude/commands/git/commit.md"
echo '{"hooks":{"PreToolUse":[]},"permissions":{"allow":["Bash"]}}' > "$HOME/.claude/settings.json"
echo '{"mcpServers":{"fs":{"command":"npx","args":["-y","@mcp/fs"],"env":{"TOKEN":"s3cret"}}}}' > "$HOME/.claude.json"
echo "Project rules: run tests" > "$T/proj/CLAUDE.md"
echo '{"mcpServers":{"gh":{"command":"gh-mcp"}}}' > "$T/proj/.mcp.json"
mkdir -p "$T/ws3/skills/pdf-tool"
cd "$T/proj"
"$BIN" migrate list | grep -q "^claude" || fail "migrate list"
P=$("$BIN" migrate plan claude --json)
python3 - "$P" <<'PY' || fail "migrate plan"
import json,sys
p=json.loads(sys.argv[1]); st={i["id"]:i for i in p["items"]}
assert st["workspace:CLAUDE.md"]["target"].endswith("/AGENTS.md") and st["memory:user-CLAUDE.md"]["target"].endswith("/USER.md")
assert st["skill:user:pdf-tool"]["status"]=="conflict"
assert st["command:user:git/commit.md"]["target"].endswith("skills/claude-command-git-commit")
assert st["mcp:fs"]["sensitive"] and "env omitted (TOKEN)" in st["mcp:fs"]["reason"] and st["mcp:gh"]["target"]=="mcp.servers.gh"
assert any(i["kind"]=="manual" for i in p["items"]) and p["summary"]["conflicts"]==1
PY
"$BIN" migrate apply claude --no-backup 2>&1 | grep -q "re-run with --force" || fail "no-backup gate"
A=$("$BIN" migrate apply claude --yes --json --backup-output "$T/bk")
python3 - "$A" "$T" <<'PY' || fail "migrate apply"
import json,sys,os
a=json.loads(sys.argv[1]); t=sys.argv[2]
assert a["summary"]["applied"]==5 and a["summary"]["conflicts"]==1 and os.path.exists(a["backup"]), a["summary"]
c=json.load(open(t+"/home/.openclaw/openclaw.json"))
assert "env" not in c["mcp"]["servers"]["fs"] and c["mcp"]["servers"]["fs"]["command"]=="npx" and c["mcp"]["servers"]["gh"]["command"]=="gh-mcp"
assert "Project rules: run tests" in open(t+"/ws3/AGENTS.md").read() and "be terse" in open(t+"/ws3/USER.md").read()
s=open(t+"/ws3/skills/claude-command-git-commit/SKILL.md").read(); assert s.startswith("---\nname: claude-command-git-commit")
assert not os.path.exists(t+"/ws3/skills/pdf-tool/SKILL.md")
PY
A2=$("$BIN" migrate apply claude --yes --json --overwrite --no-backup --force)
python3 - "$A2" "$T" <<'PY' || fail "migrate re-apply"
import json,sys,os,glob
a=json.loads(sys.argv[1]); t=sys.argv[2]; st={i["id"]:i["status"] for i in a["items"]}
assert st["workspace:CLAUDE.md"]=="skipped" and st["skill:user:pdf-tool"]=="applied" and st["mcp:gh"]=="skipped", st
assert os.path.exists(t+"/ws3/skills/pdf-tool/SKILL.md") and glob.glob(t+"/ws3/skills/pdf-tool.bak-*")
assert open(t+"/ws3/AGENTS.md").read().count("Project rules")==1
PY
"$BIN" migrate hermes 2>&1 | grep -q "not implemented" || fail hermes
cd "$ROOT"
echo "[e2e-local] migrate claude list/plan/apply (+conflicts, overwrite backups, secret redaction, backup archive): PASS"
# tui against a live gateway (mock provider)
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
OUT=$(printf 'second line\n/history\n/exit\n' | OPENCLAW_GATEWAY_URL="http://127.0.0.1:$PORT" "$BIN" tui --session e2e --message "hello tui" 2>"$T/tui.err") || { cat "$T/tui.err"; fail "tui run"; }
[[ $(grep -c "^agent: " <<<"$OUT") -ge 3 ]] || { echo "$OUT"; cat "$T/tui.err"; fail "tui replies"; }
grep -q "^you: .*hello tui" <<<"$OUT" || { echo "$OUT"; fail "tui history"; }
grep -q "session agent:main:e2e" "$T/tui.err" || fail "tui session key"
OPENCLAW_GATEWAY_URL="http://127.0.0.1:1" "$BIN" tui </dev/null 2>&1 | grep -q "gateway not reachable" || fail "tui unreachable"
echo "[e2e-local] tui line mode (send, history, slash commands, unreachable): PASS"
echo "[e2e-local] PASS"
