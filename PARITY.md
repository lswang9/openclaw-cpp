# OpenClaw 2026.10.5 → openclaw-cpp

**Version: 1.35.0-dev · 未宣称与上游全量对齐。**

## 执行摘要（今晚可用什么）

| 层级 | 内容 |
|------|------|
| **今天可当真用（`impl:real`，仍是子集）** | Gateway HTTP+WS；mock/DeepSeek/OpenAI-compat 路由；agent loop（tools、max_turns、timeout、cancel、session 锁）；sessions/CRUD/export；**workspace 文件工具 read/write/edit（沙箱化：拒绝 `..`/绝对路径/符号链接逃逸）** + **Markdown 记忆（MEMORY.md + memory/*.md，memory_search 为**混合检索**（0.7×本地向量 + 0.3×BM25，`mode` 可选 bm25/vector；向量为离线特征哈希嵌入：词 + 字符三元组 + CJK 单字，**非神经模型**）、memory_get 按行取、memory_set 持久化）**，`impl:real`；**系统提示词组装**（AGENTS/SOUL/TOOLS/IDENTITY/USER/HEARTBEAT/BOOTSTRAP/MEMORY.md 注入，单文件 20k 截断）+ **技能加载优先级**（workspace > ~/.openclaw/skills > bundled > extra）+ **gating**（requires.bins/anyBins/env、os、disable-model-invocation）+ `<available_skills>` 注入；**`skills.entries.<key>`**（enabled、env、apiKey→primaryEnv，满足 env gating，agent 运行期间注入且不覆盖已有变量）+ **技能斜杠命令**（`/<规范化名>` 或 `/skill <名> [参数]`，`user-invocable: false` 排除），`impl:real`（env 注入为进程级，并发 run 之间不隔离）；`skills list|check|info`、`context [prompt]` CLI；`chat` 支持 mock provider；**配置文件**：路径 `OPENCLAW_CONFIG_PATH` > `~/.openclaw/openclaw.json`（兼容旧 `~/.openclaw-cpp/config.json`），**JSON5 子集**解析（注释/尾逗号/无引号键/单引号/hex）、`${ENV}` 替换（缺失报错，`$${}` 转义）、`config get/set/unset/file/validate` 点路径 CLI、原子写+.bak，`impl:real`；**`$include`**（字符串/数组、相对路径、嵌套≤10、环检测、深合并后本地覆盖；写回时保留 `$include` 只写差异键）；**schema 校验**（已知根键 + gateway/agents.defaults/session.reset*/skills.entries 等的类型与取值；类型错误拒绝加载、网关拒绝启动，未知键为警告）；**热重载**（网关每次 RPC/入站最多每秒检查 mtime，非法配置被拒且保留旧配置，`gateway.reload.mode: off` 关闭，host/port 变化提示需重启，`config.reload` RPC），`impl:real`（schema 只覆盖部分子树，非上游完整 zod schema；热重载不重建已加载的渠道/插件/系统提示词）；**会话生命周期**：`sessions.json` 索引（sessionId/createdAt/updatedAt/resets）、会话键无损编码（修复 `a:b`≡`ab` 冲突，兼容旧文件）、`/new` `/reset`（可带后续消息）归档旧 transcript、`session.reset` 每日 atHour(默认 4 点)/idleMinutes 过期，`impl:real`（transcript 已为上游式 **JSONL**（session 头 + message 行 + parentId 链，旧 JSON 自动迁移为 `.migrated`）；网关只持久化**完整未裁剪**历史 + 本轮新消息（不再写入 pruned 视图/系统提示词）；**`session.resetByChannel` > `resetByType`(direct/group/thread) > `reset`** 已实现（chat_type 由 webhook `chat_type` 或会话键推断）；）；**compaction**：超过 `contextTokens - compaction.reserveTokens`（默认 64k/16k，按 chars/4 估算）时用模型把旧轮次总结成一条摘要消息，保留尾部且不从孤立 tool 结果开始，失败回退截断；`/compact [指示]` 手动压缩，`impl:real`；**compaction 前 memory flush**：自动压缩前跑一次静默 agent 轮（可用工具写 memory/*.md，输出不入 transcript，`compaction.memoryFlush.enabled/prompt/systemPrompt`）`impl:real`（仅单元测配置，网关路径无自动化端到端测试）；精确 tokenizer **missing**；多数渠道会话键非上游格式，chat_type 推断常为 direct）（远程/神经 embedding provider、sqlite-vec 索引与嵌入缓存 **missing**）；tools（含 `http_get` / `browser_*`）；**CDP browser**（Chrome debugging 就绪时 `impl:real`）；MCP mock/stdio bridge + `mcp.app` list/call/resources；cron/persist/pairing；Control UI 骨架；IM **webhook 入站**（Feishu/Slack/Discord/**WA Cloud**/iMessage）+ **Slack Socket Mode** + **Discord Gateway** + **Feishu 长连接**（mock/feed 或 app 凭证）与 **有 token 时的出站**；AES secrets（有 passphrase 时）；**JS 插件薄桥**（Node stdio / mock → `js.sample_echo`）；**ACP** `session/prompt`→AgentLoop + cancel + **channel bind** + **multimodal image**；`scripts/integration-smoke.sh`（mock，无活 LLM） |
| **仿真 / 轻量（勿当生产自动化）** | `impl:real` computer.screenshot/click/type/move（display+ffmpeg/xdotool）；canvas 本地磁盘 `impl:real`（非 A2UI）；desktop launch 等仍 `real-sim`；browser curl-session 回退 / CDP mock；**无** Playwright |
| **本地占位 store（API 面可测，不是上游 UI）** | **board** 已有磁盘 JSON + `/board` HTML CRUD；portal / wizard / openclaw.setup / mentions 仍偏占位 |
| **明确缺失** | ClawHub 技能安装/同步、技能 per-skill config/env 注入、技能 slash 命令；memory 向量/embedding 检索与 compaction 前自动 flush；ACP **acpx**/OAuth/音视频（通道绑定+多模态+session MCP+token 门禁子集已有）；完整 JS/TS 插件宿主（仅有 Node stdio 薄桥 + sample_js）；真实 OS/桌面/无障碍树；完整 Slack interactive；**Baileys**（仅有 Cloud API 子集）；Discord slash/voice；Feishu 专有 SDK 帧；Socket/Gateway/长连接仅为消息子集；上游级 board/canvas/A2UI（仅有本地看板+本地 canvas）；上游云端设备链接/QR/scope token（仅有本地配对码流程 `impl:real`；ocd_ token 可用于网关鉴权，仅两种固定角色（operator/device 拒绝列表），非细粒度 scope；限流仅进程内；/login 为服务端会话 + CSRF token + WS Origin 检查；限流按 TCP 对端 IP 持久化，不信任代理头，无多实例共享；无 OIDC/MFA）；`.github/workflows` 直接推送（见 docs CI） |

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
