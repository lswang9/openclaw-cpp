# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

**不宣称全量对齐。** 当前：**1.3.0-dev**。

## 术语

| 标签 | 含义 |
|------|------|
| **real** | 本地真实状态或实网副作用（`impl:"real"`） |
| **honest** | `impl:"honest"` 残余 catch-all |
| **stub** | `implemented:false` / not-configured / record_only — 不假装 |
| **catalog-only** | 广告表中仍未列入 `ImplementedMethods` 的少数项（board/computer/canvas/…） |

## 规模

| 指标 | 值 |
|------|-----|
| Catalog（广告） | **516**（`AdvertisedMethodCount`） |
| `ImplementedMethods` | **549**（含既有前缀桶可服务的上游名 + 本地扩展） |
| 仍明确 stub | computer / canvas / desktop / board / portal / wizard / openclaw.* / mcp.app.* / browser 等 |

> 549 多数走已有 **real** 前缀处理器（skills/users/sessions/node/…），不是空 stub 膨胀。

## 1.3 增量

| 项 | 状态 |
|----|------|
| WhatsApp / iMessage inbound | **real（部分）** — `SimpleInboundAdapter`；`WHATSAPP_VERIFY_TOKEN` / `IMESSAGE_VERIFY_TOKEN` |
| logs.export / otel.export | **real（cheap）** — JSONL under data/exports；`OTEL_EXPORTER_OTLP_ENDPOINT` 仅标记 best-effort（无完整 OTLP protobuf） |
| Control UI | sessions/tools、**tool invoke**、**session export**（`GET /v1/sessions/:id/export`） |
| Catalog 覆盖 | 大批量 skills/users/sessions/… 纳入 ImplementedMethods |

## CI

YAML 仅 [docs/ci-github-actions.yml](docs/ci-github-actions.yml)。**不**推 `.github/workflows`（token 无 `workflow` scope）。

## 里程碑

- ~~0.2–1.2~~ …
- **1.3.0-dev**：catalog real 覆盖大幅上升；WA/iMessage inbound；log/otel 文件导出；Control UI invoke+export
- 下一刀：真正 OTLP、outbound WhatsApp、继续削 stub 面

`549` ≠ 上游行为逐 API 等价。看 `impl`。
