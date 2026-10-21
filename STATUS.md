# openclaw-cpp 使用状态（中文）

当前版本：**1.29.0-dev**。这是 OpenClaw **2026.10.5** 的 C++ 重写方向，**不是**上游全功能等价物。细节见 [PARITY.md](PARITY.md)。

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
