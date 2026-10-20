# OpenClaw 2026.10.5 → openclaw-cpp

**Version: 1.8.0-dev · 未宣称与上游全量对齐。**

## 执行摘要（今晚可用什么）

| 层级 | 内容 |
|------|------|
| **今天可当真用（`impl:real`，仍是子集）** | Gateway HTTP+WS；mock/DeepSeek/OpenAI-compat 路由；agent loop（tools、max_turns、timeout、cancel、session 锁）；sessions/CRUD/export；tools（含 `http_get` / `browser_*`）；**CDP browser**（Chrome debugging 就绪时 `impl:real`）；MCP mock/stdio bridge + `mcp.app` list/call/resources；cron/persist/pairing；Control UI 骨架；IM **webhook 入站**（Feishu/Slack/Discord/WA/iMessage）与 **有 token 时的出站**；AES secrets（有 passphrase 时）；**ACP** `session/prompt`→AgentLoop（默认 mock）+ cancel；`scripts/integration-smoke.sh`（mock，无活 LLM） |
| **仿真 / 轻量（勿当生产自动化）** | `impl:real-sim` computer/canvas/desktop（占位 PNG + 事件日志）；browser curl-session 回退 / CDP mock；**无** Playwright |
| **本地占位 store（API 面可测，不是上游 UI）** | board / portal / wizard / openclaw.setup / mentions / … |
| **明确缺失** | ACP **acpx**/通道绑定/多模态；JS 插件宿主；真实 OS/桌面/无障碍树；完整 Socket Mode/Baileys；board 真 UI；`.github/workflows` 直接推送（见 docs CI） |

## Impl 标签

| `impl` | 含义 |
|--------|------|
| **real** | 本地状态和/或真实 HTTP（视 env） |
| **real-sim** | `SimulationBackend`，非生产 computer-use |
| **honest** | 少见的部分实现 |
| **implemented:false** | 故意不伪造（debugProxy / claws / nativeHook / web.login / attach 等） |

## 规模（计数 ≠ 对齐度）

| 指标 | 约值 |
|------|------|
| Catalog 广告 | 516 |
| `ImplementedMethods` | ~640（大量前缀桶/本地 store） |

详见里程碑 [SERIES.md](SERIES.md)；发版流程 [docs/RELEASE.md](docs/RELEASE.md)；上手 [STATUS.md](STATUS.md)。
