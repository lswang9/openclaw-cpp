# OpenClaw 2026.10.5 → openclaw-cpp — honest parity

**Current: 1.5.0-dev. NOT full OpenClaw 2026.10.5 parity.**

## Impl tags

| `impl` | Meaning |
|--------|---------|
| **`real`** | Local state and/or real HTTP when env configured. Subset of upstream. |
| **`real-sim`** | `SimulationBackend` — placeholder PNG, action log. **Not** production computer-use. |
| **`honest`** | Rare partial catch-all. |
| **`implemented:false`** | Intentionally missing (debugProxy, claws, nativeHook, web.login, attach, …). |

## Scale

| Metric | Value |
|--------|--------|
| Catalog advertised | **516** |
| `ImplementedMethods` | **~640** (prefix buckets + local stores for board/portal/setup/…) |
| Count ≠ parity | Many names share handlers; semantics ≠ upstream |

## 1.5 hardening

| Item | Status |
|------|--------|
| Agent loop timeout | **`real`** — `timeout_ms` / `agent.timeout_ms` → `LoopOptions.timeout` |
| Cooperative cancel | **`real`** — gateway `Stop()` sets cancel flag checked each turn |
| Session locks | **`real`** — per-id `recursive_mutex` on Load/Save/Append/Remove |
| HTTP JSON-RPC | **`real`** — `POST /v1/rpc`; `POST /v1/sessions` |
| Integration smoke | **`scripts/integration-smoke.sh`** — mock provider, no live LLM |
| Release docs | **`docs/RELEASE.md`** — how to cut a non-dev tag |

## Still hard / missing vs upstream

| Area | Honest status |
|------|----------------|
| computer/canvas/desktop | **real-sim only** |
| browser | lightweight curl+title; no Playwright |
| mcp.app host UI | list/call/resources **real**; host chrome local-ack |
| board/portal/wizard/openclaw.setup | **local JSON stores** for API testing — **not** production UIs |
| ACP wire / JS plugins / full IM | gap / partial (see prior docs) |
| CI under `.github/workflows` | docs YAML only (token scope) |

## Release

See [docs/RELEASE.md](docs/RELEASE.md). Do not claim full 2026.10.5 parity in tags unless this file says so.
