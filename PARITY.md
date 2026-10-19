# OpenClaw 2026.10.5 → openclaw-cpp — brutally honest parity

**Current: 1.4.0-dev. This is NOT full OpenClaw 2026.10.5 parity.**

OpenClaw upstream is a large Node/TS product (ACP agents, real browser/computer use, rich Control UI, JS plugins, production IM stacks). openclaw-cpp is a **C++20 rewrite** that implements a growing **API surface** with local state, optional HTTP, and **explicit simulation** where real OS/browser automation is out of scope.

## Impl tags (read these in RPC responses)

| `impl` | Meaning |
|--------|---------|
| **`real`** | Touches real local state and/or real network (curl IM/TTS/http_get when env set). Still may be a **subset** of upstream behavior. |
| **`real-sim`** | Deterministic **SimulationBackend**. Records mouse/keyboard/canvas/desktop actions; screenshot is a **placeholder 1×1 PNG**. Useful for API/catalog testing — **not** production computer-use. |
| **`honest`** | Partial; returns visible state + note. Rare residual. |
| **`stub` / `implemented:false`** | Intentionally missing or not faked (board host UI, Playwright browser, etc.). |

## Scale

| Metric | Value |
|--------|--------|
| Catalog advertised | **516** |
| `ImplementedMethods` | **566** |
| Production-grade vs upstream | **Low–medium** on chat/gateway/tools; **sim/lightweight** on computer/canvas/desktop/browser |

Many of the 566 names are served by **prefix buckets** (skills/users/sessions/…). That expands surface coverage; it does **not** mean each name matches OpenClaw 2026.10.5 semantics.

## Hard gaps (1.4 focus)

| Area | Status | Honest note |
|------|--------|-------------|
| **computer / canvas / desktop** | **`real-sim`** | `SimulationBackend`: event log, placeholder PNG, canvas JSON doc, desktop launch flags. **No** real display, accessibility tree, or process control. |
| **browser** | **lightweight `real` + stub** | `browser.open_url` / tool `browser_open_url`: curl + `<title>` regex + stored text. **`browser` tool** still refuses full automation (no Playwright/Chromium). |
| **mcp.app.*** | **`real` (partial)** | Bridged to in-proc/`mcp::Client`: listTools/callTool/listResources/readResource. Host-UI methods (`view`/`launch`/`settings`/…) are **local ack** only — not an MCP App host. |
| **board / portal / wizard / openclaw.*** | **`implemented:false`** | Not faked. |
| **ACP wire / acpx** | **skeleton / gap** | See [docs/ACP-GAP.md](docs/ACP-GAP.md). |
| **IM** | **partial `real`** | Feishu/Slack/Discord/WA/iMessage inbound webhooks; TG/Discord/Feishu outbound when tokens set. No full Socket Mode / Baileys / multi-tenant prod. |
| **Plugins** | **partial `real`** | Linux `.so` C ABI sample — **not** JS plugin host. |
| **TTS** | **partial `real`** | HTTP if `OPENCLAW_TTS_URL`; else record_only. |
| **Control UI** | **skeleton `real`** | Embedded HTML: chat SSE, tools invoke, session export — not upstream Control UI. |
| **CI** | **docs only** | [docs/ci-github-actions.yml](docs/ci-github-actions.yml); `.github/workflows` not pushed (token lacks `workflow` scope). |

## What “align OpenClaw 2026.10.5” means here

- **Goal:** grow toward the **same feature checklist** over time.
- **Today:** a Conan/C++ gateway with chat/tools/sessions/channels/MCP bridge and a **catalog-compatible RPC list**, with **honest** `impl` labels.
- **Do not** treat `ImplementedMethods` count as % feature parity.

## Milestone

- **1.4.0-dev**: SimulationBackend (`real-sim`); lightweight `browser.open_url`; `mcp.app.*` client bridge; this PARITY rewrite.
