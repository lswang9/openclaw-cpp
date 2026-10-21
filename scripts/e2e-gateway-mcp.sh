#!/usr/bin/env bash
# 1.120: gateway MCP server (attach.grant → POST /mcp), board widgets, controlUi previews/PR checks (fake GitHub + local HTML),
# conversations, catalog (fake ClawHub), session.discussion, memory migrations, device scopes/token rotate, singles.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""; CH=""; HS=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FK" ]] && kill $FK 2>/dev/null; [[ -n "$CH" ]] && kill $CH 2>/dev/null; [[ -n "$HS" ]] && kill $HS 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-gateway-mcp] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
python3 "$ROOT/scripts/fake-github.py" >"$T/fk.port" 2>"$T/fk.log" & FK=$!
CPORT=$(freeport); mkdir -p "$T/ch"; python3 "$ROOT/scripts/fake_clawhub.py" "$CPORT" "$T/ch" >"$T/ch.log" 2>&1 & CH=$!
mkdir -p "$T/www"; echo '<html><head><title>Fallback</title><meta property="og:title" content="Claw Page"><meta name="description" content="pinchy things"></head></html>' > "$T/www/index.html"
HPORT=$(freeport); (cd "$T/www" && exec python3 -u -m http.server "$HPORT" --bind 127.0.0.1 >"$T/hs.log" 2>&1) & HS=$!
for i in $(seq 1 50); do [[ -s "$T/fk.port" ]] && curl -s "http://127.0.0.1:$HPORT/" >/dev/null && break; sleep 0.1; done
FPORT=$(cat "$T/fk.port"); PORT=$(freeport)
mkdir -p "$T/home/.claude" "$T/ws"; echo "# remember: tabs not spaces" > "$T/home/.claude/CLAUDE.md"
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" HOME="$T/home" \
  OPENCLAW_WORKSPACE="$T/ws" OPENCLAW_GITHUB_API_BASE_URL="http://127.0.0.1:$FPORT" GIT_CONFIG_GLOBAL=/dev/null GH_SETUP_HANDLE_1=good-token \
  CLAWHUB_REGISTRY="http://127.0.0.1:$CPORT" OPENCLAW_LINK_PREVIEW_ALLOW_PRIVATE=1
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export BASE="http://127.0.0.1:$PORT" T HPORT
python3 - <<'P' || fail "gateway mcp/singles"
import json, os, urllib.request, threading, time
BASE, T, HP = os.environ["BASE"], os.environ["T"], os.environ["HPORT"]
def post(path, body, hdr=None):
    req = urllib.request.Request(BASE + path, json.dumps(body).encode(), {"Content-Type": "application/json", **(hdr or {})})
    try: r = urllib.request.urlopen(req, timeout=60); return r.status, r.read()
    except urllib.error.HTTPError as e: return e.code, e.read()
def rpc(m, p):
    return json.loads(post("/v1/rpc", {"id": 1, "method": m, "params": p})[1])
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
def err(m, p, s):
    r = rpc(m, p); assert not r.get("ok") and s in json.dumps(r), (m, r); return r
# --- attach + /mcp
ok("chat.send", {"sessionKey": "agent:main:main", "message": "hello there"})
g = ok("attach.grant", {"sessionKey": "main"}); tok = g["token"]
assert g["sessionKey"] == "agent:main:main" and g["mcpConfig"]["url"].endswith("/mcp"), g
H = {"Authorization": "Bearer " + tok}
st, b = post("/mcp", {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}}, H)
assert st == 200 and json.loads(b)["result"]["serverInfo"]["name"] == "openclaw-gateway", b
assert post("/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, H)[0] == 202
tl = json.loads(post("/mcp", {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, H)[1])["result"]["tools"]
assert any(t["name"] == "agents_list" for t in tl), tl
c = json.loads(post("/mcp", {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "agents_list", "arguments": {}}}, H)[1])["result"]
assert not c["isError"] and "agents" in c["content"][0]["text"], c
assert json.loads(post("/mcp", {"jsonrpc": "2.0", "id": 4, "method": "bogus"}, H)[1])["error"]["code"] == -32601
assert ok("attach.revoke", {"token": tok})["revoked"] is True
# --- board widgets
w = ok("board.widget.put", {"sessionKey": "agent:main:main", "name": "w1", "content": {"html": "<b>hi</b>"}})
err("board.widget.grant", {"sessionKey": "agent:main:main", "name": "w1", "revision": w["revision"] + 5, "instanceId": w["instanceId"], "decision": "granted"}, "widget changed")
if w["grant"] == "pending": ok("board.widget.grant", {"sessionKey": "agent:main:main", "name": "w1", "revision": w["revision"], "instanceId": w["instanceId"], "decision": "granted"})
v = ok("board.widget.appView", {"sessionKey": "agent:main:main", "name": "w1", "revision": w["revision"], "instanceId": w["instanceId"]})
assert ok("board.prompt.authorize", {"ticket": v["viewId"]})["authorized"] is True
assert ok("board.event", {"ticket": v["viewId"], "event": "click", "payload": {"x": 1}})["accepted"] is True
err("board.event", {"ticket": "bview-nope"}, "ticket expired or unknown")
# --- controlUi
lp = ok("controlUi.linkPreview", {"url": f"http://127.0.0.1:{HP}/index.html"}); assert lp["title"] == "Claw Page" and lp["description"] == "pinchy things", lp
gp = ok("controlUi.githubPreview", {"url": "https://github.com/octo/project/pull/7"}); assert gp["kind"] == "pull" and gp["number"] == 7 and gp["author"] == "octo", gp
gd = ok("controlUi.githubDetail", {"url": "https://github.com/octo/project/issues/3"}); assert gd["labels"] == ["bug"] and gd["body"] == "fake body", gd
assert ok("controlUi.githubPreview", {"owner": "octo", "repo": "project"})["kind"] == "repository"
err("controlUi.githubPreview", {"url": "https://github.com/nobody/none/pull/1"}, "not found")
assert ok("controlUi.sessionPullRequests.checks", {"sessionKey": "agent:main:main"})["pullRequests"] == []
assert ok("controlUi.sessionPullRequests.subscribe", {"sessionKey": "agent:main:main"})["subscribed"] is True
# --- conversations
assert ok("conversations.send", {"sessionKey": "agent:main:conv", "message": "note"})["sent"] is True
t = ok("conversations.turn", {"sessionKey": "agent:main:conv", "message": "hi"}); assert t["status"] == "completed", t
assert ok("conversations.turn.cancel", {"turnId": t["turnId"]})["cancelled"] is False
# --- catalog via fake ClawHub
cb = ok("catalog.browse", {}); assert "items" in cb, cb
err("catalog.searchKeywords", {}, "query is required")
# --- discussion
d = ok("session.discussion.open", {"sessionKey": "agent:main:main"}); assert d["state"] == "open" and d["messages"] >= 1, d
assert ok("session.discussion.info", {"sessionKey": "agent:main:main"})["discussionSessionKey"] == "agent:main:main:discussion"
err("session.discussion.open", {"sessionKey": "agent:main:ghost"}, "session not found")
# --- transcripts / metadata / ui
s = ok("transcripts.summarize", {"selector": {"sessionKey": "agent:main:main"}}); assert "overview" in s["summary"], s
assert ok("chat.metadata.changed", {"sessionKey": "agent:main:main"})["sessionKey"] == "agent:main:main"
assert ok("ui.command", {"command": {"type": "openSettings"}})["ok"] is True
err("ui.command", {}, "command object is required")
err("nativeHook.invoke", {"relayId": "r1", "generation": 1, "provider": "codex"}, "unknown native hook relay")
err("worker.desktop.launch", {}, "environmentId is required")
err("web.login.start", {"channel": "whatsapp"}, "no loaded channel plugin")
err("artifacts.download", {"artifactId": "nope"}, "artifact not found")
assert "monitors" in ok("claws.monitors", {})
assert "path" in ok("config.openFile", {})
# --- memory migrations
pl = ok("migrations.memory.plan", {"agentId": "main"}); cc = [x for x in pl["providers"] if x["providerId"] == "claude-code"][0]
it = cc["items"][0]; assert it["action"] == "create", it
err("migrations.memory.apply", {"agentId": "main", "providerId": "claude-code", "planFingerprint": "x", "idempotencyKey": "k1", "itemIds": [it["id"]]}, "plan changed")
a = ok("migrations.memory.apply", {"agentId": "main", "providerId": "claude-code", "planFingerprint": cc["planFingerprint"], "idempotencyKey": "k2", "itemIds": [it["id"]]})
assert open(os.path.join(pl["workspace"], it["target"])).read().find("tabs not spaces") >= 0, a
assert ok("migrations.memory.apply", {"agentId": "main", "providerId": "claude-code", "planFingerprint": "ignored-on-replay", "idempotencyKey": "k2"}) == a
# --- device scope upgrade + wait
rq = ok("device.scopes.requestUpgrade", {"deviceId": "dev-x", "scopes": ["operator.read"]})
res = {}
th = threading.Thread(target=lambda: res.update(ok("device.scopes.waitUpgrade", {"requestId": rq["requestId"], "timeoutMs": 10000}))); th.start(); time.sleep(0.5)
ok("device.scopes.requestUpgrade", {"requestId": rq["requestId"], "decision": "deny"}); th.join(15)
assert res.get("status") == "denied", res
err("device.token.rotate", {"deviceId": "ghost", "role": "device"}, "no active token")
print("ok")
P
echo "[e2e-gateway-mcp] attach+/mcp server, board widgets, controlUi previews/checks, conversations, catalog, discussion, migrations, scope upgrades: PASS"
