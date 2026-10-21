#!/usr/bin/env bash
# Real local GGUF embedding inference via llama.cpp (memorySearch provider 'gguf'). Fetches a ~21 MB
# all-MiniLM-L6-v2 Q4_K_M GGUF at test time (cached under ~/.cache/openclaw-cpp); SKIPs honestly when offline
# or when the binary was built without llama.cpp. The query shares no words with the target line, so only real
# semantic embeddings (not the hashed fallback) rank it first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
URL="${OPENCLAW_GGUF_TEST_URL:-https://huggingface.co/second-state/All-MiniLM-L6-v2-Embedding-GGUF/resolve/main/all-MiniLM-L6-v2-Q4_K_M.gguf}"
SHA="${OPENCLAW_GGUF_TEST_SHA256:-}"
grep -aq "llama.cpp failed to load" "$BIN" || { echo "[e2e-gguf-llama] SKIP: binary built without llama.cpp"; exit 0; }
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/openclaw-cpp"; mkdir -p "$CACHE"
M="$CACHE/all-MiniLM-L6-v2-Q4_K_M.gguf"
if [[ ! -s "$M" ]]; then
  curl -sfL --max-time 120 -o "$M.part" "$URL" && mv "$M.part" "$M" || { rm -f "$M.part"; echo "[e2e-gguf-llama] SKIP: model download failed (offline?)"; exit 0; }
fi
[[ -z "$SHA" ]] || echo "$SHA  $M" | sha256sum -c - >/dev/null || { echo "[e2e-gguf-llama] FAIL: sha256 mismatch"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/ws/memory" "$T/data"
printf '# Notes\nThe feline was napping on the rug all afternoon.\n\n# Finance\nQuarterly revenue grew because of strong bond yields.\n\n# Travel\nOur flight to Lisbon departs at dawn.\n' > "$T/ws/MEMORY.md"
cat > "$T/data/openclaw.json" <<J
{ agents: { defaults: { memorySearch: { provider: 'gguf', modelPath: '$M' } } } }
J
export OPENCLAW_WORKSPACE="$T/ws" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/data/openclaw.json"
R=$("$BIN" memory search "kitten sleeping on a carpet" 2>&1) || { echo "$R"; echo "[e2e-gguf-llama] FAIL: search errored"; exit 1; }
python3 - "$R" <<'PY' || { echo "$R"; echo "[e2e-gguf-llama] FAIL"; exit 1; }
import json, sys
r = json.loads(sys.argv[1])
assert "gguf" in r.get("backend", ""), r.get("backend")
hits = r.get("results") or r.get("hits") or []
assert hits, r
top = json.dumps(hits[0])
assert "feline" in top, ("semantic top hit wrong", hits[:2])
assert hits[0].get("vectorScore", 0) > 0.3 and all(h.get("vectorScore", 0) < 0.2 for h in hits[1:]), hits
PY
echo "[e2e-gguf-llama] PASS (real llama.cpp embeddings, model $(basename "$M"))"
