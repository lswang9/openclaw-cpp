# OpenClaw 2026.10.5 → openclaw-cpp

**Version: 1.62.0-dev · 未宣称与上游全量对齐。**

## 执行摘要（今晚可用什么）

| 层级 | 内容 |
|------|------|
| **今天可当真用（`impl:real`，仍是子集）** | Gateway HTTP+WS；mock/DeepSeek/OpenAI-compat 路由；agent loop（tools、max_turns、timeout、cancel、session 锁）；sessions/CRUD/export；**workspace 文件工具 read/write/edit（沙箱化：拒绝 `..`/绝对路径/符号链接逃逸）** + **Markdown 记忆（MEMORY.md + memory/*.md，memory_search 为**混合检索**（0.7×本地向量 + 0.3×BM25，`mode` 可选 bm25/vector；向量为离线特征哈希嵌入：词 + 字符三元组 + CJK 单字，**非神经模型**）、memory_get 按行取、memory_set 持久化）**，`impl:real`；**系统提示词组装**（AGENTS/SOUL/TOOLS/IDENTITY/USER/HEARTBEAT/BOOTSTRAP/MEMORY.md 注入，单文件 20k 截断）+ **技能加载优先级**（workspace > ~/.openclaw/skills > bundled > extra）+ **gating**（requires.bins/anyBins/env、os、disable-model-invocation）+ `<available_skills>` 注入；**`skills.entries.<key>`**（enabled、env、apiKey→primaryEnv，满足 env gating，agent 运行期间注入且不覆盖已有变量）+ **技能斜杠命令**（`/<规范化名>` 或 `/skill <名> [参数]`，`user-invocable: false` 排除）+ **`command-dispatch: tool`**（`command-tool`/`command-arg-mode: raw`，斜杠命令直接调用工具，参数 `{command, commandName, skillName}`，不经模型），`impl:real`（env 注入为**每次运行线程局部覆盖层**，不修改进程环境，并发 run 互相隔离；子进程经 RunProcess 获得 environ+覆盖层）；`exec`/`shell` 工具默认真实执行（1.48 起，按上游 exec 审批/沙箱语义，见下文 1.48）；`skills list|check|info`、`context [prompt]` CLI；`chat` 支持 mock provider；**配置文件**：路径 `OPENCLAW_CONFIG_PATH` > `~/.openclaw/openclaw.json`（兼容旧 `~/.openclaw-cpp/config.json`），**JSON5 子集**解析（注释/尾逗号/无引号键/单引号/hex）、`${ENV}` 替换（缺失报错，`$${}` 转义）、`config get/set/unset/file/validate` 点路径 CLI、原子写+.bak，`impl:real`；**`$include`**（字符串/数组、相对路径、嵌套≤10、环检测、深合并后本地覆盖；写回时保留 `$include` 只写差异键）；**schema 校验**（已知根键 + gateway(port/bind/reload/auth)、agents.defaults(model/compaction/memorySearch/timeouts)、agents.list、bindings、session(reset*/dmScope/keyFormat)、channels.<id>(enabled/dmPolicy/groupPolicy/allowFrom/token 类)、tools(allow/deny/profile/exec)、plugins(entries/allow/deny)、models.providers、skills.entries、logging/cron/hooks 的类型与取值；类型错误拒绝加载、网关拒绝启动，未知键为警告）；**热重载**（网关每次 RPC/入站最多每秒检查 mtime，非法配置被拒且保留旧配置，`gateway.reload.mode: off` 关闭，host/port 变化提示需重启，`config.reload` RPC），`impl:real`（schema 覆盖主要子树但仍非上游完整 zod schema，渠道私有字段/深层嵌套多未校验；热重载现会**重扫技能并重建系统提示词**、**重启 channels.<id> 子树变化的运行中适配器**、plugins 子树变化时**重扫插件并重启 JS 宿主**（提示词重建有 smoke 覆盖；渠道/插件重启由 1.49 `scripts/e2e-hot-reload-channels.sh` 覆盖））；`OPENCLAW_MOCK_SCRIPT` 脚本化 mock provider 用于离线 e2e；**会话生命周期**：`sessions.json` 索引（sessionId/createdAt/updatedAt/resets）、会话键无损编码（修复 `a:b`≡`ab` 冲突，兼容旧文件）、`/new` `/reset`（可带后续消息）归档旧 transcript、`session.reset` 每日 atHour(默认 4 点)/idleMinutes 过期，`impl:real`（transcript 已为上游式 **JSONL**（session 头 + message 行 + parentId 链，旧 JSON 自动迁移为 `.migrated`）；网关只持久化**完整未裁剪**历史 + 本轮新消息（不再写入 pruned 视图/系统提示词）；**`session.resetByChannel` > `resetByType`(direct/group/thread) > `reset`** 已实现（chat_type 由 webhook `chat_type` 或会话键推断）；）；**compaction**：超过 `contextTokens - compaction.reserveTokens`（默认 64k/16k，按 chars/4 估算）时用模型把旧轮次总结成一条摘要消息，保留尾部且不从孤立 tool 结果开始，失败回退截断；`/compact [指示]` 手动压缩，`impl:real`；**compaction 前 memory flush**：自动压缩前跑一次静默 agent 轮（可用工具写 memory/*.md，输出不入 transcript，`compaction.memoryFlush.enabled/prompt/systemPrompt`）`impl:real`（`scripts/e2e-memory-flush.sh` 端到端：脚本化 mock provider 经真实网关触发 flush→write memory/flush.md→摘要压缩，flush 轮不入 transcript；已并入 smoke）；**tokenizer**：自研字节级 BPE + 手写 cl100k 预分词，加载 tiktoken 格式词表（`OPENCLAW_TOKENIZER_PATH` 或 `~/.openclaw/tokenizers/cl100k_base.tiktoken`，`tokenizer fetch` 下载），在样例与仓库文件上与 tiktoken 计数一致，`impl:real`（词表未随仓库分发，无词表时回退 chars/4；cl100k ≠ DeepSeek 自有分词器，仍是近似；Unicode 字母类为近似实现）；渠道会话键已改为上游格式（见下））；**上游会话键**：`agent:main:main`（`session.dmScope`=main 默认 / per-peer `agent:main:dm:<peer>` / per-channel-peer）、群 `agent:main:<ch>:group:<id>`、频道 `:channel:<id>`、线程/话题追加 `:thread:<id>`；Slack(channel_type/thread_ts)、Discord(guild_id)、Feishu(chat_type/thread_id)、Telegram 更新格式(chat.type/topic，经通用 webhook 适配器)、WhatsApp 适配器填充结构化字段；**修复**：渠道消息默认不再自动绑定到 ACP 会话（上游为显式绑定；`OPENCLAW_ACP_AUTO_BIND=1` 恢复旧行为）；旧键 transcript 首次自动迁移；显式 `session_key` 或 `session.keyFormat: legacy` 保持原样，`impl:real`（多 agent 路由 bindings 见 1.47）；**embedding provider 接口**：local（哈希）/ openai（任意 OpenAI 兼容 `/embeddings`，`memorySearch.remote.baseUrl/apiKey`，auto 有 key 时选 openai）+ **持久化向量索引/嵌入缓存**（按 sha256(chunk) 存于 `$OPENCLAW_DATA_DIR/memory/main.index.json`，未变 chunk 不重复嵌入，换 provider/model 自动全量重建），远程失败自动回退本地并标记 `fallback`；`memory search|status|index` CLI；`scripts/e2e-embeddings.sh` 用本地假服务端验证，`impl:real`（本地 GGUF/mistral/ollama provider **missing**（Gemini/Voyage 见 1.60，SQLite 向量存储见 1.59））；tools（含 `http_get` / `browser_*`）；**CDP browser**（Chrome debugging 就绪时 `impl:real`）；MCP mock/stdio bridge + `mcp.app` list/call/resources；cron/persist/pairing；Control UI 骨架；IM **webhook 入站**（Feishu/Slack/Discord/**WA Cloud**/iMessage）+ **Slack Socket Mode** + **Discord Gateway** + **Feishu 长连接**（mock/feed 或 app 凭证）与 **有 token 时的出站**；AES secrets（有 passphrase 时）；**JS 插件薄桥**（Node stdio / mock → `js.sample_echo`）；**ACP** `session/prompt`→AgentLoop + cancel + **channel bind** + **multimodal image**；`scripts/integration-smoke.sh`（mock，无活 LLM） |
| **仿真 / 轻量（勿当生产自动化）** | `impl:real` computer.screenshot/click/type/move（display+ffmpeg/xdotool）；canvas 本地磁盘 `impl:real`（非 A2UI）；desktop launch 等仍 `real-sim`；browser curl-session 回退 / CDP mock；**无** Playwright |
| **本地占位 store（API 面可测，不是上游 UI）** | **board** 已有磁盘 JSON + `/board` HTML CRUD；portal / wizard / openclaw.setup / mentions 仍偏占位 |
| **明确缺失** | 本地 GGUF embedding；（ClawHub 见 1.58，向量存储 1.59，Gemini/Voyage 1.60）ACP **acpx**/OAuth/音视频（通道绑定+多模态+session MCP+token 门禁子集已有）；完整 JS/TS 插件宿主（仅有 Node stdio 薄桥 + sample_js）；真实 OS/桌面/无障碍树；完整 Slack interactive；**Baileys**（仅有 Cloud API 子集）；Discord slash/voice；Feishu 专有 SDK 帧；Socket/Gateway/长连接仅为消息子集；上游级 board/canvas/A2UI（仅有本地看板+本地 canvas）；上游云端设备链接/QR/scope token（仅有本地配对码流程 `impl:real`；ocd_ token 可用于网关鉴权，仅两种固定角色（operator/device 拒绝列表），非细粒度 scope；限流仅进程内；/login 为服务端会话 + CSRF token + WS Origin 检查；限流按 TCP 对端 IP 持久化，不信任代理头，无多实例共享；无 OIDC/MFA）；`.github/workflows` 直接推送（见 docs CI） |

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

## 1.62 Plugin runtime: reply dispatcher, media fetch/store, TTS/STT, writeConfigFile
- real: plugin → host **reverse calls** over the existing stdio JSON-RPC (`{"hostCall"}` / `{"hostReply"}`; replies bypass the host's request chain). They are served while any host request is in flight, including the 200 ms event pump; agent turns are deferred to a gateway thread so the plugin host lock is never held across them.
- real: `runtime.channel.reply.dispatchReplyWithBufferedBlockDispatcher({ctx, dispatcherOptions:{deliver, onReplyStart, onIdle, onError, blockLimit}})` runs an agent turn for the inbound ctx and delivers paragraph-aware blocks (`block`… then `final`). It is refused, with a clear error, when called inside a synchronous plugin call (tool/command), where it would deadlock.
- real: `runtime.media.fetchRemoteMedia({url, maxBytes})` uses a new SSRF guard (`net::SafeFetch`: resolve + pin via CURLOPT_RESOLVE, blocks loopback/RFC1918/link-local/CGNAT/ULA/multicast/v4-mapped, manual redirects re-checked, byte cap). Private hosts need `plugins.entries.<id>.allowPrivateNetwork`. `runtime.media.saveMediaBuffer(buf, type, dir, maxBytes, name)` writes to the gateway media store. The `http_get` tool now uses the same guard; its old substring check is gone.
- real: `runtime.tts.textToSpeech` goes through the gateway TTS engine (`OPENCLAW_TTS_URL`). New STT interface `voice::SttEngine` with an OpenAI-compatible multipart backend (`OPENCLAW_STT_URL`, `OPENCLAW_STT_MODEL`, `OPENCLAW_STT_API_KEY`), exposed as `runtime.stt.transcribeAudioFile({filePath|buffer, mime, language})`.
- real: `runtime.config.writeConfigFile(cfg)` is schema-validated (invalid config rejected), written atomically with `.bak`, and picked up by hot reload.
- gaps: blocks are buffered after the full reply (no token streaming or typing keep-alive); TTS/STT have only the generic HTTP backends (no OpenAI/ElevenLabs/Deepgram presets, no telephony format); writeConfigFile writes plain JSON (comments and `$include` structure are not preserved); no `mutateConfigFile`. Tests: `scripts/e2e-plugin-runtime.sh` (smoke) and self-test (SSRF).

## 1.61 WhatsApp Web (Baileys-equivalent): assessment + first primitives
- assessment: `docs/whatsapp-web-gap.md` (layer-by-layer gap, ordered plan, what needs the user).
- real (offline, in-process tested): `channels::waweb` — `Noise_XX_25519_AESGCM_SHA256` symmetric state with the WA prologue (`WA` 6 3), both handshake roles (client = Baileys side, server = test responder), Baileys ordering (ClientFinish framed before `FinishInit`), transport framing (intro header, 3-byte length, split/merged frames), minimal protobuf `HandshakeMessage` codec, RFC 5869 HKDF (test vector), WhatsApp media encryption (HKDF type keys, AES-256-CBC, HMAC-SHA256/10), and the pairing QR payload.
- missing: live socket login, server CertChain verification, ClientPayload/WAProto, WABinary token dictionary, ADV pairing signatures, Signal (X3DH/Double Ratchet/SenderKeys), pre-key upload, app-state/history sync, media hosts, auth persistence. Not interoperability-tested with WhatsApp servers.

## 1.60 Gemini + Voyage embedding providers
- real (fake-server tested): `memorySearch.provider: "gemini"` — `POST {baseUrl}/models/{model}:batchEmbedContents` with `x-goog-api-key`, `taskType` `RETRIEVAL_DOCUMENT` for chunks / `RETRIEVAL_QUERY` for queries, optional `memorySearch.outputDimensionality`, batches of 100; default `gemini-embedding-001` on `generativelanguage.googleapis.com/v1beta`, key `remote.apiKey` → `GEMINI_API_KEY` → `GOOGLE_API_KEY`.
- real (fake-server tested): `provider: "voyage"` — `POST {baseUrl}/embeddings` Bearer, `input_type` `document`/`query`, batches of 128; default `voyage-3.5` on `api.voyageai.com/v1`, key `remote.apiKey` → `VOYAGE_API_KEY`.
- real: `provider: "auto"` picks openai → gemini → voyage by available key, else local. New `EmbedQuery` hook for query-side task types. Failures still fall back to local with `fallback` reported.
- gaps: never run against live Gemini/Voyage (no creds; request/response shapes from public API docs); no Gemini batch-API async jobs, no Voyage multimodal/contextualized embeddings, no per-provider rate-limit backoff; mistral/ollama/local GGUF providers still missing. Test: `scripts/e2e-embeddings-gemini-voyage.sh` (smoke).

## 1.59 Persisted vector store (sqlite-vec equivalent)
- real: memory embeddings now persist in SQLite (`$OPENCLAW_DATA_DIR/memory/main.sqlite`, per-agent `agents/<id>/memory/index.sqlite`; supersedes the 1.47 `index.json` paths): table `embedding_cache(provider, model, hash, dims, embedding BLOB float32, updated_at)`, several provider/models side by side (switching models no longer wipes the cache), WAL, stale rows of the active model pruned when chunks change, legacy `*.index.json` auto-imported then renamed `.migrated`. SQL function `vec_distance_cosine(a, b)` + `VectorIndex::Knn` (ORDER BY distance LIMIT k). `memory search` reports `index.{hits,misses,pruned}`; `memory status` lists models/dims/entries.
- real-sim: "equivalent", not the sqlite-vec extension — KNN is a brute-force scan through a C++ UDF (no `vec0` virtual table / ANN index); search still scores every chunk in-process; chunks/FTS tables are not persisted (BM25 recomputed per query). Tests: self-test (KNN), `scripts/e2e-embeddings.sh` (sqlite rows, prune, legacy import).

## 1.58 ClawHub client (skills + plugins)
- real (fake-server tested): ClawHub `/api/v1` client — `CLAWHUB_REGISTRY` (legacy `CLAWDHUB_REGISTRY`), optional `CLAWHUB_TOKEN` Bearer, 429 `Retry-After`/`RateLimit-Reset` retry. `openclaw skills search|install <@owner/slug> [--version] [--force]|update [<slug>|--all]|uninstall|sync|installed|inspect` (also `openclaw clawhub …`). Skills land in `<workdir>/skills/<slug>` (`CLAWHUB_WORKDIR` or workspace), versions in `.clawhub/lock.json`, per-skill `.clawhub/origin.json`; moderation `isMalwareBlocked` refused; zip symlinks refused; update refuses to clobber locally modified installs.
- real: `plugins install clawhub:<pkg>[@ver]` — package resolve → version `/security` (`trust.blockedFromDownload` refused) → `/artifact` → `/artifact/download`; npm-pack `.tgz` verified against `artifactSha256`/`X-ClawHub-Artifact-Sha256`; legacy ZIP (`package/` root) extracted. Install record `source: clawhub` + artifact metadata; `plugins update` re-resolves latest. `plugins search <q>` (`/api/v1/plugins/search`).
- real-sim / gaps: `sync` uses our fingerprint (sha256 of sorted `path:sha256` lines) against `/api/v1/resolve` — upstream's exact bundle-fingerprint algorithm is not verified, so real-registry matches may differ; no publish/login/whoami/delete (registry-auth workflows belong to the separate `clawhub` CLI upstream); GitHub source-handoff skills not supported; `pluginApi`/`minGatewayVersion` compat checks not enforced; no install telemetry. Not run against live clawhub.ai. Tests: `scripts/e2e-clawhub.sh` + `scripts/fake_clawhub.py` (smoke).

## 1.57 exec mode=auto reviewer + PTY/send-keys
- real (mock-tested): `tools.exec.mode: "auto"` = allowlist + on-miss, but a miss first goes to a model reviewer through the provider interface (`ReviewWithProvider`, gateway router/default provider, no tools). Strict JSON verdict `allow|deny|escalate`; `allow` with `risk=high` (or inline-eval without `risk=low`) → escalate; malformed/failed reply → escalate (or deny with `autoReview.onError: "deny"`); escalate → normal human approval/askFallback. Event `exec.autoreview`. Operand SHA revalidation still applies.
- real: `exec {pty:true}` runs in a pseudo-terminal (posix_openpt, setsid, controlling TTY, 80x24) as a background session; `process` actions `write`, `send-keys` (tmux-style names: Enter, C-x, M-x, arrows, F1–F12…), `submit`, `paste` (bracketed). Tests: self-test (reviewer verdicts, isatty + line input), `scripts/e2e-exec-auto.sh` (smoke).
- real-sim / gaps: reviewer prompt is ours, not upstream's verbatim; `autoReview.model` is parsed but the reviewer uses the session's default route (no per-call model override); no live-LLM reviewer quality evaluation; PTY resize not exposed; sandbox host ignores pty.

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
- missing (install landed in 1.55): plugin runtime helpers (api.runtime.*), tool_result_persist / before_compaction hooks.

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

## 1.56 Plugin contracts: api.runtime, ChannelPlugin gateway/accounts, provider catalog
- real: `api.runtime` subset — `config.loadConfig/current`, `state.resolveStateDir/resolvePluginStateDir`, `logging.getChildLogger`, `system.enqueueSystemEvent`, `channel.sendMessage` + `channel.text.chunkText`, `agent.dispatchInbound`, `media.detectMime`. Host side effects are queued in the Node host and drained by a gateway pump (`drain_events`, ~200 ms) → system events / outbound sends / agent turns.
- real: ChannelPlugin surface — `meta`, `capabilities`, `config.listAccountIds/resolveAccount`, `gateway.startAccount(ctx{accountId, account, cfg, abortSignal, onMessage, setStatus, log})` / `stopAccount`, `outbound.sendText` + `sendMedia`, HTTP `inbound.parse`. Gateway starts every account on load and aborts on plugin refresh/shutdown; inbound → agent → `outbound.sendText`.
- real: provider plugins — `models` + `catalog.run()` (models merged, cached, `refresh`), `auth[]` descriptors listed; each catalog model routable as `<provider>/<model>` (router exact match first). RPC `plugins.providers`, `plugins.channels`, `plugins.events.drain`; `chat.abort` now aborts active session runs.
- real: `scripts/e2e-plugin-contracts.sh` (smoke, installed plugin, offline).
- real-sim / missing: runtime is a subset (no runtime.tts/stt, media fetch/store, reply dispatcher with typing/blocks, pairing/directory/actions/threading adapters, `config.writeConfigFile`); provider `auth` methods are listed but OAuth/API-key flows are not run; pump processes inbound sequentially.

## 1.55 plugins install / uninstall / update
- real: `openclaw-cpp plugins install <dir|.tgz|npm-spec> [--force] [--link] [--no-deps]` → `$OPENCLAW_EXTENSIONS_DIR|~/.openclaw/extensions/<id>` (id from openclaw.plugin.json, else package.json name; requires openclaw.plugin.json or package.json#openclaw). npm specs go through `npm pack` (so registry, `file:`, git specs work when npm can reach them); deps installed with `npm install --omit=dev --ignore-scripts` only when the package has dependencies and no node_modules. Staged copy + atomic rename; symlinks escaping the archive rejected.
- real: install record `plugins.installs.<id>` {source, spec, installPath, version, installedAt} + `plugins.entries.<id>.enabled=true`; `plugins update <id>` re-installs from the recorded spec; `plugins uninstall <id>` removes dir + records; `plugins installs` lists. JS host scans the extensions dir so installed tools load.
- real: `scripts/e2e-plugins-install.sh` (smoke, offline: dir, npm-pack tarball, `file:` spec, update, uninstall, non-plugin rejection).
- missing: registry-backed integrity pinning / signature checks, ClawHub plugin source, `plugins enable/disable` CLI parity flags.

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
