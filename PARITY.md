# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

**不宣称全量对齐。** 当前：**0.5.0-dev**。

| 上游模块 | C++ 状态 | 备注 |
|----------|----------|------|
| Gateway methods | **部分** | 广告 **516**；**实现 ~82** |
| Gateway events / WS fan-out | **部分** | `events::Bus` + 多 WS 客户端广播 `session.message` / `chat` / `session.tool` |
| MCP | **部分** | Client + **RegisterMcpTools → `mcp.*` in tool registry** + mock self-test |
| Tools + approvals | **部分** | `shell` 工具默认需 approval；mock 执行 |
| Channel adapters | **部分** | webhook 入站；**telegram/feishu outbound 记录**；discord/slack/whatsapp stub |
| Nodes | **骨架→部分** | `node.list/describe/rename` in-memory |
| Media / artifacts | **骨架** | 本地 Put/List/Get；`message.action` media.upload |
| Config validation | **部分** | `ValidateLight` + schema.lookup |
| Streaming / auth / skills / cron / sessions / agents | **部分** | 延续 0.4 |
| Control UI / live IM APIs / secrets crypto / ACP | TODO | |

## 里程碑

- ~~0.2–0.4~~ agent/gateway/catalog/adapters/MCP skeleton/approvals
- **0.5.0-dev**：MCP→tools 桥、WS 事件扇出、shell 审批门、TG/飞书出站记录、nodes/media、config 轻校验
- 下一刀：真实 channel HTTP 发送、更多 RPC、事件订阅过滤、exec 真沙箱策略
