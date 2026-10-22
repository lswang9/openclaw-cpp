#!/usr/bin/env bash
# Real local GGUF embedding inference via llama.cpp (memorySearch provider 'gguf'). Fetches a ~21 MB
# all-MiniLM-L6-v2 Q4_K_M GGUF at test time (cached under ~/.cache/openclaw-cpp); SKIPs honestly when offline
# or when the binary was built without llama.cpp. The query shares no words with the target line, so only real
# semantic embeddings (not the hashed fallback) rank it first.
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
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
# Batching + long context: 20 texts in one call must match one-at-a-time embeddings; a text far longer than
# the model's 512-token context is windowed (not truncated), so a needle at its end still moves the vector.
LONG=$(python3 -c "print(' '.join(['the weather report mentions mild clouds.']*120))")
NEEDLE="$LONG A kitten is sleeping on the carpet."
ARGS=(); for i in $(seq 1 18); do ARGS+=("note number $i about topic $((i*7))"); done
B=$("$BIN" memory embed "$LONG" "$NEEDLE" "${ARGS[@]}") && S=$("$BIN" memory embed --sequential "$LONG" "$NEEDLE" "${ARGS[@]}") || { echo "[e2e-gguf-llama] FAIL: embed"; exit 1; }
python3 - "$B" "$S" <<'PY' || { echo "$B" | head -c 600; echo "[e2e-gguf-llama] FAIL: batching/long"; exit 1; }
import json, sys
b, s = json.loads(sys.argv[1]), json.loads(sys.argv[2])
assert b["count"] == 20 and b["dims"] == 384, b["count"]
st = b["stats"]; assert st["n_ctx"] == 512 and st["windows"] > 20, st
assert st["decodes"] < st["windows"], ("not batched", st)       # several sequences per decode
assert s["stats"]["decodes"] >= 20, s["stats"]
for x, y in zip(b["head"], s["head"]): assert abs(x - y) < 5e-3, (b["head"], s["head"])  # batched vs sequential float paths differ slightly (seen 2e-3 under load)
c = b["cosine"]
assert 0.5 < c[0][1] < 0.9999, c[0][1]                            # needle beyond token 512 changed the vector
for i in range(20): assert abs(c[i][i] - 1) < 1e-3
PY
echo "[e2e-gguf-llama] PASS (real llama.cpp embeddings, model $(basename "$M"))"
