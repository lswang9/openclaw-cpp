#!/usr/bin/env bash
# Offline e2e: OpenAI-compatible embeddings client against a local fake /v1/embeddings server (no creds, no network).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_EMB_PORT:-18798}"
T="${TMPDIR:-/tmp}/oc-e2e-emb-$$"
mkdir -p "$T/ws/memory" "$T/data"
cat > "$T/srv.py" <<'P'
import json, sys, hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
calls = {"n": 0, "inputs": 0}
def vec(t):
    v = [0.0]*16
    for w in t.lower().split():
        v[int(hashlib.md5(w.strip('.,').encode()).hexdigest(), 16) % 16] += 1.0
    return v
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert self.path == "/v1/embeddings" and self.headers.get("Authorization") == "Bearer test-key"
        calls["n"] += 1; calls["inputs"] += len(body["input"])
        open(sys.argv[2], "w").write(json.dumps(calls))
        out = {"data": [{"index": i, "embedding": vec(t)} for i, t in enumerate(body["input"])], "model": body["model"]}
        b = json.dumps(out).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
P
python3 "$T/srv.py" "$PORT" "$T/calls.json" & SRV=$!
trap 'kill $SRV 2>/dev/null || true; rm -rf "$T"' EXIT
sleep 0.5
printf '# Prefs\nUser prefers dark roast coffee.\n\n# Infra\nDeploys on kubernetes clusters.\n' > "$T/ws/MEMORY.md"
cat > "$T/data/openclaw.json" <<J
{ agents: { defaults: { memorySearch: { provider: 'openai', model: 'fake-embed', remote: { baseUrl: 'http://127.0.0.1:$PORT/v1', apiKey: 'test-key' } } } } }
J
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_WORKSPACE="$T/ws" OPENCLAW_DATA_DIR="$T/data"
R=$("$BIN" memory search kubernetes clusters)
echo "$R" | grep -q '"backend": "hybrid-openai"' || { echo "[e2e-emb] wrong backend: $R" >&2; exit 1; }
echo "$R" | python3 -c 'import json,sys; r=json.load(sys.stdin); assert r["results"][0]["start_line"]==4, r'
"$BIN" memory search coffee >/dev/null
python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); assert c["inputs"]==4, c' "$T/calls.json"   # 2 chunks once + 2 queries
[[ -f "$T/data/memory/main.index.json" ]]
echo "[e2e-emb] openai-compatible embeddings + persisted index (chunks embedded once): PASS"
