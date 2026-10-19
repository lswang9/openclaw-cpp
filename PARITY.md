# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

目标：用 **C++20 + Conan** 全量重写官方 OpenClaw **2026.10.5**（非 Node 包装）。
**不宣称全量对齐。** 当前：**0.4.0-dev**。

| 上游模块 | C++ 状态 | 备注 |
|----------|----------|------|
| Gateway method catalog | **部分** | 广告 **516**；**实现 72**（含 sessions/chat/agents/skills/cron/config/approvals/secrets/channels.*）；其余 structured stub |
| Gateway events | **部分** | 广告 62；主要 `connect.challenge` |
| Gateway HTTP/WS | **部分** | 既有 REST + WS RPC；新增 `/api/channels/{id}/inbound`、`/v1/adapters`、`/v1/approvals` |
| Streaming chat | **部分** | SSE `stream:true` |
| Auth | **部分** | `OPENCLAW_GATEWAY_TOKEN` |
| Pairing | **骨架** | in-memory + pairing RPCs |
| Channel adapters | **部分** | **WebhookAdapter** 真入站→agent loop；telegram/discord/slack/feishu/whatsapp = StubAdapter+TODO |
| Agent loop + tools | **部分** | tool_calls；echo/time_now/memory_* |
| Provider DeepSeek | **部分** | sync+stream；402 等 |
| Session | **部分** | create/get/list/delete/reset/compact/patch/send/subscribe |
| Skills | **部分** | SKILL.md 扫描；list/read/detail/search |
| Plugins | **骨架** | 目录扫描；setEnabled stub |
| Agents | **部分** | list/create/update/delete（持久化到 config.raw） |
| Config | **部分** | get/set/apply/patch/schema + 落盘 |
| Memory | **部分** | get/status/search 真读 Store |
| Cron | **部分** | add/update/remove/list/run + 表达式 Tick |
| Approvals | **部分** | approval/exec/plugin.approval list/request/resolve |
| Secrets | **部分** | secrets.store list/set/delete（仅列 key，不回显 value） |
| MCP | **骨架** | `mcp::Client` stdio JSON-RPC + mock self-test；gateway `mcp.app.listTools` 占位 |
| Control UI / Nodes / Media / ACP | TODO | |

## 里程碑

1. ~~第一刀 DeepSeek CLI~~ → ~~agent/gateway 0.2~~ → ~~0.3 catalog/auth/skills/stream/cron~~
2. **0.4.0-dev（本切片）**：RPC 深化至 72、channel adapter 框架+webhook 入站、MCP client、approvals/secrets
3. 下一刀：真实 IM 适配器、MCP↔tools 桥、事件推送、secrets 加密、更多 RPC 实装

Sibling 仓仍为占位；实现集中在本仓。
