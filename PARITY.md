# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

目标：用 **C++20 + Conan** 全量重写官方 OpenClaw **2026.10.5** 行为（非 Node 包装）。
本地参考树可能是 2026.10.3；行为目标锁定 **2026.10.5**。

| 上游模块 | C++ 状态 | 备注 |
|----------|----------|------|
| CLI chat / agent loop | **部分** | `chat` + tool loop + mock self-test |
| Provider DeepSeek | **部分** | chat completions + tools；402/401/429 清晰错误 |
| Tools registry | **部分** | `echo`, `time_now`；`/tools/invoke` |
| Session store | **部分** | JSON 文件；session_id 挂到 chat API |
| Gateway HTTP | **部分** | `/health` `/v1/status` `/v1/models` `/v1/chat/completions` `/tools/invoke` |
| Gateway WebSocket | **部分** | Upgrade + hello + DispatchRpc（health/sessions/tools/config/chat.send/…） |
| Channels (WhatsApp/Telegram/…) | **骨架** | registry + `/v1/channels`；真实适配器 TODO |
| Memory / context-engine | TODO | |
| Plugins / skills | TODO | sibling: openclaw-cpp-tools |
| MCP | TODO | |
| Cron / hooks / daemon | TODO | |
| Nodes / companion apps | TODO | |
| Control UI | TODO | |
| Config / secrets | **骨架** | JSON config load/save；secrets TODO |
| Pairing / approvals | TODO | |
| Media / image / canvas | TODO | |
| ACP / alternate runtimes | TODO | |

## 近期里程碑

1. ~~第一刀：CLI → DeepSeek~~
2. **进行中**：Gateway HTTP/WS + agent loop + sessions + tools
3. 完整 Gateway JSON-RPC（对齐上游 method 集）
4. 至少一个真实 channel 适配器
5. Config/secrets + daemon 生命周期
6. Memory + plugins/skills 加载
7. 其余模块按上游 docs/src 逐项填绿

Sibling 仓库：`openclaw-cpp-gateway`（Gateway 深挖时可拆）、`openclaw-cpp-tools`（工具包）。
当前实现仍集中在 **openclaw-cpp** 核心仓以便迭代。
