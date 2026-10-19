# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

**不宣称全量对齐。** 当前：**0.6.0-dev**。

| 上游模块 | C++ 状态 | 备注 |
|----------|----------|------|
| Gateway methods | **部分** | 广告 **516**；**实现 ~89** |
| MCP | **部分** | in-proc mock + **stdio 子进程**（`MakeStdioClient` / `mcp-mock-server`）；`OPENCLAW_MCP_COMMAND` |
| WS events | **部分** | fan-out + **sessionKey / event 过滤** + recent 回放 |
| Context / pruning | **部分** | `context::Assemble`（system+history+tool 裁剪） / `Compact`；gateway chat 使用 Assemble；`sessions.compact` |
| Agents files | **部分** | `agents.files.list/get/set` 文件落盘 |
| Chat history | **部分** | `limit`/`offset`/`has_more` 分页 |
| Device pairing | **部分** | approve/reject/remove |
| Logs | **骨架** | `logs.tail` 内存环形缓冲 |
| Tools/shell/approvals/channels/cron/… | **部分** | 见 0.5 |
| Live IM / Control UI / ACP / crypto secrets | TODO | |

## 里程碑

- ~~0.2–0.5~~ …
- **0.6.0-dev**：MCP stdio 子进程、WS 订阅过滤、context assemble/compact、agents.files、history 分页、logs.tail、device.pair 深化
- 下一刀：真实 MCP 生态进程、订阅过滤下发到 WS 帧级、更多 channel 实装、exec 沙箱
