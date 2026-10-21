#!/usr/bin/env bash
# 1.109: worktrees.* (real git worktrees + snapshots), storage.locations.*, users.* profiles, device.pair.rename, message.action.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-rpc-groups] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
mkdir -p "$T/home" "$T/ws" "$T/repo"
( cd "$T/repo" && git init -q -b main && echo a > a.txt && git add a.txt && git -c user.name=t -c user.email=t@t commit -qm init )
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" \
  HOME="$T/home" OPENCLAW_WORKSPACE="$T/ws" GIT_CONFIG_GLOBAL=/dev/null
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T
python3 - <<'P' || fail "rpc groups"
import json, os, urllib.request, subprocess
URL, T = os.environ["URL"], os.environ["T"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
repo = T + "/repo"
wt = ok("worktrees.create", {"repoRoot": repo, "name": "feat"})
assert os.path.exists(wt["path"] + "/a.txt") and wt["branch"] == "openclaw/feat"
assert any(w["id"] == wt["id"] for w in ok("worktrees.list", {})["worktrees"])
br = ok("worktrees.branches", {"repoRoot": repo, "includeRepositoryStatus": True})
assert any(b["name"] == "openclaw/feat" and b["checkedOutInWorktree"] for b in br["branches"]) and br["repositoryStatus"]["dirty"] is False
open(wt["path"] + "/b.txt", "w").write("wip")
assert ok("worktrees.remove", {"id": wt["id"], "ifLossless": True})["removed"] is False
rm = ok("worktrees.remove", {"id": wt["id"]}); assert rm["removed"] and rm["snapshotRef"].startswith("refs/openclaw/worktree-snapshots/")
assert not os.path.exists(wt["path"])
rs = ok("worktrees.restore", {"id": wt["id"]}); assert rs["status"] == "active" and open(wt["path"] + "/b.txt").read() == "wip"
assert ok("worktrees.retireSnapshot", {"id": wt["id"]})["retired"]
assert subprocess.run(["git","-C",repo,"show-ref","--verify","refs/openclaw/worktree-snapshots/"+wt["id"]],capture_output=True).returncode != 0
assert ok("worktrees.gc", {})["status"] == "completed"
assert ok("worktrees.recoverRemoval", {"id": wt["id"]})["recovered"] is False
assert not rpc("worktrees.create", {"repoRoot": T + "/ws", "name": "x"}).get("ok")
locs = ok("storage.locations.list", {})["locations"]; assert {l["name"] for l in locs} >= {"data","sessions","workspace"}
assert ok("storage.locations.probe", {"name": "data"})["writable"] is True
assert not rpc("storage.locations.probe", {"name": "nope"}).get("ok")
p = ok("users.linkEmail", {"email": "me@example.com"})["profile"]; assert "me@example.com" in p["emails"]
assert not rpc("users.linkEmail", {"email": "me@example.com", "targetProfileId": "bob"}).get("ok")
ok("users.linkChannelIdentity", {"profileId": "bob", "channel": "telegram", "senderId": "42"})
assert ok("users.listChannelIdentities", {"profileId": "bob"})["identities"][0]["senderId"] == "42"
ok("users.linkAuthProfile", {"profileId": "bob", "authProfileId": "openai:bob"})
assert ok("users.selectModelAccount", {"profileId": "bob", "accountId": "openai:bob"})["selected"] == "openai:bob"
assert not rpc("users.selectModelAccount", {"profileId": "bob", "accountId": "x"}).get("ok")
ok("users.personalFile.set", {"profileId": "bob", "name": "USER.md", "content": "likes tea"})
assert ok("users.personalFile.get", {"profileId": "bob", "name": "USER.md"})["content"] == "likes tea"
assert ok("users.setRole", {"profileId": "bob", "role": "admin"})["profile"]["role"] == "admin"
m = ok("users.merge", {"sourceProfileId": "bob", "targetProfileId": "operator"})
assert m["profile"]["personalFiles"]["USER.md"] == "likes tea" and "channelIdentities" in m["movedAliasKinds"]
assert all(u["id"] != "bob" for u in ok("users.mentionable", {})["users"])
assert ok("device.pair.rename", {"deviceId": "dev1", "displayName": "Laptop"})["displayName"] == "Laptop"
assert not rpc("device.pair.rename", {"deviceId": "dev1"}).get("ok")
assert ok("message.action", {"channel": "telegram", "action": "react", "params": {"emoji": "👍"}})["result"]["status"] == "recorded"
assert not rpc("message.action", {"channel": "telegram", "action": "explode"}).get("ok")
print("ok")
P
echo "[e2e-rpc-groups] worktrees create/branches/remove+snapshot/restore/retire/gc, storage, users profiles+merge, device rename, message.action: PASS"
