# openclaw-cpp

OpenClaw **C++ 全量重写** · 对齐 **OpenClaw 2026.10.5**。

**0.7.0-dev — 未宣称全量对齐。** [PARITY.md](PARITY.md) · [SERIES.md](SERIES.md)

## 0.7 摘要

- **Multi-provider**：`deepseek` / `openai`（OpenAI-compatible）/ `mock` via `Router`
- **Persist**：`~/.openclaw-cpp/data`（或 `OPENCLAW_DATA_DIR`）保存 cron / pairing / nodes
- **Loop detection**：重复相同 tool 调用 soft-warn 后熔断
- Doctor / models / 更多 sessions·cron·skills·memory RPC → **~101** implemented / 516 advertised

## 构建

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
```

| 变量 | 说明 |
|------|------|
| `OPENCLAW_DEFAULT_PROVIDER` | `deepseek`（默认）/ `openai` / `mock` |
| `OPENAI_API_KEY` / `OPENAI_BASE_URL` | OpenAI-compatible 路由 |
| `OPENCLAW_DATA_DIR` | 持久化目录 |
