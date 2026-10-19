# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

**不宣称全量对齐。** 当前：**0.7.0-dev**。

| 上游模块 | C++ 状态 | 备注 |
|----------|----------|------|
| Gateway methods | **部分** | 广告 **516**；**实现 ~101**（含 doctor/diagnostics、models、cron.get/history、memory.set、chat.startup、sessions.usage/abort、skills.bins/install） |
| Multi-provider | **部分** | `Router` + `OpenAiCompatProvider`（deepseek/openai）+ `mock`；`OPENCLAW_DEFAULT_PROVIDER` |
| Persist | **部分** | `OPENCLAW_DATA_DIR` 下 cron/pairing/nodes JSON 落盘 |
| Tool loop guard | **部分** | 相同 tool+args 连续调用 soft-warn / break |
| Doctor / health | **部分** | `diagnostics.stability`、`doctor.memory.status` |
| MCP / WS / context / channels / … | **部分** | 见 0.5–0.6 |
| Live IM / Control UI / ACP / sandbox exec | TODO | |

## 里程碑

- ~~0.2–0.6~~ …
- **0.7.0-dev**：多 provider 路由、状态持久化、重复 tool 软熔断、doctor/models/更多 RPC（~101）
- 下一刀：更多 RPC 实装、provider 热切换 API、持久化 approvals/secrets、真实 channel 发送
