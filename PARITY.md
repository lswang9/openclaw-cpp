# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

**不宣称全量对齐。** 当前：**1.1.0-dev**。目标仍是随时间覆盖上游功能，诚实标注缺口。

## 术语

| 标签 | 含义 |
|------|------|
| **real** | 本地真实状态或实网副作用（`impl:"real"`） |
| **honest** | `impl:"honest"` 部分实现 |
| **stub** | 占位 / `record_only` / `implemented:false` / `configured:false` — **不假装已实现** |
| **catalog-only** | 广告 516 中未进 `ImplementedMethods` |

## 规模

| 指标 | 值 |
|------|-----|
| Catalog | **516** |
| `ImplementedMethods` | **266** |
| 1.1 相对 1.0 | +TTS/voice、http_get、Feishu inbound、更多 controlUi/catalog/agent/… 本地 real |

## 模块

| 上游模块 | 状态 | 备注 |
|----------|------|------|
| Gateway / daemon / media / plugins `.so` | **real（部分）** | 见 1.0 |
| TTS / talk / voicewake | **real \| stub** | `OPENCLAW_TTS_URL`(+API_KEY) → HTTP；否则 `record_only` |
| Browser tool | **stub** | `browser` → `not configured`；**real** `http_get` curl 拉取 |
| Feishu | **real（部分）** | outbound webhook URL；inbound challenge 校验 + 消息入队 agent |
| Telegram / Discord outbound | **real \| stub** | env token → live_http |
| computer / canvas / desktop / board | **stub** | `implemented:false` |
| ACP | **stub / gap** | [docs/ACP-GAP.md](docs/ACP-GAP.md) |
| CI | **ready** | Workflow YAML in [docs/ci-github-actions.yml](docs/ci-github-actions.yml); installing under `.github/workflows/` needs a GitHub token with `workflow` scope (OAuth push rejected without it) |

## 里程碑

- ~~0.2–1.0~~ …
- **1.1.0-dev**：catalog real 扩充；TTS HTTP/record；http_get + browser stub；Feishu inbound；GitHub Actions CI
- 下一刀：继续把 catalog-only 变为 real 本地行为；更深 channel/IM；缩小 stub 面

`266` ≠ 上游 266 API 行为等价。看 `impl` 与本文。
