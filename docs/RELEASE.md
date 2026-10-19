# Release checklist (openclaw-cpp)

## Pre-release

1. `./build/build/Release/openclaw-cpp self-test` — must be **ALL PASS**
2. `./scripts/integration-smoke.sh` — gateway HTTP smoke with **mock** provider (no live LLM)
3. Skim [PARITY.md](../PARITY.md) — update any `impl` / gap language for this cut
4. Confirm no secrets in tree (`rg -i 'api[_-]?key|sk-' --glob '!**/.venv/**'`)
5. Version strings agree: `CMakeLists.txt`, `conanfile.py`, `kVersion` in `src/main.cpp` + `src/gateway/server.cpp`

## Cutting a non-dev tag (later)

Dev line uses `X.Y.Z-dev` (e.g. `1.5.1-dev`). For a tagged release:

```bash
# 1. Bump to X.Y.Z (drop -dev) in CMakeLists.txt, conanfile.py, kVersion constants, PARITY/README
# 2. Commit: release: X.Y.Z
# 3. Tag annotated:
git tag -a vX.Y.Z -m "openclaw-cpp X.Y.Z"
git push origin main --tags
# 4. Optional: gh release create vX.Y.Z --generate-notes
# 5. Immediately bump main to (X.Y.Z+1)-dev or next planned -dev
```

Do **not** claim OpenClaw 2026.10.5 full parity on the release notes unless PARITY says so.

## CI

Canonical workflow YAML: [ci-github-actions.yml](ci-github-actions.yml).  
Installing under `.github/workflows/` needs a GitHub token with `workflow` scope (OAuth push may reject).

## Smoke without LLM

```bash
export OPENCLAW_DEFAULT_PROVIDER=mock
./scripts/integration-smoke.sh
```
