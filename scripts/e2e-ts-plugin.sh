#!/usr/bin/env bash
# TypeScript plugin load test: needs Node >= 22.6 (OPENCLAW_NODE, node on PATH, or .tools/node-v22*).
set -euo pipefail
# Offline guard: never leak host LLM keys when this script is run alone (1.136).
export OPENCLAW_DEFAULT_PROVIDER="${OPENCLAW_DEFAULT_PROVIDER:-mock}"
unset DEEPSEEK_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENROUTER_API_KEY XAI_API_KEY MISTRAL_API_KEY GROQ_API_KEY 2>/dev/null || true
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
ok_node() { "$1" -e 'const [a,b]=process.versions.node.split(".").map(Number);process.exit(a>22||(a===22&&b>=6)?0:1)' 2>/dev/null; }
N="${OPENCLAW_NODE:-}"
if [[ -z "$N" ]]; then
  for c in node "$ROOT"/../.tools/node-v2*/bin/node "$ROOT"/.tools/node-v2*/bin/node; do
    if command -v "$c" >/dev/null 2>&1 && ok_node "$c"; then N="$c"; break; fi
  done
fi
if [[ -z "$N" ]] || ! ok_node "$N"; then echo "[e2e-ts] SKIP (no Node >= 22.6)"; exit 0; fi
OUT=$(OPENCLAW_NODE="$N" "$BIN" tsdemo)
[[ "$OUT" == "ts-square:49" ]] || { echo "[e2e-ts] tsdemo CLI failed: $OUT" >&2; exit 1; }
OPENCLAW_NODE="$N" "$BIN" plugins info 2>/dev/null | grep -q '"js.ts_square"' || { echo "[e2e-ts] ts tool not registered" >&2; exit 1; }
echo "[e2e-ts] TypeScript plugin (Node $("$N" --version), strip-types): tool + CLI: PASS"
