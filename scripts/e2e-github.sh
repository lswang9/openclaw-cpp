#!/usr/bin/env bash
# 1.100: projects.searchRemote and sessions.github.publish/confirm against an offline fake GitHub API and a local
# bare repository (git transport redirected with OPENCLAW_GITHUB_GIT_URL_BASE). No network, no real credentials.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FK=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FK" ]] && kill $FK 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-github] FAIL: $*"; tail -5 "$T/gw.log"; exit 1; }
python3 "$ROOT/scripts/fake-github.py" >"$T/fk.port" 2>"$T/fk.log" & FK=$!
for i in $(seq 1 50); do [[ -s "$T/fk.port" ]] && break; sleep 0.1; done
FPORT=$(cat "$T/fk.port")
# remote "octo/project" as a bare repo with a main branch; the session workspace is a clone with origin on github.com
mkdir -p "$T/remotes/octo" "$T/seed"
git init -q --bare -b main "$T/remotes/octo/project.git"
( cd "$T/seed" && git init -q -b main && git -c user.name=t -c user.email=t@t commit -q --allow-empty -m init && echo base >README.md && git add README.md \
  && git -c user.name=t -c user.email=t@t commit -q -m base && git push -q "$T/remotes/octo/project.git" main )
git clone -q "$T/remotes/octo/project.git" "$T/ws"
git -C "$T/ws" remote set-url origin https://github.com/octo/project.git
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" HOME="$T/home" \
  OPENCLAW_WORKSPACE="$T/ws" OPENCLAW_GITHUB_API_BASE_URL="http://127.0.0.1:$FPORT" OPENCLAW_GITHUB_GIT_URL_BASE="file://$T/remotes/" \
  GIT_CONFIG_GLOBAL=/dev/null
mkdir -p "$T/home"
OPENCLAW_GATEWAY_PORT=$PORT GH_TOKEN=good-token "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" FAKE="http://127.0.0.1:$FPORT" WS="$T/ws" REMOTE="$T/remotes/octo/project.git"
cat >"$T/t.py" <<'P'
import json, os, subprocess, urllib.request
URL, FAKE, WS, REMOTE = os.environ["URL"], os.environ["FAKE"], os.environ["WS"], os.environ["REMOTE"]
def rpc(m, p):
    r = urllib.request.urlopen(urllib.request.Request(URL, json.dumps({"jsonrpc":"2.0","id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"}))
    return json.loads(r.read())
def ok(m, p):
    j = rpc(m, p); assert j.get("ok") is True, (m, j); return j["payload"]
def err(m, p, needle):
    j = rpc(m, p); assert j.get("ok") is False, (m, j); assert needle in json.dumps(j.get("error")), (m, needle, j)
def log(): return json.loads(urllib.request.urlopen(FAKE + "/_log").read())
# ---- projects.searchRemote: exact repo first, affiliated (token) matches, then global; dedupe; credential flag
r = ok("projects.searchRemote", {"query": "widgets"})
names = [p["fullName"] for p in r["projects"]]
assert r["credential"] == "configured" and names[0] == "octo/secret-widgets" and "acme/widgets" in names and "acme/gadgets" in names, r
assert names.count("acme/widgets") == 1 and all(p["cloneUrl"].endswith(".git") and p["webUrl"] for p in r["projects"]), r
assert next(p for p in r["projects"] if p["fullName"] == "octo/secret-widgets")["private"] is True
e = ok("projects.searchRemote", {"query": "acme/gadgets"})
assert e["projects"][0]["fullName"] == "acme/gadgets" and e["projects"][0]["defaultBranch"] == "main", e
n0 = len(log()["log"]); ok("projects.searchRemote", {"query": "WIDGETS "}); assert len(log()["log"]) == n0 + 1, "60s cache (case/trim-insensitive)"
err("projects.searchRemote", {"query": ""}, "query")
err("projects.searchRemote", {"query": "x", "extra": 1}, "unexpected property")
# ---- sessions.github.publish
ok("chat.send", {"sessionKey": "agent:main:feature-x", "message": "work"})
S = "agent:main:feature-x"
nc = ok("sessions.github.publish", {"sessionKey": S, "idempotencyKey": "k0"})
assert nc["status"] == "failed" and nc["code"] == "no_changes" and nc["nextAction"], nc
open(os.path.join(WS, "feature.txt"), "w").write("new feature\n")
pub = ok("sessions.github.publish", {"sessionKey": S, "idempotencyKey": "k1", "title": "Add feature", "body": "Adds feature.txt"})
assert pub["status"] == "published" and pub["repository"] == "octo/project" and pub["branch"] == "openclaw/agent-main-feature-x", pub
assert pub["url"] == "https://github.com/octo/project/pull/1" and len(pub["headCommit"]) == 40 and pub["publisher"]["login"] == "octo", pub
remote_head = subprocess.check_output(["git", "--git-dir", REMOTE, "rev-parse", "refs/heads/" + pub["branch"]], text=True).strip()
assert remote_head == pub["headCommit"], (remote_head, pub)
msg = subprocess.check_output(["git", "--git-dir", REMOTE, "log", "-1", "--format=%an <%ae>%n%B", remote_head], text=True)
assert "octo <4242+octo@users.noreply.github.com>" in msg and "OpenClaw-Publication: " + pub["requestId"] in msg and "Add feature" in msg, msg
files = subprocess.check_output(["git", "--git-dir", REMOTE, "ls-tree", "--name-only", remote_head], text=True).split()
assert "feature.txt" in files, files
assert subprocess.check_output(["git", "-C", WS, "status", "--porcelain"], text=True).strip() == "?? feature.txt", "user index/HEAD untouched"
pulls = log()["pulls"]; assert len(pulls) == 1 and pulls[0]["head"] == pub["branch"] and pulls[0]["base"] == "main" and "openclaw-publication:" in pulls[0]["body"], pulls
again = ok("sessions.github.publish", {"sessionKey": S, "idempotencyKey": "k1", "title": "Add feature", "body": "Adds feature.txt"})
assert again == pub, "idempotent replay"
err("sessions.github.publish", {"sessionKey": S, "idempotencyKey": "k1", "title": "Different"}, "different publication request")
# second publication on the same branch reuses the open PR
open(os.path.join(WS, "feature.txt"), "a").write("more\n")
p2 = ok("sessions.github.publish", {"sessionKey": S, "idempotencyKey": "k2", "title": "More"})
assert p2["status"] == "published" and p2["url"] == pub["url"] and p2["headCommit"] != pub["headCommit"] and len(log()["pulls"]) == 1, p2
# personal selection → needs_confirmation → confirm (digest/generation/account must match)
acct = {"accountId": 4242, "login": "octo"}; gen = "6f1c2d3e-4a5b-4c6d-8e7f-0123456789ab"
open(os.path.join(WS, "feature.txt"), "a").write("personal\n")
c = ok("sessions.github.publish", {"sessionKey": S, "idempotencyKey": "k3", "selection": {"source": "personal", "generation": gen, "account": acct}})
assert c["status"] == "needs_confirmation", c
import glob
db = json.load(open(glob.glob(os.environ["OPENCLAW_DATA_DIR"] + "/github-publications.json")[0]))
digest = db["requests"][c["requestId"]]["requestDigest"]
err("sessions.github.confirm", {"sessionKey": S, "requestId": c["requestId"], "generation": gen, "account": acct, "requestDigest": "0" * 64}, "does not match")
done = ok("sessions.github.confirm", {"sessionKey": S, "requestId": c["requestId"], "generation": gen, "account": acct, "requestDigest": digest})
assert done["status"] == "published" and done["publisher"]["source"] == "personal", done
bad = {"sessionKey": S, "requestId": c["requestId"], "generation": gen, "account": {"accountId": 1, "login": "x"}, "requestDigest": digest}
assert ok("sessions.github.confirm", bad)["status"] == "published", "settled confirmation is idempotent"
# not_github workspace
subprocess.check_call(["git", "-C", WS, "remote", "set-url", "origin", "https://gitlab.com/octo/project.git"])
ng = ok("sessions.github.publish", {"sessionKey": S, "idempotencyKey": "k4"})
assert ng["status"] == "failed" and ng["code"] == "not_github", ng
print("ok")
P
OPENCLAW_DATA_DIR="$T/data" python3 "$T/t.py" || fail "searchRemote/publish"
kill $GW; wait $GW 2>/dev/null || true; GW=""
# missing credential: credential=missing for search; publish fails identity_unavailable
OPENCLAW_GATEWAY_PORT=$PORT GH_TOKEN= GITHUB_TOKEN= OPENCLAW_GITHUB_TOKEN= "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
R=$(curl -s "$URL" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"projects.searchRemote","params":{"query":"widgets"}}')
python3 -c "import json,sys;j=json.loads(sys.argv[1])['payload'];assert j['credential']=='missing' and 'octo/secret-widgets' not in [p['fullName'] for p in j['projects']],j" "$R" || fail "anonymous search: $R"
P=$(curl -s "$URL" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"sessions.github.publish","params":{"sessionKey":"agent:main:feature-x","idempotencyKey":"z"}}')
grep -q identity_unavailable <<<"$P" || fail "no credential: $P"
echo "[e2e-github] projects.searchRemote (exact/affiliated/global, cache) + sessions.github.publish/confirm (push, PR, idempotency, personal confirm): PASS"
