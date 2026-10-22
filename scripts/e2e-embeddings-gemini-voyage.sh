#!/usr/bin/env bash
# Offline: Gemini batchEmbedContents + Voyage /embeddings clients against local fake servers (task types,
# auth headers, outputDimensionality), auto-selection by env key, both models side by side in the SQLite store.
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_GV_PORT:-$((20400 + RANDOM % 300))}"
T="${TMPDIR:-/tmp}/oc-e2e-gv-$$"
mkdir -p "$T/ws/memory" "$T/data"
cat > "$T/srv.py" <<'P'
import json, sys, hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
log = open(sys.argv[2], "a")
def vec(t, n):
    v = [0.0]*n
    for w in t.lower().split():
        v[int(hashlib.md5(w.strip('.,').encode()).hexdigest(), 16) % n] += 1.0
    return v
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def ok(self, o):
        b = json.dumps(o).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/gemini/v1beta/models/gemini-embedding-001:batchEmbedContents":
            if self.headers.get("x-goog-api-key") != "g-key": self.send_response(401); self.end_headers(); return
            reqs = body["requests"]
            tasks = sorted({r["taskType"] for r in reqs}); dims = {r.get("outputDimensionality") for r in reqs}
            assert all(r["model"] == "models/gemini-embedding-001" for r in reqs)
            log.write(json.dumps({"p": "gemini", "n": len(reqs), "tasks": tasks, "dims": list(dims)}) + "\n"); log.flush()
            return self.ok({"embeddings": [{"values": vec(r["content"]["parts"][0]["text"], 24)} for r in reqs]})
        if self.path == "/voyage/v1/embeddings":
            if self.headers.get("Authorization") != "Bearer v-key": self.send_response(401); self.end_headers(); return
            log.write(json.dumps({"p": "voyage", "n": len(body["input"]), "type": body["input_type"], "model": body["model"]}) + "\n"); log.flush()
            return self.ok({"data": [{"index": i, "embedding": vec(t, 20)} for i, t in enumerate(body["input"])], "model": body["model"]})
        self.send_response(404); self.end_headers()
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
P
python3 "$T/srv.py" "$PORT" "$T/log.jsonl" & SRV=$!
trap 'kill $SRV 2>/dev/null || true; rm -rf "$T"' EXIT
sleep 0.5
printf '# Prefs\nUser prefers dark roast coffee.\n\n# Infra\nDeploys on kubernetes clusters.\n' > "$T/ws/MEMORY.md"
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json" OPENCLAW_WORKSPACE="$T/ws" OPENCLAW_DATA_DIR="$T/data"
unset OPENAI_API_KEY GEMINI_API_KEY GOOGLE_API_KEY VOYAGE_API_KEY
fail() { echo "[e2e-gv] $*" >&2; exit 1; }
# Gemini via explicit provider, outputDimensionality
cat > "$OPENCLAW_CONFIG_PATH" <<J
{ agents: { defaults: { memorySearch: { provider: 'gemini', outputDimensionality: 24, remote: { baseUrl: 'http://127.0.0.1:$PORT/gemini/v1beta', apiKey: 'g-key' } } } } }
J
R=$("$BIN" memory search kubernetes clusters)
echo "$R" | grep -Eq '"backend": "hybrid-gemini(\+fts5)?"' || fail "gemini backend: $R"
echo "$R" | python3 -c 'import json,sys; r=json.load(sys.stdin); assert r["results"][0]["start_line"]==4, r'
grep -q '"tasks": \["RETRIEVAL_DOCUMENT"\]' "$T/log.jsonl" && grep -q '"tasks": \["RETRIEVAL_QUERY"\]' "$T/log.jsonl" || fail "gemini task types"
grep -q '"dims": \[24\]' "$T/log.jsonl" || fail "outputDimensionality not sent"
# Voyage via auto + VOYAGE_API_KEY env (no key in config)
cat > "$OPENCLAW_CONFIG_PATH" <<J
{ agents: { defaults: { memorySearch: { provider: 'auto', model: 'voyage-3.5-lite', remote: { baseUrl: 'http://127.0.0.1:$PORT/voyage/v1' } } } } }
J
R=$(VOYAGE_API_KEY=v-key "$BIN" memory search dark roast coffee)
echo "$R" | grep -Eq '"backend": "hybrid-voyage(\+fts5)?"' || fail "voyage backend: $R"
echo "$R" | python3 -c 'import json,sys; r=json.load(sys.stdin); assert r["results"][0]["start_line"]==1, r'
grep -q '"type": "document"' "$T/log.jsonl" && grep -q '"type": "query"' "$T/log.jsonl" || fail "voyage input_type"
# both models persisted side by side
python3 - "$T/data/memory/main.sqlite" <<'PY'
import sqlite3,sys
rows=dict(((p,m),(n,d)) for p,m,d,n in sqlite3.connect(sys.argv[1]).execute("select provider,model,dims,count(*) from embedding_cache group by 1,2,3"))
assert rows[("gemini","gemini-embedding-001@24")]==(2,24) and rows[("voyage","voyage-3.5-lite")]==(2,20), rows
PY
# wrong key → falls back to local, reported
R=$(VOYAGE_API_KEY=nope "$BIN" memory search coffee)
echo "$R" | grep -q '"fallback"' || fail "auth failure should fall back: $R"
echo "[e2e-gv] Gemini + Voyage embeddings (task types, dims, auth, auto, side-by-side store, fallback): PASS"
