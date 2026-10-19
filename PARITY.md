# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

**不宣称全量对齐。** 当前：**0.9.0-dev**（基线 commit 目标：本推送）。

## 术语（强制区分）

| 标签 | 含义 |
|------|------|
| **real** | 本地真实状态读写或实网副作用（`impl:"real"`）。会改 session/config/store，或在有 env token 时真正 HTTP 出站。 |
| **honest** | 明确部分实现：返回本地可见 state + `impl:"honest"` + note，**不假装**与上游 2026.10.5 行为一致。 |
| **stub** | 仅占位/记录（如无 token 时的 outbound `mode:record_only`），或文档化缺口（ACP wire）。 |
| **catalog-only** | 出现在上游方法广告表（516）但未列入 `ImplementedMethods`，网关不处理。 |

> 规则：**不要**用空 stub 膨胀 `ImplementedMethods` 计数。0.9 优先把 honest 加深为 real 本地逻辑。

## 规模

| 指标 | 值 |
|------|-----|
| Catalog（上游广告） | **516** |
| `ImplementedMethods` | **206**（含 acp.*） |
| ExtraDispatch `Real(...)` 专用路径 | ~43 处（providers / users / plugins / update / sessions.* extras / transcripts / ACP / presence / …） |
| ExtraDispatch `Honest(...)` 回退桶 | ~13 处前缀桶（themes/backup/computer/… 与未加深的 session/agents/channels/…） |
| 核心 `DispatchRpc` 深路径 | chat/sessions/tools/cron/memory/nodes/pairing/approvals/secrets/mcp/…（多数 **real**） |

## 模块表

| 上游模块 | 状态 | 备注 |
|----------|------|------|
| Gateway HTTP+WS | **real（部分）** | `:18789` health/status/models/methods/chat SSE、WS JSON-RPC、event bus + SubFilter |
| Chat + agent loop | **real（部分）** | tool_calls、identical-tool soft-warn、context assemble/compact、多 provider |
| Sessions / transcripts | **real（部分）** | list/get/create/delete/reset/compact/search/cleanup/files/goal/groups；Control UI 列表 |
| Tools | **real（部分）** | registry + MCP bridge；`GET /v1/tools`；UI chips |
| Provider routing | **real** | `providers.list` / `setDefault` / `models.setDefault` |
| Control UI | **real（骨架加深）** | sessions 列表、tools 列表、SSE 流式显示、health/status |
| Secrets | **real（部分）** | AES-256-GCM iff `OPENCLAW_SECRETS_PASSPHRASE` |
| Cron / persist / pairing / nodes | **real（部分）** | 落盘 StateStore |
| Channels outbound TG/Discord/Feishu | **real \| stub** | env token → `live_http`（libcurl）；无 token → `record_only`（graceful） |
| Channels inbound webhooks | **real（部分）** | `/api/channels/<id>/inbound` |
| MCP | **real（部分）** | in-proc mock + stdio child + bridge→tools |
| Skills | **real（部分）** | SKILL.md 扫描；update/import 仍诚实说明走文件系统 |
| ACP / agent-protocol | **stub / gap** | `acp::Registry` 内存会话 + RPC；**无** ACP wire / acpx。见 [docs/ACP-GAP.md](docs/ACP-GAP.md) |
| Themes / canvas / computer / TTS / wizard / … | **honest** | ExtraDispatch 前缀桶，带本地 state 快照 |
| Full sandbox / OAuth IM login / Control UI WS | TODO | |

## 里程碑

- ~~0.2–0.8~~ …
- **0.9.0-dev**：加深 ExtraDispatch 为 real 本地状态；Control UI sessions/SSE/tools；ACP skeleton + gap doc；Discord/Telegram/Feishu curl 出站（无 token 优雅 no-op）；PARITY 三分法
- 下一刀：更多 honest→real；Control UI WS 订阅；ACP wire 或明确延后；出站投递回执与重试

诚实说明：`ImplementedMethods=206` ≠ 上游 206 个 API 行为等价。看响应里的 `impl` 字段与本文档。
