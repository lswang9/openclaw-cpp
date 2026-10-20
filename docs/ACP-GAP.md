# ACP (Agent Client Protocol) — gap vs OpenClaw 2026.10.5

Upstream OpenClaw supports **ACP agents** (external coding harnesses via ACP/acpx).
Canonical protocol: [Agent Client Protocol](https://agentclientprotocol.com/) (JSON-RPC, typically stdio).

## openclaw-cpp status (**1.7.0-dev**)

| Piece | Status | `impl` |
|-------|--------|--------|
| JSON-RPC ACP **wire subset** (newline stdio / in-process) | **present** — `initialize`, `session/new`, `session/list`, `session/prompt`, `session/cancel`, `session/close`, `session/resume`, `ping` | **real** |
| In-process client + agent (`acp::Client::MakeInProcessPair`) | **present** — used by self-test | **real** |
| CLI `acp-stdio` agent | **present** — line-delimited JSON-RPC on stdin/stdout | **real** |
| Gateway `acp.status` / `acp.agents.list` / `acp.sessions.*` / `acp.wire` | **present** — bridges to in-process `AgentEndpoint` | **real** (subset) |
| Prompt → `session/update` notifications | **present** — mock agent echo reply (not live LLM harness) | **real** (honest mock turn) |
| acpx / Claude Code / Codex ACP adapter | **not implemented** | **missing** |
| Channel-bound ACP sessions | **not implemented** | **missing** |
| Multimodal prompt (image/audio/embedded), session MCP | **not advertised / not implemented** | **missing** |
| Auth (`auth/login`) | **not implemented** (`authMethods: []`) | **missing** |

### How to try

```bash
# In-process: self-test covers initialize → session/new → session/prompt
./build/build/Release/openclaw-cpp self-test

# Stdio agent (speak ACP as the *agent* side):
./build/build/Release/openclaw-cpp acp-stdio
# then send newline-delimited JSON-RPC on stdin (initialize, session/new, …)

# Gateway pass-through:
# POST /v1/rpc  method=acp.wire  params={"request":{...json-rpc...}}
```

### Honest limits

This is a **working ACP v2 subset** so clients can negotiate and drive a session without shipping acpx.
It is **not** OpenClaw’s full ACP agent hosting (no external harness spawn, no channel binding, no production coding-agent runtime).
