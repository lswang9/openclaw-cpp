# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

**不宣称全量对齐。** 当前：**1.2.0-dev**。持续把 catalog 缺口转为诚实的本地 **real** 实现。

## 术语

| 标签 | 含义 |
|------|------|
| **real** | 本地真实状态或实网副作用（`impl:"real"`） |
| **honest** | `impl:"honest"` 部分实现 |
| **stub** | `implemented:false` / `record_only` / `configured:false` |
| **catalog-only** | 广告 516 中未进 `ImplementedMethods` |

## 规模

| 指标 | 值 |
|------|-----|
| Catalog | **516** |
| `ImplementedMethods` | **285** |
| 1.2 增量 | Slack/Discord inbound；session export/import；backup restore；max_turns + rate_limit 进 agent loop |

## 模块

| 上游模块 | 状态 | 备注 |
|----------|------|------|
| Agent loop | **real（部分）** | `config.raw.agent.max_turns` / `rate_limit_per_minute` → `LoopOptions` + 进程内滑动窗口 |
| Sessions | **real（部分）** | `sessions.export` / `sessions.import`；fork/rewind/…（既有） |
| Backup | **real（部分）** | create（含消息正文）/ restore / list / status / recordOutcome |
| Feishu / Slack / Discord inbound | **real（部分）** | challenge/ping 校验 + 消息入队；verify token env 可选 |
| TG/Discord/Feishu outbound | **real \| stub** | env token → live_http |
| TTS / http_get / browser | **real \| stub** | 见 1.1 |
| computer / canvas / desktop / board | **stub** | 仍不伪造 |
| CI | **ready** | [docs/ci-github-actions.yml](docs/ci-github-actions.yml) — **不**推 `.github/workflows`（token 无 `workflow` scope） |

## 里程碑

- ~~0.2–1.1~~ …
- **1.2.0-dev**：Slack/Discord inbound；session export/import；backup restore；rate limit + max_turns 配置；ImplementedMethods 285
- 下一刀：继续 skills/users/sessions catalog；更深 IM；缩小 stub

`285` ≠ 上游行为等价。看响应 `impl`。
