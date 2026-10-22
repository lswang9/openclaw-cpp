#!/usr/bin/env bash
# Offline: Ollama /api/embeddings + GGUF magic validation (hash embedder) + FTS5 on memory sqlite.
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
PORT="${OPENCLAW_E2E_OG_PORT:-$((21600 + RANDOM % 200))}"
T="${TMPDIR:-/tmp}/oc-e2e-og-$$"
mkdir -p "$T/ws/memory" "$T/data"
cat > "$T/srv.py" <<'P'
import json,sys,hashlib
from http.server import BaseHTTPRequestHandler,HTTPServer
def vec(t):
    v=[0.0]*16
    for w in t.lower().split(): v[int(hashlib.md5(w.encode()).hexdigest(),16)%16]+=1.0
    return v
class H(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert self.path=="/api/embeddings" and "prompt" in body and body["model"]=="nomic-embed-text"
        open(sys.argv[2],"a").write(body["prompt"]+"\n")
        b=json.dumps({"embedding":vec(body["prompt"])}).encode()
        self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
HTTPServer(("127.0.0.1",int(sys.argv[1])),H).serve_forever()
P
python3 "$T/srv.py" "$PORT" "$T/prompts.txt" & SRV=$!
trap 'kill $SRV 2>/dev/null||true; rm -rf "$T"' EXIT
sleep 0.4
printf '# Prefs\nUser prefers dark roast coffee.\n\n# Infra\nDeploys on kubernetes clusters.\n' > "$T/ws/MEMORY.md"
export OPENCLAW_GGUF_HASH_FALLBACK=1  # header-only stub; real llama.cpp inference is e2e-gguf-llama.sh
python3 -c "open('$T/toy.gguf','wb').write(b'GGUF'+bytes([3,0,0,0])+b'\0'*64)"
export OPENCLAW_WORKSPACE="$T/ws" OPENCLAW_DATA_DIR="$T/data"
fail(){ echo "[e2e-og] $*" >&2; exit 1; }
cat > "$T/data/openclaw.json" <<J
{ agents: { defaults: { memorySearch: { provider: 'ollama', model: 'nomic-embed-text', remote: { baseUrl: 'http://127.0.0.1:$PORT' } } } } }
J
export OPENCLAW_CONFIG_PATH="$T/data/openclaw.json"
R=$("$BIN" memory search kubernetes)
echo "$R" | grep -q 'hybrid-ollama' || fail "ollama backend: $R"
python3 -c "import json,sys; r=json.load(sys.stdin); assert 'ollama' in r['backend'], r" <<<"$R" || fail "shape"
grep -q kubernetes "$T/prompts.txt" || fail "ollama not called"
# FTS: second search should still work; index may report fts
if echo "$R" | grep -q fts; then echo "[e2e-og] fts mentioned in response"; fi
cat > "$T/data/openclaw.json" <<J
{ agents: { defaults: { memorySearch: { provider: 'gguf', modelPath: '$T/toy.gguf', dimensions: 64 } } } }
J
R=$("$BIN" memory search coffee)
echo "$R" | grep -q 'hybrid-gguf' || fail "gguf backend: $R"
echo "[e2e-og] ollama + gguf providers (FTS when available): PASS"
