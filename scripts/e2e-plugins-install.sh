#!/usr/bin/env bash
# 1.117: plugins catalog/install (path, .tgz) / uninstall / inspect / reload / controlUi / credentials / skills.read.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; wait 2>/dev/null; rm -rf "$T" 2>/dev/null || true' EXIT
fail() { echo "[e2e-plugins-install] FAIL: $*"; tail -8 "$T/gw.log"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
echo mock-resource > "$T/res.txt"; mkdir -p "$T/home" "$T/ws" "$T/repo"
( cd "$T/repo" && git init -q -b main && echo a > a.txt && git add a.txt && git -c user.name=t -c user.email=t@t commit -qm init )
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" \
  HOME="$T/home" OPENCLAW_WORKSPACE="$T/ws" GIT_CONFIG_GLOBAL=/dev/null
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export URL="http://127.0.0.1:$PORT/v1/rpc" T
mkdir -p "$T/src/acme/skills/hello"
cat > "$T/src/acme/openclaw.plugin.json" <<'J'
{"id": "acme", "name": "Acme", "version": "1.2.3", "credentials": [{"env": "ACME_API_KEY"}], "controlUi": [{"id": "acme-panel", "title": "Acme"}]}
J
echo "# hello skill" > "$T/src/acme/skills/hello/SKILL.md"
( cd "$T/src" && tar -czf acme.tgz acme )
python3 - <<'P' || fail "plugins"
import json, os, urllib.request
URL, T = os.environ["URL"], os.environ["T"]
def rpc(m, p):
    req = urllib.request.Request(URL, json.dumps({"id":1,"method":m,"params":p}).encode(), {"Content-Type":"application/json"})
    try: return json.loads(urllib.request.urlopen(req, timeout=60).read())
    except urllib.error.HTTPError as e: return json.loads(e.read())
def ok(m, p):
    r = rpc(m, p); assert r.get("ok"), (m, r); return r["payload"]
assert any(c["id"] == "examples" for c in ok("plugins.catalog.categories", {})["categories"])
b = ok("plugins.catalog.browse", {"category": "examples"})["plugins"]; assert b and all(x["installed"] for x in b if x["id"] == "sample-echo"), b
assert ok("plugins.catalog.get", {"id": "sample-upstream"})["plugin"]["name"]
assert not rpc("plugins.catalog.get", {"id": "nope"}).get("ok")
i = ok("plugins.install", {"source": T + "/src/acme"}); assert i["id"] == "acme" and i["kind"] == "path"
assert any(p["id"] == "acme" and p["enabled"] for p in ok("plugins.list", {})["plugins"])
assert not rpc("plugins.install", {"source": T + "/src/acme"}).get("ok")
assert ok("plugins.install", {"source": T + "/src/acme.tgz", "force": True})["kind"] == "archive"
ins = ok("plugins.inspect", {"id": "acme"}); assert ins["manifest"]["version"] == "1.2.3" and ins["skills"] == ["hello"] and ins["install"]["kind"] == "archive", ins
c = ok("plugins.credentials.inspect", {"id": "acme"})["credentials"]; assert c == [{"env": "ACME_API_KEY", "present": False}], c
assert "hello skill" in ok("plugins.skills.read", {"id": "acme", "skill": "hello"})["content"]
assert not rpc("plugins.skills.read", {"id": "acme", "skill": "../x"}).get("ok")
assert any(e["id"] == "acme-panel" for e in ok("plugins.controlUi.list", {})["entries"])
ok("plugins.controlUi.report", {"pluginId": "acme", "status": "error", "error": "boom"})
assert ok("plugins.controlUi.status", {"pluginId": "acme"})["status"]["status"] == "error"
assert ok("plugins.reload", {})["plugins"]
assert not rpc("plugins.uninstall", {"id": "sample-echo"}).get("ok")
ok("plugins.uninstall", {"id": "acme"}); assert not os.path.exists(T + "/data/plugins/acme")
assert not any(p["id"] == "acme" for p in ok("plugins.list", {})["plugins"])
assert "plugins.changed" in json.dumps(ok("logs.tail", {"n": 400})["lines"])
print("[e2e-plugins-install] ok")
P
