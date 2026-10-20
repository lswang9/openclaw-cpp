# openclaw-cpp 使用状态（中文）

当前版本：**1.9.0-dev**。这是 OpenClaw **2026.10.5** 的 C++ 重写方向，**不是**上游全功能等价物。细节见 [PARITY.md](PARITY.md)。

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
- Chat（可勾选 SSE）；**Invoke** 工具；**Export** 会话（`GET /v1/sessions/:id/export`）
- **不是**上游完整 Control UI（无完整 GitHub/PR 面板等）

## 已知限制（务必读）

1. **computer/desktop**：有 `DISPLAY`/`WAYLAND` 且存在 scrot/import/ffmpeg/grim 时，**截图可为 `impl:real`**；否则占位 PNG（`real-sim`）。鼠标/键盘/canvas 仍为仿真。可用 `OPENCLAW_SCREENSHOT_FORCE_SIM=1` 强制占位。
2. **browser**：优先 **CDP**（已运行的 Chrome `--remote-debugging-port`，见 [docs/BROWSER-CDP.md](docs/BROWSER-CDP.md)）；不可用时回退 **curl-session**（cookie/重定向/链接表单）。**不是** Playwright。可用 `OPENCLAW_BROWSER_CDP_MOCK=1` 自测。
| [docs/COMPUTER-CAPTURE.md](docs/COMPUTER-CAPTURE.md) | Linux 截图后端 |
3. **IM**：webhook 入站较完整；出站需对应 env token，否则只本地记录。
4. **MCP App**：可 list/call tools、resources；宿主 UI 类 RPC 多为本地 ack。
5. **ACP**：`session/prompt` 已接 **AgentLoop**（默认 mock）；`session/cancel` 可取消；`acp-stdio` 可用。**无** acpx/通道绑定/多模态。见 `docs/ACP-GAP.md`。
6. **对齐数字**：ImplementedMethods 很多 ≠ 与 OpenClaw 2026.10.5 行为一一等价。

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
