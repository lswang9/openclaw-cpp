# openclaw-cpp 使用状态（中文）

当前版本：**1.160.0-dev**。这是 OpenClaw **2026.10.5** 的 C++ 重写方向，**不是**上游全功能等价物。细节见 [PARITY.md](PARITY.md)。

## 1.160.0-dev 新增

LINE：postback 事件（富菜单点击、日期时间选择器）送达智能体，与上游格式一致；新增富菜单管理命令 line richmenu（创建/校验/列出/删除/默认/绑定/解绑，含图片上传与别名）。

## 1.159.0-dev 新增

插件安装记录移出 openclaw.json，改存于已安装插件索引（<state>/plugins/installs.json，启用状态库时存 SQLite），旧配置中的 plugins.installs 自动迁移一次（e2e：scripts/e2e-plugin-install-index.sh）。

## 1.158.0-dev 新增

Mattermost：WebSocket 重连退避与上游一致（初始 2 秒、失败翻倍至 60 秒、±20% 抖动、成功会话后重置），并加入 ping/pong 存活检测（e2e：scripts/e2e-mattermost-reconnect.sh）。

## 1.157.0-dev 新增

Telegram：已关闭投票结果（含匿名投票）记入会话并注销路由；business_message 入站与带 business_connection_id 的回复；支付（sendInvoice、pre_checkout_query 应答、successful_payment 记录）。

## 1.156.0-dev 新增

WhatsApp Web：认证状态持久化（creds/预密钥/会话存入状态库）、XEdDSA 签名、预密钥上传 IQ 与数量策略（离线 e2e：scripts/e2e-waweb-auth.sh）。

## 1.155.0-dev 新增

插件域：context-engine bootstrap/compact/maintain、embedding、可信工具策略、realtime 目录

## 1.154.0-dev 新增

Zalo OA 贴纸收发

## 1.153.0-dev 新增

LINE flex 消息（extra.flex）

## 1.152.0-dev 新增

X Account Activity CRC 校验

## 1.151.0-dev 新增

Nostr NIP-04 进程内加解密

## 1.150.0-dev 新增

IRC SASL PLAIN 认证

## 1.149.0-dev 新增

Mattermost 斜杠命令：/command 回调、同步 CommandResponse、可选 /api/v4/commands 注册

## 1.148.0-dev 新增

Signal 附件（getAttachment 入 / data-URI 出）与反应（sendReaction + 入站备注）

## 1.147.0-dev 新增

OpenAI Codex 浏览器 PKCE 登录（--method browser|pkce、本地回调、离线假服务）

## 1.146.0-dev 新增

Telegram 投票（sendPoll + poll_answer 注册表）与论坛话题（按话题会话、创建/编辑/关闭/重开）

## 1.145.0-dev 新增

剩余存储迁入 SQLite：canvas/A2UI、WebAuthn、MFA、auth-profiles；状态库开启时 embeddings 优先 .sqlite

## 1.144.0-dev 新增

网关状态文档迁移至 SQLite（cron/配对/节点/看板/设备链接/子代理/子系统存储），一次性导入 JSON

## 1.143.0-dev 新增

实时渠道传输：Signal JSON-RPC 守护进程、Mattermost WebSocket、原生 IRC 与 Twitch IRC

## 1.142.0-dev 新增

1.142：插件注册接入领域——上下文引擎（plugins.slots.contextEngine）、记忆提示补充、Agent Harness（agentRuntime.id）、语音合成提供方；离线 e2e-plugin-domains

## 1.141.0-dev 新增

1.141：GitHub Copilot（设备码登录、copilot_internal/user、CAPI 头）与 OpenAI Codex（设备码登录、令牌刷新、chatgpt-account-id）；离线 e2e-oauth-providers

## 1.140.0-dev 新增

1.140：Telegram 表情回应（收发）、编辑/删除/置顶消息、setMyCommands 命令菜单、入站编辑标记；离线 e2e-telegram-actions

## 1.139.0-dev 新增

1.139：原 48 个仅捕获的 api.register* 可通过 plugins.invokeRegistration 调用；decision_evaluate / web_fetch / image|video|music_generate 优先插件；reload/lifecycle 通知；离线 e2e-plugin-caps

## 1.138.0-dev 新增

16 个杂项频道文本传输

## 1.137.0-dev 新增

Bedrock Converse 与 Vertex ADC 鉴权；离线 e2e

## 1.136.0-dev 新增

其余独立 e2e 脚本强制 mock 并清除宿主 LLM 密钥

## 1.135.0-dev 新增

Telegram 媒体与内联键盘；工具名清洗；测试离线保护

## 1.134.0-dev 新增

agent：补齐 13 个上游核心工具

## 1.133.0-dev 新增

agent：view_image 作为真实图像输入

## 1.132.1-dev 新增

测试：退出/中断时回收后台网关

## 1.132.0-dev 新增

频道：实时监听与入站鉴权

## 1.131.0-dev 新增

SQLite 会话存储（上游 schema v25）及 JSON 迁移

## 1.130.0-dev 新增

插件 API：接受全部 register* 入口，并接入搜索/文本转换/工具结果中间件

## 1.129.0-dev 新增

新增 Signal/Matrix/Teams/Google Chat/Mattermost 渠道

## 1.128.0-dev 新增

核心工具：web_search、ask_user、view_image、agents_wait、conversations_*

## 1.127.0-dev 新增

Anthropic/Gemini/Responses 原生 SSE 流式输出

## 1.126.0-dev 新增

Telegram 实时 Bot API 长轮询与分块发送；修复冒烟测试偶发失败

## 1.125.0-dev 新增

对齐审计文档 + 原生多提供商路由、/hooks 入口、配置键与若干崩溃修复

## 1.124.0-dev 新增

CLI：上游子命令全部可用（27→0）；connect、mcp serve/configure、acp client 等；SecretRef 解析

## 1.123.0-dev 新增

CLI 第4批：顶层命令防误入聊天、mcp 管理与 OAuth 登录、reset、uninstall、docs 搜索、clawbot/acp/resume 别名

## 1.122.0-dev 新增

本地 CLI 第三批：directory、proxy、database、update；修正 CLI 树生成（缺失 48->27）

## 1.121.0-dev 新增

OAuth 2.1 登录：mcp.authLogin、models.authLogin、users.authConnect oauth；未实现 2->0

## 1.120.0-dev 新增

网关 MCP 服务器、看板、controlUi、会话、目录、迁移、设备权限等（未实现 38->2）；修复 Discord 流式冒烟测试超时

## 1.119.0-dev 新增

Web Push 加密推送、用户模型账号连接、代理 GitHub 身份;未实现 51->38

## 1.118.0-dev 新增

门户代理、梦境日记维护、网关挂起/停止、诊断;未实现 71->51

## 1.117.0-dev 新增

插件安装/目录、会话归档导入恢复、setup 向导与系统代理、mcp.app 订阅推送;未实现 108->71

## 1.116.0-dev 新增

talk 语音会话与 mcp.app 视图实现;未实现 138->108

## 1.115.0-dev 新增

用 cl100k 分词器与模型定价计算用量/费用

## 1.114.0-dev 新增

agents.entries 全链路接入

## 1.113.0-dev 新增

补齐 27 个上游本地 CLI 命令

## 1.112.0-dev 新增

终端 PTY、模型认证、用量、环境等 RPC

## 1.111.0-dev 新增

移植上游完整配置 schema、查找与校验、agents.entries

## 1.110.0-dev 新增

批量实现本地状态类 RPC

## 1.109.0-dev 新增

worktrees/storage/users/设备重命名/message.action 真实实现

## 1.108.0-dev 新增

新增上游 agent 工具、会话键规范化与链式测试

## 1.107.0-dev 新增

诚实标注未实现RPC、新增真实方法与工具、上游再对比

## 1.106.0-dev 新增

TUI：实时流式输出、工具/思考块（agent.tool 事件）、输入队列、模型/代理选择器；sessions.patch 支持会话模型覆盖

## 1.105.0-dev 新增

节点主机协议（role=node 连接、node.invoke/result）；已派发到设备的会话在节点 worker 上执行（worker.launch/status.v1），网关保留转录；新增 node run

## 1.104.0-dev 新增

claws add/status/remove：安装记录、漂移检测、remove 通过网关删除日志（租约+摘要）执行并关闭日志

## 1.103.0-dev 新增

个人 GitHub 登录（设备授权流程，users.github.*）、会话参与者 Co-authored-by 署名、工作流文件守卫（代理工具受限）、github_publish 工具

## 1.102.0-dev 新增

- Discord 语音 DAVE 端到端加密已实现：`scripts/build-dave.sh` 按固定版本构建 libdave（8de72b1）+ mlspp（1cc50a1），CMake 检测到后自动链接。客户端实现 DAVE v1：key package、外部发送者提案→commit/welcome、过渡状态机（21/22/23/24/29/30/31）、成员名单（11/13），Opus 帧在传输加密之前做 DAVE 加密，接收端按说话人解密。离线测试 e2e-discord-dave.sh：假语音服务器通过 `openclaw-dave-helper`（真实 MLS 外部发送者 + 真实 libdave 对端）双向验证。不足：未对 live Discord 测试；仅音频；CI 不构建 libdave（未改 workflows），所以 CI 上该测试会 SKIP。

## 1.101.0-dev 新增

- `openclaw-cpp tui` 在终端下进入全屏界面（ANSI 备用屏幕 + raw termios，不依赖 curses）：标题栏（会话、思考动画、网关）、自动换行的对话区（中日韩/emoji 宽度）、PgUp/PgDn 滚动、输入行编辑与历史、后台发送、Ctrl+C 中止/退出、窗口缩放、斜杠命令；退出时恢复终端。管道和 `--plain` 仍是行模式。测试 e2e-tui-fullscreen.sh 用 pty 驱动。不足：无流式输出、无工具调用块和选择器、同一时间只能有一条消息在处理。

## 1.100.0-dev 新增

- 6 个 RPC 桩全部换成真实实现（src/gateway/subsystems.cpp）：`sessions.companion.ask/state/reset` 只读旁聊（内存保留 ≤24 轮，不写会话）；`sessions.dispatch/move/reclaim` 持久化 worker placement 台账（设备 worker 来自节点注册表，离线→failed→recoverToGateway 恢复；**没有远端 worker 运行时**，只是台账）；`claws.removalJournal` begin/rollback（绑定配置/状态/cron 路径、文件物理身份、配置/安装/journal 摘要、lease 归属），CLI `claws removal-journal`；`projects.searchRemote` GitHub 搜索（精确、affiliated、全局，60 秒缓存）；`sessions.github.publish/confirm`（私有 index 快照 → 提交 → 推送 `openclaw/<session>` 分支 → 建/复用 PR，幂等键、personal 确认）。离线测试：e2e-session-subsystems.sh、e2e-github.sh（假 GitHub + 本地 bare 仓库）。PARITY.md 补齐 1.95–1.99。

## 1.99.0-dev 新增

- WebAuthn 注册 attestation 新增 4 种格式校验：`android-key`（KeyDescription 扩展：challenge、allApplications、KM_ORIGIN_GENERATED、SIGN 用途；`OPENCLAW_WEBAUTHN_ANDROID_TEE_ONLY` 只认 TEE）、`apple`（nonce 扩展 + 公钥一致）、`tpm`（TPM 2.0 pubArea/certInfo 解析、extraData、name 摘要、AIK 证书 v3/空 subject/SAN/EKU/非 CA/aaguid）、`android-safetynet`（JWS 签名、attest.android.com、nonce、ctsProfileMatch、时间戳）。都可链到 `OPENCLAW_WEBAUTHN_TRUST_ANCHORS`。仍无 FIDO MDS、无 ECDAA，未在真实设备上测试。

## 1.98.0-dev 新增

- 补齐上游按参数决定的权限规则：`node.invoke`（browser.proxy/fs.listDir/terminal.upload 需 admin，被禁止的浏览器配置变更交给处理器精确拒绝）、`sessions.patch/patchMany/create/delete/dispatch/move`（按字段判断 write 还是 admin，如 permissionMode=full、sandboxMode、隐身会话、archivedOnly 删除、按 profile 调度）、`plugins.sessionAction`（参数不完整时任意 operator 权限可进入处理器）。
- 新增窄权限 `operator.sessions.read` / `operator.sessions.write`：只对上游列出的会话类方法（chat.history、sessions.list、chat.send 等）可替代 read/write。

## 1.97.0-dev 新增

- 网关类 CLI 命令（devices/nodes/sessions/logs/health/pairing/hooks/secrets/system/users/worktrees/agents/backup/usage/claws/audit/message/storage/automations/exec-approvals 等）改为按子命令路由到正确的 RPC（如 `devices approve <requestId>`→device.pair.approve、`sessions tail <key>`→chat.history、`message react`→message.action），参数按上游 v2026.10.5 选项校验（未知选项报错），`--message-id` 等转为 camelCase 参数，位置参数按上游参数名填入；默认输出人类可读的表格/键值，`--json` 输出原始 payload；`logs --follow` 轮询。上游只在本地实现、没有网关 RPC 的子命令（如 `devices clear`、`backup create`）明确提示未实现并返回 2。

## 1.96.0-dev 新增

- 新增上游本地命令（参数按 v2026.10.5 commander 定义，未知参数报错）：
  - `completion [-s bash|zsh|fish|powershell] [--write-state] [-i --install] [-y]`：补全脚本由上游命令树（自动从上游源码提取，`scripts/gen-cli-tree.py`）+ 本地命令生成。
  - `setup --baseline [--workspace] [--skip-bootstrap] [--json]`：写入工作区/gateway.mode，并用上游模板生成 AGENTS/SOUL/TOOLS/IDENTITY/USER/HEARTBEAT/BOOTSTRAP.md；`setup -m 文本` 走一次代理；配置不完整时自动进入 onboard。`crestodian` 为其废弃别名。
  - `onboard`：交互式（逐行读 stdin，可脚本化）或 `--non-interactive --accept-risk`，支持模型密钥/自定义 OpenAI 兼容端点、gateway 端口/绑定/令牌或密码、远程模式、频道、网页搜索、`--agent-name/--team`、`--reset --reset-scope`（移到 trash 而不是删除）、`--json` 摘要。不安装后台服务（如实提示）。
  - `configure [--section workspace|model|web|gateway|daemon|channels|skills|health]...`：分节交互配置。
  - `migrate list|plan|apply claude`：导入 CLAUDE.md→AGENTS.md/USER.md、技能、自定义命令→技能、MCP 服务器→mcp.servers（env 密钥默认不导入），hooks/权限只列为人工检查；冲突需 `--overwrite`（先备份），apply 前 tar 备份（`--no-backup` 需 `--force`）。hermes 未实现。
  - `tui`：行模式聊天客户端（连网关 RPC，历史、/session /new /abort /status /exit），不是上游的全屏 TUI。

## 1.95.0-dev 新增

- 飞书长连接支持链接预览回调 `url.preview.get`：回包 inline 标题（默认 域名·路径；设 `FEISHU_LINK_PREVIEW_FETCH=1` 时抓取公网页面 <title>，3 秒/256KB 上限，内网地址拒绝）。
- GGUF 本地 embedding：上下文取模型训练长度（上限 `OPENCLAW_GGUF_CTX`，默认 8192），多条文本打包成多序列批次一次推理（`OPENCLAW_GGUF_SEQ`，默认 16）；超长文本按 3/4 步长滑窗、加权平均，不再截断。新命令 `memory embed [--sequential] 文本...` 输出余弦矩阵和推理统计。

## 1.42.0-dev 新增

- 技能可声明 `command-dispatch: tool` + `command-tool: <工具名>`：用户发 `/技能名 参数` 时直接调用该工具（参数为 `{command, commandName, skillName}`），不经过模型，结果直接回复。

## 1.41.0-dev 新增

- 记忆向量检索支持接入真实 embedding 服务：配置 `agents.defaults.memorySearch.provider: "openai"`、`model`、`remote.baseUrl` / `remote.apiKey`，任意 OpenAI 兼容的 `/embeddings` 接口都可以用；`provider: "auto"` 有 key 时用远程，否则用本地。
- 向量索引和缓存会持久化到 `$OPENCLAW_DATA_DIR/memory/main.index.json`，没变的内容不会重复调用接口；远程失败自动退回本地向量。
- 新命令：`memory search <问题>`、`memory status`、`memory index`。
- 测试用本地假服务器（`scripts/e2e-embeddings.sh`），不需要任何真实密钥。

## 1.40.0-dev 新增

- 配置校验覆盖面扩大：`session.dmScope`、`gateway.auth`、`logging.level`、`tools.allow/deny/profile/exec`、`agents.defaults.memorySearch`、`agents.list`、`bindings`、`channels.<渠道>`（dmPolicy/groupPolicy/allowFrom/各类 token）、`plugins.entries`、`models.providers`、`cron`/`hooks` 等都会检查类型和取值，错误会指出具体路径。

## 1.39.0-dev 新增

- 热重载增强：配置变更后会重新扫描技能（含 `skills.entries`）并重建系统提示词；`channels.<id>` 配置变化时重启对应的运行中渠道；`plugins` 配置变化时重扫插件并重启 JS 插件宿主。`config.reload` 返回 `systemPromptChanged` / `channelsRestarted` / `pluginsRestarted`。
- 新增端到端测试 `scripts/e2e-memory-flush.sh`（已并入 smoke）：用 `OPENCLAW_MOCK_SCRIPT` 脚本化的 mock 模型，通过真实网关验证“压缩前记忆落盘 → 摘要压缩”整条链路。

## 1.38.0-dev 新增

- 渠道会话键改为上游格式：私聊默认共用 `agent:main:main`（`session.dmScope` 可设为 `per-peer` 或 `per-channel-peer` 按人分开），群聊 `agent:main:<渠道>:group:<群ID>`，Slack/Discord 频道 `agent:main:<渠道>:channel:<ID>`，线程/话题再加 `:thread:<ID>`。
- Slack、Discord、飞书、Telegram、WhatsApp 会解析私聊/群/线程信息；旧格式会话（如 `slack-C123`）第一次收到消息时自动迁移到新键。
- 修复：之前 Slack/飞书/Discord/WhatsApp 的消息默认被自动绑定到 ACP 会话，没有进入正常会话；现在改为与上游一致的“需显式绑定”（`OPENCLAW_ACP_AUTO_BIND=1` 可恢复旧行为）。
- 调用方显式传 `session_key`，或配置 `session.keyFormat: "legacy"` 时保持旧行为。

## 1.37.0-dev 新增

- 技能环境变量隔离：`skills.entries` 的 env/apiKey 只对当前这次运行可见（线程局部覆盖层），不再改动整个进程的环境变量，同时进行的对话互不可见。
- `shell` 工具可真实执行：设置 `OPENCLAW_EXEC_MODE=real` 后在工作区目录用 `/bin/sh` 运行（默认 60 秒超时，输出最多 64KB），并继承本次运行的技能环境变量；默认仍是模拟执行，且仍需审批。

## 1.36.0-dev 新增

- 真实 tokenizer：内置 BPE 实现，加载 cl100k_base 词表后与 tiktoken 计数一致（已在多种中英文/代码样例上核对）。用 `openclaw-cpp tokenizer fetch` 下载词表到 `~/.openclaw/tokenizers/`，`tokenizer status` 查看状态，`tokenizer count 文本` 计数。
- 自动压缩阈值改用这个 tokenizer 计数；没有词表时仍按“字符数/4”估算。
- 注意：DeepSeek 用的是自己的分词器，cl100k 计数只是更准确的近似。

## 1.35.0-dev 新增

- 技能级配置 `skills.entries.<技能名>`：`enabled: false` 禁用；`env: {...}` 与 `apiKey`（对应技能声明的 `primaryEnv`）会在 agent 运行期间注入环境变量（不覆盖已存在的变量），也能满足技能的 env 依赖检查。
- 技能斜杠命令：`/技能名 参数` 或 `/skill 技能名 参数`（名称转小写、非字母数字变 `_`）；`skills list` 会显示每个技能的命令。`user-invocable: false` 的技能不暴露命令。
- 注意：环境变量注入是进程级的，同时进行的多个对话之间不隔离。

## 1.34.0-dev 新增

- 配置支持 `$include`：`"$include": "./parts/agents.json5"` 或数组，路径相对当前文件，支持嵌套与循环检测；本文件的键覆盖被包含的内容，保存时保留 `$include`。
- 配置校验：常用字段类型/取值错误（如 `gateway.port` 不是 1–65535 的整数）会拒绝加载，网关拒绝启动并提示；未知顶层键只警告。`config validate` 输出 warnings。
- 热重载：网关运行中修改配置文件会自动生效（每秒最多检查一次）；改坏的配置会被拒绝并保留旧配置；改 host/port 需重启。`gateway.reload.mode: "off"` 可关闭；RPC `config.reload` 立即检查。
- 仍缺：完整上游 schema；热重载不会重建已启动的渠道/插件和系统提示词。

## 1.33.0-dev 新增

- `memory_search` 改为混合检索：本地向量（离线特征哈希，不需要任何密钥）占 0.7，BM25 关键词占 0.3。能匹配词形变化，比如搜 “preferences” 能找到 “prefers”。可用 `mode: bm25|vector` 单独使用其中一种。
- 说明：这不是神经网络 embedding，语义同义词（如 “汽车/轿车”）仍可能匹配不到；接入真实 embedding 模型还没做。

## 1.32.0-dev 新增

- 分渠道/分类型重置：`session.resetByChannel.<渠道>` 优先于 `session.resetByType.<direct|group|thread>`，再退回 `session.reset`。
- 自动压缩前先做一次“记忆落盘”静默轮：模型可用工具把重要信息写入 `memory/日期.md`，这轮输出不写入会话记录；可用 `agents.defaults.compaction.memoryFlush.enabled: false` 关闭。

## 1.31.0-dev 新增

- 会话记录改为上游 JSONL 格式（`<键>.jsonl`：首行 session 头，之后每行一条消息，带 parentId 链）；旧 `.json` 首次写入时自动迁移并改名为 `.json.migrated`。
- 修复：网关不再把裁剪后的上下文（`[pruned tool result]`）和系统提示词写进会话文件，保存的是完整记录。

## 1.30.0-dev 新增

- 自动压缩：会话估算 token 超过 `agents.defaults.contextTokens - agents.defaults.compaction.reserveTokens`（默认 64000 / 16000）时，用当前模型把较早的对话总结为一条摘要，保留最近消息；模型失败时退化为截断。`compaction.mode: "off"` 可关闭。
- 手动命令 `/compact [额外要求]`。
- 仍缺：压缩前自动写入记忆（memory flush）、精确 tokenizer。

## 1.29.0-dev 新增

- 会话索引 `sessions.json`：每个会话键记录 sessionId、创建/更新时间、重置次数。
- 发送 `/new` 或 `/reset` 开启新会话（旧记录移到 `archive/`）；`/new 后面的话` 会在新会话里继续处理。
- 自动重置：默认每天本地时间 4 点后开新会话（`session.reset.mode/atHour`），也支持 `idleMinutes` 空闲过期，`mode: "off"` 关闭。
- 修复会话键 `agent:main:main` 与 `agentmainmain` 写到同一文件的问题（旧文件仍可读取）。
- 仍缺：JSONL transcript 格式、按渠道的重置策略、自动压缩摘要。

## 1.28.0-dev 新增

- 配置文件改为上游路径 `~/.openclaw/openclaw.json`（可用 `OPENCLAW_CONFIG_PATH` 覆盖；若只有旧的 `~/.openclaw-cpp/config.json` 则继续用旧文件）。
- 支持 JSON5 写法（注释、尾逗号、无引号键、单引号字符串）和 `${环境变量}` 替换。
- 新 CLI：`config file|show|validate|get <路径>|set <路径> <值>|unset <路径>`，写入为原子替换并保留 `.bak`。旧版把整个配置嵌套在 `raw` 里的 bug 已修复，读取时自动展开。
- 仍缺：上游完整 schema 严格校验、`$include`、配置热重载。

## 1.27.0-dev 新增

- 系统提示词按上游方式组装：注入工作区 `AGENTS.md`/`SOUL.md`/`TOOLS.md`/`IDENTITY.md`/`USER.md`/`HEARTBEAT.md`/`BOOTSTRAP.md`/`MEMORY.md`（单文件超 20000 字符截断），并附带 `<available_skills>` 列表；网关和 `chat` 都会使用。
- 技能加载优先级：工作区 `skills/` > `~/.openclaw/skills` > 内置示例 > `OPENCLAW_SKILLS_DIR`；支持 `metadata.openclaw.requires`（bins/anyBins/env）、`os` 与 `disable-model-invocation` 过滤。
- 新 CLI：`skills list|check|info <名称>`、`context`（查看注入情况）、`context prompt`（打印完整系统提示词）；`OPENCLAW_DEFAULT_PROVIDER=mock openclaw-cpp chat ...` 可离线测试。
- 仍缺：ClawHub 安装、技能级配置/环境变量注入、技能斜杠命令。

## 1.26.0-dev 新增

- `openclaw-cpp --version` / `-V` / `version` 打印版本；未知 `--xxx` 选项报错退出（rc=2），不再被当成 prompt 发给 LLM。
- 工作区文件工具 `read` / `write` / `edit`（沙箱，禁止路径逃逸）。
- Markdown 记忆：`MEMORY.md` 与 `memory/*.md`，`memory_search`（本地 BM25，支持中文）、`memory_get`、`memory_set`。向量检索仍缺失。

## 构建

依赖：C++20、CMake、Conan 2、libcurl、OpenSSL。

```bash
cd openclaw-cpp
python3 -m venv .venv && . .venv/bin/activate
pip install -U conan
conan profile detect --force   # 如需要
conan install . --output-folder=build --build=missing \
  -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release
cmake --build --preset conan-release -j
```

二进制通常在：`build/build/Release/openclaw-cpp`。

```bash
./build/build/Release/openclaw-cpp self-test
./scripts/integration-smoke.sh    # mock 网关冒烟，不调用真实 LLM
```

## 用 mock 跑 Gateway（不烧 API）

```bash
export OPENCLAW_DEFAULT_PROVIDER=mock
./build/build/Release/openclaw-cpp gateway --port 18789
```

- Health：`http://127.0.0.1:18789/health`
- Status：`http://127.0.0.1:18789/v1/status`
- Control UI：浏览器打开 **`http://127.0.0.1:18789/`**
- JSON-RPC：`POST http://127.0.0.1:18789/v1/rpc`

后台启停（pidfile）：

```bash
./build/build/Release/openclaw-cpp gateway start
./build/build/Release/openclaw-cpp gateway status
./build/build/Release/openclaw-cpp gateway stop
```

## 接真实模型（可选）

```bash
export DEEPSEEK_API_KEY=...          # 或 OPENAI_API_KEY + OPENAI_BASE_URL
export OPENCLAW_DEFAULT_PROVIDER=deepseek   # 或 openai
./build/build/Release/openclaw-cpp gateway --port 18789
```

## Control UI 能做什么

- Health / Status、sessions / tools 列表
- Chat（可勾选 SSE）；**Invoke** 工具；**Export** 会话
- **Board**（`/board`）：本地 JSON 看板 CRUD（非上游完整 board UX）。见 `docs/BOARD.md`
- **Pairing**（`/pairing`）：设备配对码 → 批准/拒绝 → 一次性 token（磁盘仅存 SHA-256；token 可作网关 HTTP/WS 鉴权，撤销即 401；claim 限流+错误锁定；批准时选择角色 operator/device（device 不能管理配对/secrets/config）；`/login` 创建服务端会话（cookie 仅存会话 ID，吊销 token 即失效），cookie POST 需 CSRF token，WS 角色检查已有冒烟测试，claim 限流按 IP 持久化）。见 `docs/PAIRING.md`
- **Canvas**（`/canvas`）：本地 HTML/JSON 画布 + 事件日志（非上游 A2UI）。见 `docs/CANVAS.md`
- **不是**上游完整 Control UI（无完整 GitHub/PR 面板等）

## 已知限制（务必读）

1. **computer/desktop**：有 display 时 **截图**（ffmpeg/scrot…）与 **click/type/move**（xdotool）可为 `impl:real`；否则 `real-sim`。**canvas** 本地磁盘 HTML/JSON（`/canvas`，非完整 A2UI）。`OPENCLAW_COMPUTER_FORCE_SIM=1` / `OPENCLAW_INPUT_FORCE_SIM=1` 强制仿真。见 `docs/COMPUTER-CAPTURE.md`。
2. **browser**：优先 **CDP**（已运行的 Chrome `--remote-debugging-port`，见 [docs/BROWSER-CDP.md](docs/BROWSER-CDP.md)）；不可用时回退 **curl-session**（cookie/重定向/链接表单）。**不是** Playwright。可用 `OPENCLAW_BROWSER_CDP_MOCK=1` 自测。
| [docs/COMPUTER-CAPTURE.md](docs/COMPUTER-CAPTURE.md) | Linux 截图后端 |
3. **IM**：webhook 入站较完整；**WhatsApp Cloud API**（verify/feed/send，**非 Baileys**）；Slack Socket / Discord Gateway / Feishu 长连接（`FORCE_MOCK` / `.feed`）。见 `docs/WA-CLOUD.md` 等。
4. **MCP App**：可 list/call tools、resources；宿主 UI 类 RPC 多为本地 ack。
5. **ACP**：`session/prompt`→AgentLoop（默认 mock）；`session/cancel`；`acp-stdio`；**通道绑定** `acp.bind` / auto-bind（Slack/Feishu/Discord/WhatsApp→prompt+outbound）。支持 **多模态 prompt**（text+image_base64/url）；**session MCP**；**auth 门禁**（`OPENCLAW_ACP_TOKEN` / passphrase / `auth/login`）；**无** acpx/OAuth/音视频。见 `docs/ACP-GAP.md`。
6. **JS 插件**：Node stdio 桥（`plugins/js_bridge`）可加载 `plugins/sample_js` 并注册 `js.*` 工具；`OPENCLAW_JS_FORCE_MOCK=1` 走内存 mock。**不是**上游完整 JS 插件 API。见 `docs/JS-PLUGIN-HOST.md`。
7. **对齐数字**：ImplementedMethods 很多 ≠ 与 OpenClaw 2026.10.5 行为一一等价。

## 文档索引

| 文档 | 内容 |
|------|------|
| [STATUS.md](STATUS.md) | 本文：构建与运行 |
| [PARITY.md](PARITY.md) | 对齐诚实说明 |
| [docs/RELEASE.md](docs/RELEASE.md) | 发版与打 tag |
| [SERIES.md](SERIES.md) | 版本里程碑 |
| [docs/ACP-GAP.md](docs/ACP-GAP.md) | ACP 缺口 |
| [docs/PLUGIN-SDK.md](docs/PLUGIN-SDK.md) | 原生插件 ABI |
| [docs/BROWSER-CDP.md](docs/BROWSER-CDP.md) | CDP 浏览器路径 |
| [docs/COMPUTER-CAPTURE.md](docs/COMPUTER-CAPTURE.md) | Linux 截图后端 |
| [docs/ci-github-actions.yml](docs/ci-github-actions.yml) | CI 工作流草案 |

### 1.94.0-dev.0-dev
- 动态权限：按上游规则依参数判定 agent(/reset)、talk.config、environments.list、channels.pairing.approve、fs.listDir、sessions.delete 所需 scope。

### 1.93.0-dev.0-dev
- Discord 用户右键菜单（USER 命令）已实现。
- DAVE 评估：自 2026-03-01 起 Discord 语音（Stage 除外）强制端到端加密，本实现未支持 DAVE，真实语音频道无法加入；4017 关闭码现会明确报告。详见 docs/DAVE.md。

### 1.92.0-dev.0-dev
- WebAuthn 注册证明校验：none / packed（自证明与 x5c）/ fido-u2f，可配置信任根与策略；tpm/安卓/苹果格式未实现。

### 1.91.0-dev.0-dev
- 语音回复扩展到 Discord / Slack / 飞书 / WhatsApp（ffmpeg 转 Ogg/Opus，假平台端到端测试）；修复 TTS 二进制前缀导致 JSON 崩溃。

### 1.90.0-dev.0-dev
- ACP 客户端：终端能力（默认关闭）、多轮对话、session/load 续接、需登录代理的 authenticate 流程（假代理端到端测试）。

### 1.89.0-dev.0-dev
- 本地 GGUF 向量：接入 llama.cpp 真实推理（测试时下载约 21MB 的 MiniLM 模型，离线则如实跳过）。

### 1.88.0-dev.0-dev
- 飞书长连接：按 ClientConfig 自动重连退避、gzip 负载解压、卡片回调返回卡片 JSON（离线假服务器端到端测试通过；未连真实飞书）。

### 1.87.0-dev.0-dev
- 对照上游 10.5 CLI 顶层命令：约 40 个依赖网关的命令改为经 /v1/rpc 转发（不再误当聊天提示）；交互式本地命令（onboard/tui/configure 等）仍缺。

### 1.86.0-dev.0-dev
- 设备权限范围改为按上游 v2026.10.5 逐方法表（519 条）判定；dynamic 方法暂按 operator.write 基线。
- 与上游 RPC 方法表对比补齐 15 个缺失方法（部分为诚实桩）。

### 1.85.0-dev
- Discord：参数自动补全（type 4→8，/model、/help 有候选）与消息右键菜单“Ask OpenClaw”。

### 1.84.0-dev
- WebAuthn 通行密钥：已登录主体可注册（继承其角色），随后可无密码登录；校验 challenge/origin/rpId/UP/签名/signCount；移除通行密钥即结束会话。登录页增加通行密钥按钮。

### 1.83.0-dev
- 设备令牌细粒度 scope：approve 时可指定 operator.read/write/approvals/pairing/admin 或 rpc:<前缀>，RPC 与 HTTP 均强制，403 返回所需 scope。

### 1.82.0-dev
- 飞书长连接：实现 pbbp2 二进制帧（ping/pong、ClientConfig、分片重组、ACK），端点发现按 SDK 方式；长连接收到的消息会通过 im/v1/messages 回复。

### 1.81.0-dev
- ACP acpx：openclaw-cpp 作为 ACP 客户端驱动外部智能体（codex/claude/gemini 预设或配置命令），处理权限请求、限定在 cwd 内的 fs 读写；音频块直通或本地转写；内置 ACP agent 支持音频与规范字段（v1、agent_message_chunk、stopReason）。RPC `acp.agents.run`、工具 `acp_agent`、CLI `acp run`。

### 1.80.0-dev
- Discord 语音：主网关 op 4 自动取得语音端点/令牌；libopus（conan opus/1.5.2）编码 WAV/TTS 音频；播放后可接收他人语音并按说话人解码为 WAV、可选转写；RPC `discord.voice.*` + agent 工具 `discord_voice`。无 DAVE。

### 1.79.0-dev
- Telegram webhook 密钥改为校验 `X-Telegram-Bot-Api-Secret-Token` 头；`messages.tts.auto` 让 Telegram 回复额外以语音（流式 TTS 汇总后 sendVoice/sendAudio）发送；语音转写不再持有网关锁。

### 1.78.0-dev
- PARITY 审计：修正“明确缺失”行中已交付的 Slack interactive / Discord slash / JS-TS 插件宿主；GGUF 如实标注为替身。修正 1.77 版本字符串。

### 1.77.0-dev
- Discord 语音出站播放：语音网关 v8、UDP IP 发现、RTP + rtpsize AEAD（AES-GCM / XChaCha20）、Ogg Opus 解封装；假服务器 e2e 已进 smoke。
- 修复：conan libcurl 8.10.1 不含 WebSocket，Slack Socket/Discord 网关/Feishu WS 实际无法连接；升级到 8.12.1。
- 缺口：未从主网关自动获取语音服务器；无接收、无 DAVE、无 Opus 编码。

### 1.76.0-dev
- 语音消息自动转写：Telegram/WhatsApp/Discord 的语音先经 STT 转写，再交给 agent；新增 Telegram webhook 入站。
- 流式 TTS：CLI `voice tts --stream`，RPC `tts.stream` 通过 WS 事件分块推送。

### 1.75.0-dev
- WA Web：支持 zlib 压缩帧；离线实现 Signal 协议（X3DH + 双棘轮，pkmsg/msg 格式、乱序、防重放），尚未与 libsignal 做字节级对拍。
- 仍缺：XEdDSA 签名、群组 sender key、会话持久化、实时登录。

### 1.74.0-dev
- OIDC：支持 ES256、按组映射角色、用刷新令牌定期重新校验（失败即结束会话）；MFA 改为按用户注册（`--user`）。

### 1.73.0-dev
- 新增 agent 工具 `desktop`；A2UI 输入框/复选框可写入数据模型，按钮点击作为 canvas 通道消息送入 agent 会话。

### 1.72.0-dev
- 桌面控制：真实启动应用（默认拒绝，需白名单）、窗口列表/聚焦/关闭、释放进程；AT-SPI 无障碍树与动作（点击、设文本）；e2e 在 Xvfb 下运行，已纳入 smoke。
- 仍缺：Wayland/macOS/Windows、agent 工具封装。

### 1.71.0-dev
- Canvas：A2UI v0.8 宿主，包括 JSONL 校验（拒绝 v0.9）、推送/重置/状态 RPC、服务端渲染常用组件与数据绑定、按钮 userAction 事件，以及 agent 工具 `canvas`；e2e 已纳入 smoke。
- 仍缺：上游 Lit 渲染器/节点 WebView、双向输入、部分组件，以及把 userAction 自动送入会话。

### 1.70.0-dev
- 网关 OIDC 登录：授权码 + PKCE、state/nonce 校验、通过 JWKS 校验 RS256 id_token、白名单默认拒绝；会话角色为 operator 或 admin；用假 IdP 的 e2e 已纳入 smoke。
- 仍缺：ES256/EdDSA、刷新令牌/登出回调、按组映射角色、真实 IdP 验证。

### 1.69.0-dev
- 网关 MFA：/login 支持 TOTP（RFC 6238，防重放）和一次性恢复码；CLI `auth mfa enroll/status/code/verify/disable`；e2e 已纳入 smoke。
- 仍缺：WebAuthn、按用户的 MFA；Bearer API 调用不受二次验证约束。

### 1.68.0-dev
- WA Web：WABinary 编解码与 Baileys 字节级兼容（令牌字典 v3，5 个 Baileys 生成的样例往返一致）。
- WA Web：XEdDSA 证书链校验（根→中间→叶，序列号/静态公钥/有效期检查），离线自测。
- 仍缺：zlib 压缩帧、真实服务器验证、Signal 协议、实时扫码登录（按要求保持缺失）。

### 1.67.0-dev
- 逐 token 流式输出：OpenAI 兼容 / DeepSeek 走 SSE（包括分片的 tool_call 参数），有消费方时启用（WS 客户端、插件、或 `agents.defaults.streaming: true`）；插件可以收到 `onPartialReply` / `onTyping` 回调。
- 输入中提示保活：Discord / Telegram 在 agent 运行期间定时发送 typing（`channels.<id>.typingIntervalSeconds`，默认 6 秒）。还不支持边生成边编辑消息。

### 1.66.0-dev
- 每个 agent 有自己的 auth-profiles（自己的 store 里没有该 provider 的 profile 时回退到 main），CLI 支持 `--agent`；Linux 上有 Secret Service 时密钥存进 libsecret 钥匙串，JSON 文件里只留标记。没有 Secret Service 时如实回退到 0600 文件。

### 1.65.0-dev
- TTS/STT 新增 OpenAI、ElevenLabs、Deepgram 三家的专用请求格式（`OPENCLAW_TTS_PROVIDER` / `OPENCLAW_STT_PROVIDER`），新增 `voice tts|stt` 命令；离线假服务端校验了请求格式，没有真连供应商。

### 1.64.0-dev
- 插件 `dispatchReplyWithBufferedBlockDispatcher` 现可收到流式 `reply.block`（`hostEvent`），命令/工具内发起 agent.reply 不再死锁（写锁与请求锁分离 + 重入 hook 跳过 + 嵌套前释放 deps 锁）。
- `writeConfigFile` 保留文件头 `//` 注释与 `$include`（`SavePreserving`）；插件安装写回同样走该路径。
- 内置 DeepSeek/OpenAI-compat 走 auth-profiles + 多 profile 轮换（401/429）；`autoReview.model` 按路由选模型；PTY 支持 resize。
- 本地 Ollama / GGUF embedding（离线 e2e）+ memory SQLite FTS5；WhatsApp Web 仅补证书/二进制节点/Signal 离线桩，**不**做真机登录。
- 仍缺：专用 TTS/STT 客户端；Baileys 真机；WA 证书根密钥校验与 dictionary v3；可互操作的 Signal。

### 1.63.0-dev
- Provider 鉴权：`models auth login/paste-token/list/remove`，插件 OAuth/API-key 流程（终端提示、openUrl），auth-profiles.json（0600），调用时注入凭证，过期前自动刷新 OAuth。

### 1.62.0-dev
- 插件运行时补齐：回复分块派发、媒体抓取（SSRF 防护）/保存、TTS/STT 接口、writeConfigFile（校验 + 热重载）；`http_get` 改用同一 SSRF 防护。

### 1.61.0-dev
- WhatsApp Web（Baileys 等价）：差距评估 `docs/whatsapp-web-gap.md`，以及第一批原语：Noise_XX 握手、帧封装、媒体加解密、二维码内容，均为离线自测；真实登录与 Signal 协议仍缺失。

### 1.60.0-dev
- 新增 Gemini / Voyage embedding provider（任务类型、维度、鉴权、auto 选择、失败回退本地）；只用本地假服务端测试，未连真实 API。

### 1.59.0-dev
- 向量存储改为 SQLite（float32 BLOB + `vec_distance_cosine` SQL 函数，多模型并存、过期行清理、旧 JSON 自动导入）；等价实现，并非 sqlite-vec 扩展本身（无 ANN 索引）。

### 1.58.0-dev
- ClawHub 客户端：技能搜索/安装/更新/卸载/同步（lock.json），`plugins install clawhub:<pkg>`（sha256 校验、拦截被封禁/被篡改的发布）；用本地假 registry 测试，未连真实 clawhub.ai。

### 1.57.0-dev
- exec `mode=auto`：allowlist 未命中时先交给模型审查（经 provider 接口，mock 测试），允许/拒绝/升级到人工；高风险自动升级。
- exec `pty:true` 伪终端 + process `write/send-keys/submit/paste`。

### 1.43.0-dev
- JS/TS 插件宿主支持上游插件 API：registerTool/registerCommand/钩子(message_received、before_agent_start、message_sending、agent_end)/registerGatewayMethod/registerService；按配置启用/允许/拒绝。
- TS 插件需 Node ≥ 22.6 或安装 typescript（当前盒子 Node 20，测试用 .mjs 样例）。频道/提供商/HTTP 路由注册尚未支持。

### 1.44.0-dev
- Slack 交互：回复中的按钮/下拉/模态指令转为 Block Kit；按钮点击、模态提交、快捷方式（HTTP 与 Socket Mode）进入智能体回合；模态按钮调用 views.open。签名校验与 response_url 尚未实现。

### 1.45.0-dev
- Discord 斜杠命令：由内置命令、技能命令与插件命令生成命令定义并同步到 Discord；HTTP 与 Gateway 交互均进入智能体管线（Gateway 先延迟应答再编辑原消息）。新增 /status、/help、/stop、/model 内置命令。Ed25519 签名校验尚缺。

### 1.46.0-dev
- 插件 API 补全：TypeScript 插件（Node ≥ 22.6，已在盒子安装 Node 22.20 测试）、HTTP 路由、CLI 命令、工具调用前后钩子（可拦截/改参）、会话开始/结束钩子、插件渠道（入站解析 + 出站发送）、插件模型提供商（complete 接口）。
- 渠道与提供商为简化契约，与上游完整 ChannelPlugin / 提供商认证目录不同；插件 npm 安装尚缺。

### 1.47.0-dev
- 多智能体路由：agents.list + bindings（peer > guild > team > account > channel 优先级），会话键带智能体 ID，按智能体使用独立系统提示词与模型。会话存储与工具注册表仍为共享。

### 1.48.0-dev
- exec 工具默认真实执行（网关主机，security=full / ask=off，与上游默认一致），按 tools.exec 配置与审批文件取更严格者；支持允许列表（路径通配、参数正则、逐段匹配）、安全命令、严格内联代码检测、askFallback。
- 审批流程：发出 exec.approval.requested，在原聊天发提示，可用 RPC 或聊天 /approve（仅请求者或配置的审批人）批准；allow-always 写入精确参数+目录绑定的条目。
- 沙箱：sandbox.mode off/non-main/all；有 docker 用 docker，否则用 bwrap（只读根、可写工作区、无网络）。审批文档仍为 JSON（上游已改为 SQLite）。

### 1.49.0-dev
- 热重载渠道/插件重启有了端到端测试：只重启变更的渠道，插件配置变更时用新配置重启 JS 宿主（修复了之前沿用旧配置的问题）并重新注册插件渠道。tools.invoke 兼容纯文本工具结果。

### 1.50.0-dev
- Webhook 鉴权：Slack 签名密钥（HMAC v0 + 5 分钟防重放）与 Discord Ed25519 签名校验，失败返回 401；支持表单编码请求体（Slack 斜杠命令、交互 payload）。

### 1.51.0-dev
- 多智能体隔离：每个 agents.list 条目独立会话目录、工作区、记忆索引与工具集（allow/deny/profile）；main 在设置了 OPENCLAW_SESSION_DIR 时仍用该目录以保持兼容。技能库与 JS 插件宿主仍为进程级共享。

### 1.56.0-dev
- 插件契约扩充：`api.runtime`（配置/状态目录/日志/系统事件/渠道发送/派发到 agent/媒体类型）、ChannelPlugin 的账号列表 + `gateway.startAccount` 长连接 + `outbound.sendText/sendMedia`、provider 的 `catalog` 模型目录（可用 `<provider>/<model>` 路由）；`chat.abort` 真正中止会话回合。

### 1.55.0-dev
- 新增 `plugins install <目录|tgz|npm 包>`、`plugins update/uninstall/installs`：安装到 `~/.openclaw/extensions/<id>`，记录在配置 `plugins.installs`；离线测试覆盖目录、tarball、`file:` 规格。

### 1.54.0-dev
- Slack `response_url` 回帖；Discord/聊天 `/stop` 取消当前会话回合、`/model` 会话级切换；渠道适配器从配置读取凭证（热重载/重启时 SetConfig，Start 时补齐 env 供 Socket/Gateway）。

### 1.53.0-dev
- exec 审批改为 SQLite（`state/openclaw.sqlite#exec_approvals_config`），自动从旧 JSON 迁移；批准前后对可执行文件/脚本做 SHA-256 校验，变更则拒绝；支持 `yieldMs`/`background` + `process`（list/poll/log/kill）；有 docker 用 docker，否则 bwrap。`mode=auto` 仍无 LLM reviewer（等同 ask）。

### 1.52.0-dev
- 热重载与并发 HTTP：`deps_mu_`（独占 `mutex`）串行化配置/工具换出与请求读路径；agent 循环在释锁后跑，避免与 reload 死锁。HTTP 改为有界工作线程池（默认 8，`OPENCLAW_GATEWAY_WORKERS`，队列满返回 503），取代原先无界 `detach` 在并发 reload 下 OOM。agent 工具注册为 `shared_ptr` 以便在途 run 在 rebuild 后仍持有旧 registry。`scripts/e2e-config-lock.sh`（已并入 smoke）并发 config.reload + inbound/tools.list。
