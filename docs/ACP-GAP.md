# ACP (Agent Client Protocol) — gap vs OpenClaw 2026.10.5

Upstream OpenClaw supports **ACP agents** (external coding harnesses via ACP/acpx).
See upstream docs: `docs/tools/acp-agents.md`.

## openclaw-cpp status (1.5.1-dev)

| Piece | Status |
|-------|--------|
| `acp::Session` skeleton | **present** — in-memory session table, prompt queue |
| JSON-RPC ACP wire | **not implemented** |
| acpx / Claude Code / Codex ACP adapter | **not implemented** |
| Channel-bound ACP sessions | **not implemented** |

RPC `acp.status` returns this gap summary. Full ACP is a dedicated milestone after core Gateway/chat parity deepens.
