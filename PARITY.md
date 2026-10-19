# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

**不宣称全量对齐。** 当前：**1.0.0-dev**。

## 术语（强制区分）

| 标签 | 含义 |
|------|------|
| **real** | 本地真实状态读写或实网副作用（`impl:"real"`）。 |
| **honest** | 明确部分实现：`impl:"honest"` + note，不假装与上游等价。 |
| **stub** | 占位 / 无 token 出站 `record_only` / 明确 `implemented:false`（不假装已实现）。 |
| **catalog-only** | 广告表 516 中未进 `ImplementedMethods`。 |

> **不要**用空 stub 膨胀计数。1.0 把原 honest 桶尽量改为带本地 state 的 **real**。

## 规模

| 指标 | 值 |
|------|-----|
| Catalog | **516** |
| `ImplementedMethods` | **237** |
| ExtraDispatch `Real(...)` 路径 | ~多数前缀（users/plugins/sessions/agents/channels/cron/memory/themes/backup/fs/audit/media/…） |
| ExtraDispatch `Honest(...)` | 极少 residual catch-all |
| 故意不伪实现 | computer / canvas / webSearch / browser / tts / wizard / … → `implemented:false` |

## 模块表

| 上游模块 | 状态 | 备注 |
|----------|------|------|
| Gateway HTTP+WS | **real（部分）** | health/status/chat SSE、WS RPC、media upload/serve |
| Daemon lifecycle | **real** | pidfile、`gateway start\|stop\|status`、SIGINT/SIGTERM 优雅停 |
| Chat + agent loop | **real（部分）** | tools、loop guard、多 provider |
| Sessions / transcripts | **real（部分）** | fork/rewind/resolve/catalog/storage/files/goal/groups/branches/meta |
| Media pipeline | **real** | `POST/GET /v1/media`、`media.*` RPC、落盘按 id 取回 |
| Tools / MCP | **real（部分）** | registry + mock/stdio bridge |
| Plugins | **real（部分）** | `plugin.json` scan + Linux `.so` via [docs/PLUGIN-SDK.md](docs/PLUGIN-SDK.md)；非 JS 宿主 |
| Provider routing | **real** | list / setDefault |
| Control UI | **real（骨架）** | sessions/tools/SSE（0.9） |
| Secrets | **real（部分）** | AES-GCM iff passphrase |
| Cron / persist / pairing / nodes | **real（部分）** | StateStore |
| Channels outbound | **real \| stub** | token→live_http；否则 record_only |
| ACP | **stub / gap** | 内存骨架；见 [docs/ACP-GAP.md](docs/ACP-GAP.md) |
| Browser / computer / TTS / canvas | **stub** | `implemented:false`，不伪造 |
| Full JS plugin host / OAuth IM / UI WS | TODO | |

## 里程碑

- ~~0.2–0.9~~ …
- **1.0.0-dev**：honest→real 本地状态加深；daemon pidfile/start/stop；Plugin C SDK + sample `.so`；media 上传/按 id 服务；PARITY 三分法
- 下一刀：更多核心 RPC 行为对齐；Control UI WS；ACP wire 或继续明确延后

`ImplementedMethods=237` ≠ 上游 237 API 行为等价。看响应 `impl` 与本文档。
