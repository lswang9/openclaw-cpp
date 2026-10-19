# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

目标：用 **C++20 + Conan** 全量重写官方 OpenClaw **2026.10.5** 行为（非 Node 包装）。
**不宣称已完成全量对齐。** 当前版本：**0.3.0-dev**。

| 上游模块 | C++ 状态 | 备注 |
|----------|----------|------|
| Gateway method catalog | **部分** | 广告 **516** 个上游方法名；**36** 个有真实/半真实实现；其余返回结构化 `stub` |
| Gateway events catalog | **部分** | 广告 **62** 个上游事件名；目前主要发 `connect.challenge` |
| Gateway HTTP | **部分** | `/health` `/v1/status|models|methods|channels|config|skills` `/v1/chat/completions` `/tools/invoke` |
| Gateway WebSocket | **部分** | Upgrade + DispatchRpc；未实现方法 → structured stub |
| Streaming chat | **部分** | `POST /v1/chat/completions` + `stream:true` → SSE（DeepSeek）；需 API key/余额 |
| Auth | **部分** | `OPENCLAW_GATEWAY_TOKEN` → Bearer / `X-OpenClaw-Token`；`/health` 豁免 |
| Pairing | **骨架** | in-memory PairingStore；`channels.pairing.*` / `node.pair.list` / `device.pair.list` |
| Agent loop + tools | **部分** | tool_calls 循环；`echo` `time_now` `memory_*` |
| Provider DeepSeek | **部分** | sync + stream；402/401/429 |
| Session store | **部分** | JSON 文件；sessions.create/get/list/delete/reset/preview |
| Skills | **部分** | 扫描 `*/SKILL.md` frontmatter；示例 `skills/examples/hello-skill` |
| Plugins | **骨架** | 扫描 `plugins/*/plugin.json` |
| Channels | **骨架** | registry（cli/http/ws 启用；IM 适配器 TODO） |
| Config | **骨架** | JSON load/save；`config.get` / `config.schema` |
| Memory | **骨架** | JSON KV + tools；`memory.*` RPC 多为 stub |
| Cron | **部分** | 5 字段表达式匹配 + Tick 去重 + `cron-daemon`；`cron.add/remove/list` |
| Agents | **骨架** | `agents.list` 返回 main |
| Control UI / Nodes / MCP / Media / ACP | TODO | |
| Secrets / Approvals / Daemon install | TODO | |

## 里程碑

1. ~~CLI → DeepSeek~~
2. ~~Agent loop + Gateway HTTP/WS 初版~~
3. ~~Channels/config/memory/cron 骨架~~
4. **进行中 (0.3.0-dev)**：完整方法目录广告、auth、skills、streaming、cron eval
5. 下一刀：真实 channel 适配器、更多 RPC 实装、secrets/approvals、Control UI 协议

详见 [SERIES.md](SERIES.md)。
