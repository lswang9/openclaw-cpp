# OpenClaw 2026.10.5 → openclaw-cpp

**Version: 1.54.0-dev · 未宣称与上游全量对齐。**

## 执行摘要（今晚可用什么）

| 层级 | 内容 |
|------|------|
| **今天可当真用（`impl:real`，仍是子集）** | Gateway HTTP+WS；mock/DeepSeek/OpenAI-compat 路由；agent loop（tools、max_turns、timeout、cancel、session 锁）；sessions/CRUD/export；**workspace 文件工具 read/write/edit（沙箱化：拒绝 `..`/绝对路径/符号链接逃逸）** + **Markdown 记忆（MEMORY.md + memory/*.md，memory_search 为**混合检索**（0.7×本地向量 + 0.3×BM25，`mode` 可选 bm25/vector；向量为离线特征哈希嵌入：词 + 字符三元组 + CJK 单字，**非神经模型**）、memory_get 按行取、memory_set 持久化）**，`impl:real`；**系统提示词组装**（AGENTS/SOUL/TOOLS/IDENTITY/USER/HEARTBEAT/BOOTSTRAP/MEMORY.md 注入，单文件 20k 截断）+ **技能加载优先级**（workspace > ~/.openclaw/skills > bundled > extra）+ **gating**（requires.bins/anyBins/env、os、disable-model-invocation）+ `<available_skills>` 注入；**`skills.entries.<key>`**（enabled、env、apiKey→primaryEnv，满足 env gating，agent 运行期间注入且不覆盖已有变量）+ **技能斜杠命令**（`/<规范化名>` 或 `/skill <名> [参数]`，`user-invocable: false` 排除）+ **`command-dispatch: tool`**（`command-tool`/`command-arg-mode: raw`，斜杠命令直接调用工具，参数 `{command, commandName, skillName}`，不经模型），`impl:real`（env 注入为**每次运行线程局部覆盖层**，不修改进程环境，并发 run 互相隔离；子进程经 RunProcess 获得 environ+覆盖层）；`exec`/`shell` 工具默认真实执行（1.48 起，按上游 exec 审批/沙箱语义，见下文 1.48）；`skills list|check|info`、`context [prompt]` CLI；`chat` 支持 mock provider；**配置文件**：路径 `OPENCLAW_CONFIG_PATH` > `~/.openclaw/openclaw.json`（兼容旧 `~/.openclaw-cpp/config.json`），**JSON5 子集**解析（注释/尾逗号/无引号键/单引号/hex）、`${ENV}` 替换（缺失报错，`$${}` 转义）、`config get/set/unset/file/validate` 点路径 CLI、原子写+.bak，`impl:real`；**`$include`**（字符串/数组、相对路径、嵌套≤10、环检测、深合并后本地覆盖；写回时保留 `$include` 只写差异键）；**schema 校验**（已知根键 + gateway(port/bind/reload/auth)、agents.defaults(model/compaction/memorySearch/timeouts)、agents.list、bindings、session(reset*/dmScope/keyFormat)、channels.<id>(enabled/dmPolicy/groupPolicy/allowFrom/token 类)、tools(allow/deny/profile/exec)、plugins(entries/allow/deny)、models.providers、skills.entries、logging/cron/hooks 的类型与取值；类型错误拒绝加载、网关拒绝启动，未知键为警告）；**热重载**（网关每次 RPC/入站最多每秒检查 mtime，非法配置被拒且保留旧配置，`gateway.reload.mode: off` 关闭，host/port 变化提示需重启，`config.reload` RPC），`impl:real`（schema 覆盖主要子树但仍非上游完整 zod schema，渠道私有字段/深层嵌套多未校验；热重载现会**重扫技能并重建系统提示词**、**重启 channels.<id> 子树变化的运行中适配器**、plugins 子树变化时**重扫插件并重启 JS 宿主**（提示词重建有 smoke 覆盖；渠道/插件重启由 1.49 `scripts/e2e-hot-reload-channels.sh` 覆盖））；`OPENCLAW_MOCK_SCRIPT` 脚本化 mock provider 用于离线 e2e；**会话生命周期**：`sessions.json` 索引（sessionId/createdAt/updatedAt/resets）、会话键无损编码（修复 `a:b`≡`ab` 冲突，兼容旧文件）、`/new` `/reset`（可带后续消息）归档旧 transcript、`session.reset` 每日 atHour(默认 4 点)/idleMinutes 过期，`impl:real`（transcript 已为上游式 **JSONL**（session 头 + message 行 + parentId 链，旧 JSON 自动迁移为 `.migrated`）；网关只持久化**完整未裁剪**历史 + 本轮新消息（不再写入 pruned 视图/系统提示词）；**`session.resetByChannel` > `resetByType`(direct/group/thread) > `reset`** 已实现（chat_type 由 webhook `chat_type` 或会话键推断）；）；**compaction**：超过 `contextTokens - compaction.reserveTokens`（默认 64k/16k，按 chars/4 估算）时用模型把旧轮次总结成一条摘要消息，保留尾部且不从孤立 tool 结果开始，失败回退截断；`/compact [指示]` 手动压缩，`impl:real`；**compaction 前 memory flush**：自动压缩前跑一次静默 agent 轮（可用工具写 memory/*.md，输出不入 transcript，`compaction.memoryFlush.enabled/prompt/systemPrompt`）`impl:real`（`scripts/e2e-memory-flush.sh` 端到端：脚本化 mock provider 经真实网关触发 flush→write memory/flush.md→摘要压缩，flush 轮不入 transcript；已并入 smoke）；**tokenizer**：自研字节级 BPE + 手写 cl100k 预分词，加载 tiktoken 格式词表（`OPENCLAW_TOKENIZER_PATH` 或 `~/.openclaw/tokenizers/cl100k_base.tiktoken`，`tokenizer fetch` 下载），在样例与仓库文件上与 tiktoken 计数一致，`impl:real`（词表未随仓库分发，无词表时回退 chars/4；cl100k ≠ DeepSeek 自有分词器，仍是近似；Unicode 字母类为近似实现）；渠道会话键已改为上游格式（见下））；**上游会话键**：`agent:main:main`（`session.dmScope`=main 默认 / per-peer `agent:main:dm:<peer>` / per-channel-peer）、群 `agent:main:<ch>:group:<id>`、频道 `:channel:<id>`、线程/话题追加 `:thread:<id>`；Slack(channel_type/thread_ts)、Discord(guild_id)、Feishu(chat_type/thread_id)、Telegram 更新格式(chat.type/topic，经通用 webhook 适配器)、WhatsApp 适配器填充结构化字段；**修复**：渠道消息默认不再自动绑定到 ACP 会话（上游为显式绑定；`OPENCLAW_ACP_AUTO_BIND=1` 恢复旧行为）；旧键 transcript 首次自动迁移；显式 `session_key` 或 `session.keyFormat: legacy` 保持原样，`impl:real`（多 agent 路由 bindings 见 1.47）；**embedding provider 接口**：local（哈希）/ openai（任意 OpenAI 兼容 `/embeddings`，`memorySearch.remote.baseUrl/apiKey`，auto 有 key 时选 openai）+ **持久化向量索引/嵌入缓存**（按 sha256(chunk) 存于 `$OPENCLAW_DATA_DIR/memory/main.index.json`，未变 chunk 不重复嵌入，换 provider/model 自动全量重建），远程失败自动回退本地并标记 `fallback`；`memory search|status|index` CLI；`scripts/e2e-embeddings.sh` 用本地假服务端验证，`impl:real`（Gemini/Voyage/本地 GGUF provider、sqlite-vec、旧缓存条目清理 **missing**）；tools（含 `http_get` / `browser_*`）；**CDP browser**（Chrome debugging 就绪时 `impl:real`）；MCP mock/stdio bridge + `mcp.app` list/call/resources；cron/persist/pairing；Control UI 骨架；IM **webhook 入站**（Feishu/Slack/Discord/**WA Cloud**/iMessage）+ **Slack Socket Mode** + **Discord Gateway** + **Feishu 长连接**（mock/feed 或 app 凭证）与 **有 token 时的出站**；AES secrets（有 passphrase 时）；**JS 插件薄桥**（Node stdio / mock → `js.sample_echo`）；**ACP** `session/prompt`→AgentLoop + cancel + **channel bind** + **multimodal image**；`scripts/integration-smoke.sh`（mock，无活 LLM） |
| **仿真 / 轻量（勿当生产自动化）** | `impl:real` computer.screenshot/click/type/move（display+ffmpeg/xdotool）；canvas 本地磁盘 `impl:real`（非 A2UI）；desktop launch 等仍 `real-sim`；browser curl-session 回退 / CDP mock；**无** Playwright |
| **本地占位 store（API 面可测，不是上游 UI）** | **board** 已有磁盘 JSON + `/board` HTML CRUD；portal / wizard / openclaw.setup / mentions 仍偏占位 |
| **明确缺失** | ClawHub 技能安装/同步；Gemini/Voyage/本地 GGUF embedding、sqlite-vec；ACP **acpx**/OAuth/音视频（通道绑定+多模态+session MCP+token 门禁子集已有）；完整 JS/TS 插件宿主（仅有 Node stdio 薄桥 + sample_js）；真实 OS/桌面/无障碍树；完整 Slack interactive；**Baileys**（仅有 Cloud API 子集）；Discord slash/voice；Feishu 专有 SDK 帧；Socket/Gateway/长连接仅为消息子集；上游级 board/canvas/A2UI（仅有本地看板+本地 canvas）；上游云端设备链接/QR/scope token（仅有本地配对码流程 `impl:real`；ocd_ token 可用于网关鉴权，仅两种固定角色（operator/device 拒绝列表），非细粒度 scope；限流仅进程内；/login 为服务端会话 + CSRF token + WS Origin 检查；限流按 TCP 对端 IP 持久化，不信任代理头，无多实例共享；无 OIDC/MFA）；`.github/workflows` 直接推送（见 docs CI） |

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

## 1.43 JS/TS plugin host (upstream API)
- real: host.mjs v2 discovers openclaw.plugin.json / package.json `openclaw.extensions` / legacy plugin.json across repo plugins/, ~/.openclaw/extensions, <workspace>/.openclaw/extensions, plugins.load.paths; honours plugins.enabled/allow/deny and entries.<id>.enabled/config (api.pluginConfig).
- real: api.registerTool (execute/handler, factory), registerCommand (gateway runs before skills, no model turn), api.on hooks: message_received, before_agent_start (prependContext/systemPrompt), message_sending (content rewrite / cancel), agent_end; registerGatewayMethod (DispatchRpc fallback); registerService (start/stop).
- (1.46: TS + channel/provider/http/CLI/tool & session hooks became real — see below.)

## 1.44 Slack interactive
- real: reply directives `[[buttons: L|v,...]]`, `[[select: P|a,b]]`, `[[modal: Title|F1,F2]]` → Block Kit (section + actions); plain text keeps a fallback.
- real: block_actions (button/static_select), view_submission, shortcut/message_action over HTTP (`payload` form field or JSON) and Socket Mode `interactive` envelopes → agent turn with upstream-style session key; reply posted with blocks/thread_ts via chat.postMessage.
- real: modal button → views.open with plain_text_input per field (live when SLACK_BOT_TOKEN, SLACK_API_BASE overridable; recorded otherwise).
- real-sim: no live Slack test (no creds); response_url replies + signing covered in 1.50/1.54 (offline e2e).

## 1.45 Discord slash commands + native chat commands
- real: application command specs built from native (/new /reset /status /help /compact /model /stop) + user-invocable skills + plugin commands (sanitized, dedup, ≤100, optional `args` string option); `discord.commands` / `discord.commands.sync` RPC; PUT /applications/{app}[/guilds/{g}]/commands live with DISCORD_BOT_TOKEN+DISCORD_APPLICATION_ID (auto on adapter start), recorded otherwise.
- real: APPLICATION_COMMAND + message-component interactions via HTTP (type 4 response) and Gateway INTERACTION_CREATE (deferred type 5 ack + PATCH @original follow-up) → agent pipeline with upstream session keys.
- real: native /status, /help, /stop, /model handled in gateway without a model turn.
- real-sim: no live Discord test; Ed25519 in 1.50; /stop aborts in-flight session run and /model session override in 1.54.

## 1.46 Plugin API completion
- real: TypeScript plugin entries on Node >= 22.6 (`--experimental-strip-types` auto-added; OPENCLAW_NODE selects the binary) or with the `typescript` package; tested by scripts/e2e-ts-plugin.sh with Node 22.20 (skips honestly when no Node >= 22.6).
- real: api.registerHttpRoute({path, handler, match: exact|prefix, auth: gateway|plugin}) / registerHttpHandler served by the gateway HTTP server (Node-style req/res shim).
- real: api.registerCli (commander-compatible shim: command/description/option/argument/action) → `openclaw-cpp <cmd> ...` and `plugins run|cli`.
- real: before_tool_call (block / blockReason / params rewrite) and after_tool_call (result, durationMs, error) in the agent loop; session_start / session_end on new, reset and policy reset.
- real-sim: api.registerChannel — openclaw-cpp contract is `inbound.parse(body)` → messages + `outbound.sendText({to,text})`; upstream ChannelPlugin's full adapter surface (gateway start/stop, pairing, directory, actions, threading) is missing.
- real-sim: api.registerProvider with `complete({model, messages, tools})` → router route `<id>`; upstream provider plugins are auth/catalog descriptors (OAuth flows, model catalogs) — not implemented; a route uses the provider's first model.
- missing: npm-based `plugins install`, plugin runtime helpers (api.runtime.*), tool_result_persist / before_compaction hooks.

## 1.47 Multi-agent bindings
- real: agents.list (id, name, default, workspace, model string|{primary}, identity.name, systemPrompt) + bindings[{agentId, match:{channel, accountId, peer:{kind,id}, guildId, teamId}}]; most-specific wins (peer > guild > team > account > channel), first listed on ties; unknown agentIds ignored; missing accountId only matches the default account.
- real: inbound session keys use the routed agent (`agent:<id>:...`); per-agent system prompt (agent workspace bootstrap files + identity/systemPrompt, cached, cleared on reload) and per-agent model route via the provider router; `agents.list` / `agents.route` RPC.
- real-sim (superseded by 1.51): per-agent sessions/workspace/memory/tools; skills + JS host still process-global.

## 1.48 exec tool: real by default + approvals/sandbox semantics
- real: `exec` tool (legacy `shell` alias) runs for real by default (gateway host, security=full, ask=off — upstream gateway defaults); params command/workdir/env/timeout/host/ask; host env overrides of PATH/LD_*/DYLD_*/NODE_OPTIONS rejected. OPENCLAW_EXEC_MODE=mock keeps a mock for tests.
- real: policy = stricter of config (tools.exec.mode deny|allowlist|ask|auto|full or security/ask, per-agent agents.list[].tools.exec) and the approvals document (defaults + agents.<id>: security/ask/askFallback/allowlist); askFallback deny|allowlist|full when no approval UI.
- real: allowlist per agent — resolved-path globs (`**`), bare names only via PATH, argPattern (ECMAScript), every shell segment (&&, ||, ;, |) must match; $(…)/backticks/heredocs never allowlisted; safe bins (stdin-only) ; strictInlineEval (python -c, node -e, …).
- real: approval flow — exec.approval.requested event + prompt posted to the originating chat; resolve via `exec.approval.resolve` RPC, WS, or chat `/approve <id> allow-once|allow-always|deny` (requester or tools.exec.approvers only); allow-always persists exact-argv + cwd-bound entries; timeout → askFallback. Gateway HTTP now serves each client on its own thread so approvals can arrive mid-run.
- real: sandbox host — agents.defaults.sandbox.mode off|non-main|all (per-agent override) picks host; docker backend (`docker run --network none -v ws:/workspace`) or bwrap (read-only root, writable workspace, no network) when docker is absent; `openclaw-cpp approvals get|set`, `exec-policy show|preset yolo|cautious|deny-all`.
- real-sim (superseded in part by 1.53): see 1.53 for SQLite store, binary-hash binding, background/process; bwrap remains the local substitute when docker is absent; no node host; no LLM auto-reviewer for mode=auto (still allowlist+on-miss like ask).

## 1.49 Hot-reload channel/plugin restart coverage
- real: scripts/e2e-hot-reload-channels.sh (part of smoke) proves a reload restarts only adapters whose channels.<id> subtree changed, restarts nothing on unrelated edits, adapters keep serving afterwards, and a plugins.* change restarts the JS host with the **new** plugin config (bug fixed: the host previously restarted with the stale config) and re-registers plugin channels/providers.
- fix: `tools.invoke` RPC accepts plain-text tool results (JS plugin tools) instead of failing JSON parse.
- real-sim (superseded by 1.54): adapters SetConfig + ChannelCred; Start exports config tokens into env for Socket/Gateway children.
- note (superseded by 1.52): the “config reload swaps `deps.config` without a reader lock” gap is closed in **1.52** (exclusive `deps_mu_` + bounded HTTP workers + e2e).

## 1.50 Webhook authenticity + form bodies
- real: Slack signing secret (SLACK_SIGNING_SECRET or channels.slack.signingSecret): HMAC-SHA256 `v0:ts:body`, constant-time compare, 5-minute replay window → 401 otherwise.
- real: Discord interactions Ed25519 (DISCORD_PUBLIC_KEY or channels.discord.publicKey) over `X-Signature-Timestamp + body` → 401 otherwise.
- real: application/x-www-form-urlencoded inbound bodies (Slack slash commands and interactive `payload=`); HTTP Slack slash commands answer with `response_type`/`text`/`blocks`.
- unchanged: other webhook channels still use shared verify tokens only.

## 1.51 Per-agent isolation
- real: each agents.list entry gets its own sessions root (`$OPENCLAW_AGENTS_DIR|<state|data>/agents/<id>/sessions`; `main` keeps `OPENCLAW_SESSION_DIR` when set for back-compat), workspace (`agents.list[].workspace` or `agents/<id>/workspace`), memory index (`agents/<id>/memory/index.json`; main keeps `$OPENCLAW_DATA_DIR/memory/main.index.json`), and agent files under `AgentsRoot()/<id>/`.
- real: tools.allow / tools.deny / tools.profile (`minimal|coding|messaging|full`) from global `tools.*`, then `agents.defaults.tools`, then `agents.list[]|entries.<id>.tools` — deny wins; profile supplies a base allow set (coding includes exec/read/write/edit/http_get/browser_*).
- real: per-agent sandbox object (mode/backend/…) still flows into exec ResolvePolicy; agent Pool rebuilds tool registries on config reload and after JS plugin host restart (re-RegisterTools into the shared base).
- real-sim: skills library and JS plugin host remain process-global (not per-agent); MCP tool list is shared then filtered by allow/deny.

## 1.54 Slack response_url · Discord /stop /model · config credentials
- real: Slack interactive + slash replies POST to Slack `response_url` when present (Block Kit preserved); covered by `scripts/e2e-stop-model-response-url.sh`.
- real: `/stop` sets a per-session cancel flag that aborts an in-flight agent loop (Discord slash `/stop` and chat); `/model [name|clear|list|status]` sets a session model override used via the provider router.
- real: built-in adapters call `SetConfig` on gateway start / hot-reload / channel restart and resolve tokens via env **or** `channels.<id>.*` (botToken/token/appToken/…). Start exports missing env from config so Socket Mode / Discord Gateway children also see credentials.
- real-sim: Socket/Gateway still primarily read process env after export; no per-account multi-token rotation.

## 1.53 Exec: SQLite approvals, binary revalidation, background/process
- real: approvals policy stored in `$OPENCLAW_STATE_DIR|$OPENCLAW_DATA_DIR/state/openclaw.sqlite` key `exec_approvals_config` (upstream locator); legacy `exec-approvals.json` migrated once to `.migrated`. `OPENCLAW_EXEC_APPROVALS_PATH` still forces JSON for tests.
- real: before approval wait, snapshot SHA-256 of resolved executables + existing script/file operands; after allow-once/always, revalidate and deny with `operand changed after approval` if bytes differ. `allow-always` with mutable script operands is downgraded to one-shot.
- real: `yieldMs` / `background` return `{status:running, sessionId}`; companion `process` tool (`list|poll|log|kill`) scoped per agent. Docker sandbox used when `docker` is on PATH, else bwrap (honest when neither available).
- real: `scripts/e2e-exec-sqlite-bg.sh` (smoke) covers migrate + binary-change denial + background list.
- real-sim / remaining: no LLM `tools.exec.reviewer` for mode=auto (auto ≡ ask = allowlist + on-miss); no PTY/send-keys/paste; no node host; WaitBackground overall timeout still SIGKILLs.

## 1.52 Config reload vs concurrent HTTP (exclusive lock + worker pool)
- real: `Server` holds exclusive `deps_mu_` (`std::mutex`). Request handlers and `ReloadConfigNow` serialize on it for config/tools/sessions swaps; adapter Stop/Start and the agent model/tool loop run **after** releasing the lock so reload cannot deadlock against a long run. `config.reload` RPC is handled without holding the lock across adapters.
- real: earlier `shared_mutex` + unbounded `std::thread().detach()` per HTTP client OOMed under reload hammers; HTTP is now a **bounded worker pool** (default 8, `OPENCLAW_GATEWAY_WORKERS`, queue cap 256 → HTTP 503). WebSocket upgrades still detach (long-lived).
- real: per-agent `tools::Registry` is a `shared_ptr` so in-flight agent runs keep a live registry across SetConfig rebuilds (`tools.reset()` + lazy rebuild on next Get).
- real: `scripts/e2e-config-lock.sh` (part of smoke) hammers concurrent `config.reload` + Slack inbound + `tools.list` and asserts the gateway stays healthy.
- real-sim / remaining: not a full formal proof of every deps_ field; long agent loops may still observe a mix of old tools + new config if they re-read `deps_` without the lock for the whole run. Channel credential env re-read semantics unchanged from 1.49.
