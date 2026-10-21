#!/usr/bin/env bash
# acpx: openclaw-cpp as ACP *client* driving external agents over stdio. (1) a fake agent exercises streamed chunks,
# tool_call/tool_call_update, session/request_permission (deny vs allow_once), fs/read_text_file (cwd-confined,
# /etc/passwd refused), fs/write_text_file (only with --fs-write), audio passthrough vs local STT fallback;
# (2) real interop with openclaw-cpp's own `acp-stdio` agent, incl. an audio block transcribed agent-side.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d)
fail() { echo "[e2e-acpx] FAIL: $*"; exit 1; }
mkdir -p "$T/ws"; export OPENCLAW_CONFIG_PATH="$T/openclaw.json"
cat > "$OPENCLAW_CONFIG_PATH" <<JSON
{"acp": {"agents": {"fake": {"command": ["python3", "$ROOT/scripts/fake-acp-agent.py"]}}}}
JSON
echo "original" > "$T/ws/notes.txt"
R=$("$BIN" acp run --agent fake --cwd "$T/ws" "fix the notes") || fail "deny run: $R"
J() { printf '%s' "$R" | python3 -c "import json,sys; r=json.load(sys.stdin); $1"; }
J 'assert r["ok"] and r["stopReason"]=="end_turn", r
assert "got[text:fix the notes]" in r["text"] and "read=original" in r["text"] and "escape=blocked" in r["text"], r["text"]
assert "write=" not in r["text"], r["text"]
assert r["permissions"][0]["optionId"]=="r1", r["permissions"]
assert r["tool_calls"][0]["status"]=="failed" and r["tool_calls"][0]["title"]=="Edit notes.txt", r["tool_calls"]
assert [f["ok"] for f in r["fs"]]==[True, False], r["fs"]' || fail "deny assertions"
grep -q original "$T/ws/notes.txt" || fail "file changed under deny"
R=$("$BIN" acp run --agent fake --cwd "$T/ws" --permission allow_once "fix") || fail "allow run"
J 'assert "write=fs writes disabled" in r["text"], r["text"]' || fail "write without --fs-write must be refused"
R=$("$BIN" acp run --agent fake --cwd "$T/ws" --permission allow_once --fs-write "fix") || fail "allow+write run"
J 'assert "write=ok" in r["text"] and r["tool_calls"][0]["status"]=="completed", r' || fail "allow+write"
grep -q "edited by agent" "$T/ws/notes.txt" || fail "write not applied"
# audio: fake STT server
python3 "$ROOT/scripts/fake-voice-media.py" > "$T/fp" 2>/dev/null & FP=$!
trap 'kill $FP 2>/dev/null || true' EXIT
for _ in $(seq 50); do [[ -s "$T/fp" ]] && break; sleep 0.1; done
export OPENCLAW_STT_PROVIDER=openai OPENCLAW_STT_URL="http://127.0.0.1:$(cat "$T/fp")/v1/audio/transcriptions" OPENCLAW_STT_API_KEY=k
python3 -c 'import sys; open(sys.argv[1],"wb").write(b"OggS"+b"\0"*200+b"tg")' "$T/v.ogg"
R=$("$BIN" acp run --agent fake --cwd "$T/ws" --audio "$T/v.ogg" "listen") || fail "audio fallback run"
J 'assert "text:[Audio] Transcript: hello from voice tg" in r["text"] and r["audio"][0]["mode"]=="transcribed_locally", r' || fail "local STT fallback"
R=$(FAKE_ACP_AUDIO=1 "$BIN" acp run --agent fake --cwd "$T/ws" --audio "$T/v.ogg" "listen") || fail "audio passthrough run"
J 'assert "audio:audio/ogg" in r["text"] and r["audio"][0]["mode"]=="passthrough", r' || fail "audio passthrough"
# real interop with openclaw-cpp acp-stdio (mock provider); audio transcribed by the agent
R=$("$BIN" acp run --agent openclaw --cwd "$T/ws" --updates --audio "$T/v.ogg" "hello") || fail "openclaw interop: $R"
J 'assert r["ok"] and r["stopReason"]=="end_turn" and r["text"] and r["audio"][0]["mode"]=="passthrough", r
assert "hello from voice tg" in json.dumps(r["updates"]), "agent-side transcript not in updates"' || fail "openclaw interop"
# terminal capability (ACP terminal/*): off → agent sees no terminal; on → create/wait/output/kill/release, cwd-confined
R=$("$BIN" acp run --agent fake --cwd "$T/ws" "run tests") || fail "no-terminal run"
J 'assert "no-terminal" in r["text"], r["text"]' || fail "terminal must be off by default"
R=$("$BIN" acp run --agent fake --cwd "$T/ws" --terminal "run tests") || fail "terminal run: $R"
J 'o=json.loads(r["text"])
assert o["exit"]=={"exitCode":3,"signal":None}, o
assert "out-bar" in o["output"] and "err" in o["output"], o
assert o["cwd_escape"] is True and o["killed"]["signal"], o
assert len(r["terminals"])==2, r["terminals"]' || fail "terminal assertions: $R"
# RPC callers need allowTerminal in config
R=$("$BIN" acp run --agent fake --cwd "$T/ws" "turn one" --then "turn two" --then "turn three") || fail "multi-turn: $R"
J 'assert len(r["turns"])==3 and r["turns"][2]["text"].endswith("#3:turn three"), r["turns"]' || fail "multi-turn"
# session/load across processes (agent persists its sessions)
export FAKE_ACP_STATE="$T/acp-state.json"
R=$("$BIN" acp run --agent fake --cwd "$T/ws" "turn a") || fail "state run"
SID=$(J 'print(r["sessionId"])')
R=$("$BIN" acp run --agent fake --cwd "$T/ws" --session "$SID" "turn b") || fail "load run: $R"
J 'assert r["loaded"] and r["text"].endswith("#2:turn b"), r' || fail "session/load resume"
unset FAKE_ACP_STATE
# agent requiring login: auth_required without --auth; authenticate + retry with it
R=$(FAKE_ACP_AUTH=1 "$BIN" acp run --agent fake --cwd "$T/ws" "turn x" || true)
J 'assert r.get("auth_required") and r["authMethods"][0]["id"]=="fake-login", r' || fail "auth_required: $R"
R=$(FAKE_ACP_AUTH=1 "$BIN" acp run --agent fake --cwd "$T/ws" --auth auto "turn x") || fail "auth run: $R"
J 'assert r["ok"] and r["authenticated"]=="fake-login", r' || fail "authenticate"
R=$(FAKE_ACP_AUTH=1 "$BIN" acp run --agent fake --cwd "$T/ws" --auth nope "turn x" || true)
J 'assert not r["ok"] and "not advertised" in r["error"], r' || fail "unknown auth method"
R=$("$BIN" acp run --agent nosuch "x" || true)
printf '%s' "$R" | grep -q "unknown ACP agent" || fail "unknown agent"
echo "[e2e-acpx] PASS"
