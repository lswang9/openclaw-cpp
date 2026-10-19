# openclaw-cpp

OpenClaw 的 **C++ 全量重写**（非 Node 包装）。对齐目标：**OpenClaw 2026.10.5**。

进度见 [PARITY.md](PARITY.md)，系列规划见 [SERIES.md](SERIES.md)。

## 当前能力（0.2.0-dev）

- **Agent loop**：messages → model → tool_calls → tool result → model → stop
- **Tools**：内置 `echo`、`time_now`；OpenAI tools 协议
- **Provider**：DeepSeek chat completions（含 402/401/429 错误处理）
- **Session**：JSON 文件会话存储
- **Gateway**（默认 `127.0.0.1:18789`）：HTTP + WebSocket Upgrade
  - `GET /health` `GET /v1/status` `GET /v1/models`
  - `POST /v1/chat/completions`（可跑 agent loop）
  - `POST /tools/invoke`
  - WS：hello + `status` / `tools.list`
- **CLI**：`chat` / `gateway` / `self-test` / `status` / `tools`
- **离线自测**：`openclaw-cpp self-test`（mock provider，无需 API Key）

## 依赖

- CMake ≥ 3.20、Conan ≥ 2、C++20
- Conan：`nlohmann_json`、`libcurl`

## 构建

```bash
conan install . --output-folder=build --build=missing -s build_type=Release
cmake --preset conan-release
cmake --build --preset conan-release -j
```

二进制：`build/build/Release/openclaw-cpp`

```bash
./build/build/Release/openclaw-cpp self-test
./build/build/Release/openclaw-cpp status
./build/build/Release/openclaw-cpp tools
```

## 运行

```bash
export DEEPSEEK_API_KEY=...   # chat / gateway 的 LLM 路径需要
./build/build/Release/openclaw-cpp chat -s demo "你好"
./build/build/Release/openclaw-cpp gateway --port 18789
curl -s http://127.0.0.1:18789/health
curl -s http://127.0.0.1:18789/v1/status
```

| 变量 | 默认 |
|------|------|
| `DEEPSEEK_API_KEY` | （chat 必填） |
| `DEEPSEEK_BASE_URL` | `https://api.deepseek.com` |
| `DEEPSEEK_MODEL` | `deepseek-chat` |
| `OPENCLAW_SESSION_DIR` | `/tmp/openclaw-cpp-sessions` |
| `OPENCLAW_GATEWAY_HOST` / `PORT` | `127.0.0.1` / `18789` |

## 许可证

待定（对齐上游与系列策略后再定）。
