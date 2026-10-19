# openclaw-cpp

OpenClaw **C++ 全量重写**。对齐目标：**OpenClaw 2026.10.5**。

**0.5.0-dev — 未宣称全量对齐。** [PARITY.md](PARITY.md) · [SERIES.md](SERIES.md)

## 0.5 摘要

- MCP tools 桥入 agent registry（`mcp.*`）
- WebSocket 事件扇出（`session.message` / `chat` / `session.tool`）
- `shell` 工具 + approvals 门禁（默认 mock，不真 exec）
- Telegram / Feishu **outbound 载荷记录**（无真实 API key 也可测）
- nodes / media artifacts / config 轻量校验
- Gateway 实现方法约 **82** / 广告 **516**

## 构建

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
```
