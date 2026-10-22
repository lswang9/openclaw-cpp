#!/usr/bin/env bash
# 1.134: the 13 remaining upstream core tools (computer, dashboard, decision_evaluate, gateway, github_identity_status,
# mobile_ui, personal_instructions, plugins, portal, progress_card, screen, skill_workshop, terminal) over tools.invoke,
# with a simulated-UI node for computer/mobile_ui and the mock provider as decision model.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; ND=""
trap '[[ -n "$ND" ]] && kill $ND 2>/dev/null; [[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-core-tools] FAIL: $*"; echo "T=$T"; tail -8 "$T/gw.log"; exit 1; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_DOCS_URL
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home" "$T/nodehome"
cat > "$T/script.json" <<'J'
[{"content": "{\"q1\": {\"probabilityTrue\": 0.8}, \"q2\": {\"distribution\": {\"a\": 3, \"b\": 1}}, \"q3\": {\"distribution\": [0, 1, 1]}}"}]
J
cat > "$T/cfg.json" <<'J'
{"agents": {"defaults": {"decisionModel": "mock/decider"}}, "tools": {"exec": {"security": "full"}}}
J
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_STATE_DIR="$T/state"
export OPENCLAW_CONFIG_PATH="$T/cfg.json" HOME="$T/home"
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
HOME="$T/nodehome" OPENCLAW_NODE_SIMULATE_UI=1 "$BIN" node run --url "http://127.0.0.1:$PORT" --node-id ui-1 >"$T/node.log" 2>&1 & ND=$!
for i in $(seq 1 50); do grep -q "registered as ui-1" "$T/node.log" && break; sleep 0.1; done
grep -q "registered as ui-1" "$T/node.log" || fail "node registration"
URL="http://127.0.0.1:$PORT/v1/rpc" T="$T" python3 - <<'PY' || fail "assertions"
import json, os, urllib.request
URL, T = os.environ["URL"], os.environ["T"]
def rpc(m, p):
    return json.loads(urllib.request.urlopen(urllib.request.Request(URL, json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}).encode(), {"Content-Type": "application/json"}), timeout=60).read())
SK = "agent:main:main"
def tool(name, args, sk=SK):
    j = rpc("tools.invoke", {"name": name, "args": args, "sessionKey": sk})
    assert j.get("ok") is True, (name, j)
    r = j["payload"]["result"]
    if isinstance(r, dict) and "content" in r and isinstance(r["content"], list): r = r["content"][0]["text"]
    return json.loads(r) if isinstance(r, str) else r
names = [t["function"]["name"] for t in rpc("tools.list", {})["payload"]["openai"]]
for n in ["computer", "dashboard", "decision_evaluate", "gateway", "github_identity_status", "mobile_ui", "personal_instructions", "plugins", "portal", "progress_card", "screen", "skill_workshop", "terminal"]:
    assert n in names, ("missing tool", n)
# gateway
g = tool("gateway", {"action": "config.get", "path": "agents.defaults.decisionModel"}); assert g["ok"] and g["result"]["config"] == "mock/decider", g
assert tool("gateway", {"action": "config.schema.lookup", "path": "agents"})["result"]["path"] == "agents"
assert tool("gateway", {"action": "config.get", "path": "nope.x"})["ok"] is False
u = tool("gateway", {"action": "update.run"}, sk="agent:main:telegram:dm:42"); assert u["code"] == "owner_required", u
# plugins
pl = tool("plugins", {"action": "list", "query": "sample"}); assert pl["matching"] >= 1 and all("sample" in p["id"] for p in pl["plugins"]), pl
assert tool("plugins", {"action": "install", "source": "official", "pluginId": "x", "version": "1"})["ok"] is False
# progress_card
pc = tool("progress_card", {"markdown": "Working", "plan": [{"title": "a", "status": "completed"}, {"title": "b", "status": "in_progress"}]})
assert pc["steps"] == {"completed": 1, "total": 2} and pc["revision"] == 1, pc
assert rpc("progressCard.get", {"sessionKey": SK, "agentId": "main"})["payload"]["card"]["markdown"] == "Working"
assert tool("progress_card", {"plan": [{"title": "x", "status": "bogus"}]})["ok"] is False
# screen
assert tool("screen", {"action": "terminal_show", "dock": "bottom"})["ok"] is True
assert tool("screen", {"action": "bad"})["ok"] is False
# dashboard
d = tool("dashboard", {"action": "tab_create", "tabId": "ops", "title": "Ops"}); assert [t["tabId"] for t in d["tabs"]] == ["main", "ops"], d
d = tool("dashboard", {"action": "widget_put", "name": "rep", "pluginKind": "session:report", "props": {"title": "R"}, "tabId": "ops"})
assert d["tabs"][1]["widgets"][0]["name"] == "rep", d
d = tool("dashboard", {"action": "widget_move", "name": "rep", "tabId": "main"}); assert d["tabs"][0]["widgets"][0]["name"] == "rep" and not d["tabs"][1]["widgets"], d
d = tool("dashboard", {"action": "tabs_reorder", "tabIds": ["ops", "main"]}); assert d["tabs"][0]["tabId"] == "ops"
assert tool("dashboard", {"action": "widget_put", "name": "x", "pluginKind": "bad"})["ok"] is False
assert tool("dashboard", {"action": "set_default_presentation", "presentation": "expanded"})["defaultPresentation"] == "expanded"
assert tool("dashboard", {"action": "read"})["defaultPresentation"] == "expanded"
# portal
po = tool("portal", {"action": "open", "port": 5173, "title": "dev"}); assert "Portal route allocated" in po["text"], po
assert len(tool("portal", {"action": "list"})["portals"]) == 1
assert tool("portal", {"action": "open", "port": 1}, sk="agent:main:telegram:dm:42")["ok"] is False
# terminal: operator opens one for this session, agent lists/reads/inputs/closes
op = rpc("terminal.open", {"sessionKey": SK, "agentId": "main", "cwd": T})["payload"]; sid = op["sessionId"]
ls = tool("terminal", {"action": "list"}); assert [s["sessionId"] for s in ls["sessions"]] == [sid], ls
assert tool("terminal", {"action": "input", "sessionId": sid, "data": "echo core-tools-$((40+2))\n"})["ok"] is True
import time
for _ in range(50):
    txt = tool("terminal", {"action": "read", "sessionId": sid})["text"]
    if "core-tools-42" in txt: break
    time.sleep(0.1)
assert "core-tools-42" in txt, txt[-300:]
assert tool("terminal", {"action": "list"}, sk="agent:main:other")["sessions"] == []
assert tool("terminal", {"action": "close", "sessionId": sid})["ok"] is True
# computer / mobile_ui via the simulated node
c = tool("computer", {"action": "screenshot"}); assert c["nodeId"] == "ui-1" and c["dataUrl"].startswith("data:image/png;base64,iVBOR"), c
c = tool("computer", {"action": "left_click", "coordinate": [10, 20]}); assert c["echo"]["coordinate"] == [10, 20], c
o = tool("mobile_ui", {"action": "observe"}); assert o["snapshotId"] == "snap-1" and o["nodes"][0]["ref"] == "n1", o
a = tool("mobile_ui", {"action": "act", "snapshotId": "snap-1", "uiAction": {"type": "activate", "ref": "n1"}}); assert a["performed"]["ref"] == "n1", a
assert tool("mobile_ui", {"action": "act", "snapshotId": "old", "uiAction": {"type": "tap", "ref": "n1"}})["ok"] is False
# decision_evaluate via the mock decision model
de = tool("decision_evaluate", {"state": {"x": 1}, "questions": {"q1": {"type": "boolean"}, "q2": {"type": "choice", "criteria": {"a": "A", "b": "B"}}, "q3": {"type": "score", "criteria": ["lo", "mid", "hi"]}}})
assert de["status"] == "ok" and de["result"]["q1"]["probabilityTrue"] == 0.8 and abs(de["result"]["q2"]["distribution"]["a"] - 0.75) < 1e-9, de
assert abs(de["result"]["q3"]["expectedPosition"] - 0.75) < 1e-9 and de["provenance"]["rubricVersion"].startswith("decision-v1-"), de
assert tool("decision_evaluate", {"state": 1, "questions": {}})["ok"] is False
# personal_instructions
pi = tool("personal_instructions", {"action": "get"}); assert pi["exists"] is False and pi["hash"] is None, pi
s1 = tool("personal_instructions", {"action": "set", "content": "Prefers metric.", "expectedHash": None}); assert len(s1["hash"]) == 64, s1
assert tool("personal_instructions", {"action": "set", "content": "x", "expectedHash": None})["code"] == "hash_mismatch"
pi = tool("personal_instructions", {"action": "get"}); assert pi["content"] == "Prefers metric." and pi["hash"] == s1["hash"], pi
# github_identity_status: no credentials offline -> reconnect guidance
gh = tool("github_identity_status", {}); assert "nextAction" in gh or gh.get("ok") is False, gh
# skill_workshop
sw = "---\nname: deploy-check\ndescription: Verify a deploy\n---\n# Steps\n1. curl health\n"
assert tool("skill_workshop", {"action": "create", "name": "deploy-check", "content": sw})["change"] == "created"
assert tool("skill_workshop", {"action": "create", "name": "bad", "content": "no frontmatter"})["ok"] is False
p = tool("skill_workshop", {"action": "patch", "name": "deploy-check", "old_text": "curl health", "new_text": "curl /health and /ready", "reason": "ready probe"}); assert p["change"] == "patched", p
assert "curl /health and /ready" in tool("skill_workshop", {"action": "view", "name": "deploy-check"})["content"]
assert tool("skill_workshop", {"action": "write_file", "name": "deploy-check", "file_path": "scripts/check.sh", "content": "echo ok"})["ok"] is True
assert tool("skill_workshop", {"action": "write_file", "name": "deploy-check", "file_path": "../evil", "content": "x"})["ok"] is False
r = tool("skill_workshop", {"action": "restore", "name": "deploy-check"}); assert "restored" in r["change"], r
r = tool("skill_workshop", {"action": "restore", "name": "deploy-check"}); assert "restored" in r["change"], r
assert "curl health" in tool("skill_workshop", {"action": "view", "name": "deploy-check"})["content"]
assert tool("skill_workshop", {"action": "archive", "name": "deploy-check", "reason": "merged"})["change"] == "archived"
assert "Archived" in tool("skill_workshop", {"action": "list"})["text"]
assert "restored from archive" in tool("skill_workshop", {"action": "restore", "name": "deploy-check"})["change"]
assert os.path.isfile(T + "/state/agents/main/agent/workshop-skills/deploy-check/SKILL.md")
print("ok")
PY
echo "[e2e-core-tools] OK"
