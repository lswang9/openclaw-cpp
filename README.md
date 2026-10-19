# openclaw-cpp

OpenClaw 的 **C++ 全量重写**（非 Node 包装）。对齐目标：**OpenClaw 2026.10.5**。

**当前：0.3.0-dev — 未宣称全量对齐。** 进度见 [PARITY.md](PARITY.md)，系列见 [SERIES.md](SERIES.md)。

## 亮点（0.3.0-dev）

- Gateway 广告上游 **516** methods / **62** events；**36** 个已实现或半实现
- 可选网关鉴权：`OPENCLAW_GATEWAY_TOKEN`（Bearer / `X-OpenClaw-Token`）
- Skills：加载 `SKILL.md`（含 frontmatter）；示例 `skills/examples/hello-skill`
- Streaming：`POST /v1/chat/completions` + `"stream": true` → SSE
- Cron：5 字段表达式 + `openclaw-cpp cron-daemon`
- Agent loop、session、tools、channels/config/plugins 骨架

## 构建（Conan）

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release
cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
```

## 常用命令

```bash
./build/build/Release/openclaw-cpp status
./build/build/Release/openclaw-cpp skills
./build/build/Release/openclaw-cpp methods
./build/build/Release/openclaw-cpp gateway --port 18789
# 鉴权示例：
# export OPENCLAW_GATEWAY_TOKEN=secret
# curl -H "Authorization: Bearer secret" http://127.0.0.1:18789/v1/status
```

| 变量 | 说明 |
|------|------|
| `DEEPSEEK_API_KEY` | chat / stream 需要 |
| `OPENCLAW_GATEWAY_TOKEN` | 非空则启用 Gateway HTTP/WS 鉴权 |
| `OPENCLAW_SKILLS_DIR` | 额外 skills 扫描根目录 |
| `OPENCLAW_SESSION_DIR` | 会话/记忆目录 |
| `OPENCLAW_CRON_DEMO_SECONDS` | cron-daemon 短跑演示秒数 |

Sibling：`openclaw-cpp-gateway`、`openclaw-cpp-tools`（占位，实现仍在本仓）。
