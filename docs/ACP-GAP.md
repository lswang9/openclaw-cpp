# ACP (Agent Client Protocol) — gap vs OpenClaw 2026.10.5

Upstream OpenClaw supports **ACP agents** (external coding harnesses via ACP/acpx).
Canonical protocol: [Agent Client Protocol](https://agentclientprotocol.com/) (JSON-RPC, typically stdio).

## openclaw-cpp status (**1.8.0-dev**)

| Piece | Status | `impl` |
|-------|--------|--------|
| JSON-RPC ACP **wire subset** | **present** — `initialize`, `session/new\|list\|prompt\|cancel\|close\|resume`, `ping` | **real** |
| **`session/prompt` → `RunAgentLoop`** | **present** — default **mock** provider (`OPENCLAW_PROVIDER` / `OPENCLAW_DEFAULT_PROVIDER`); reply via `session/update` `agent_message` | **real** |
| **`session/cancel` → loop cancel** | **present** — cooperative `std::atomic` flag into `LoopOptions.cancel` | **real** |
| In-process client + `acp-stdio` | **present** | **real** |
| Gateway `acp.*` / `acp.wire` | **present** | **real** (subset) |
| Live LLM via ACP (DeepSeek/OpenAI) | injectable via `SetLoopBackend` / env naming; **CLI stdio still defaults mock** unless wired | **honest** / optional |
| acpx / Claude Code / Codex adapters | **not implemented** | **missing** |
| Channel-bound ACP sessions | **not implemented** | **missing** |
| Multimodal prompt / session MCP / auth | **not implemented** | **missing** |

### Env

| Variable | Role |
|----------|------|
| `OPENCLAW_PROVIDER` / `OPENCLAW_DEFAULT_PROVIDER` | Backend label (default `mock`) |
| `OPENCLAW_ACP_TURN_DELAY_MS` | Test-only delay before loop (cancel races) |

### Honest limits

Prompt runs the **same** C++ agent loop as chat, but the default provider is **mock** so CI/self-test need no API keys.
This is **not** full OpenClaw ACP hosting (no acpx spawn, no channel binding).
