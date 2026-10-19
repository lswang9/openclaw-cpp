# openclaw-cpp

OpenClaw **C++ 全量重写** · 对齐 **OpenClaw 2026.10.5**。

**0.6.0-dev — 未宣称全量对齐。** [PARITY.md](PARITY.md) · [SERIES.md](SERIES.md)

## 0.6 摘要

- MCP **stdio 子进程**（`mcp-mock-server` + `MakeStdioClient`；可选 `OPENCLAW_MCP_COMMAND`）
- WS **session/event 订阅过滤**
- **Context assemble / compact**（系统提示 + 历史裁剪 + tool 结果修剪）
- `agents.files.*`、`chat.history` 分页、`logs.tail`、`device.pair.*`
- 实现方法约 **89** / 广告 **516**

## 构建与自测

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
# MCP mock over stdio:
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | ./build/build/Release/openclaw-cpp mcp-mock-server
```
