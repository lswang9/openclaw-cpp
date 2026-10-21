# openclaw-cpp 使用状态（中文）

当前版本：**1.42.0-dev**。这是 OpenClaw **2026.10.5** 的 C++ 重写方向，**不是**上游全功能等价物。细节见 [PARITY.md](PARITY.md)。

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
