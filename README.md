# openclaw-cpp

OpenClaw **C++ 全量重写**（非 Node 包装）。对齐：**OpenClaw 2026.10.5**。

**0.4.0-dev — 未宣称全量对齐。** 见 [PARITY.md](PARITY.md) · [SERIES.md](SERIES.md)。

## 0.4.0-dev 摘要

- Gateway：**516** methods 广告 / **72** 实现
- **Channel adapters**：`webhook` HTTP 入站 → agent loop；telegram/discord/slack/feishu/whatsapp stubs
- **MCP client** 骨架（stdio JSON-RPC + mock self-test）
- **Approvals / secrets** stores + RPC
- Auth、skills、streaming、cron eval（延续 0.3）

## 构建

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
```

## Webhook 入站示例

```bash
./build/build/Release/openclaw-cpp gateway --port 18789
curl -sS -X POST http://127.0.0.1:18789/api/channels/webhook/inbound \
  -H 'Content-Type: application/json' \
  -d '{"text":"hello","sender":"alice","session_key":"demo"}'
```

| 变量 | 说明 |
|------|------|
| `DEEPSEEK_API_KEY` | chat / webhook→LLM |
| `OPENCLAW_GATEWAY_TOKEN` | 启用鉴权 |
| `OPENCLAW_SKILLS_DIR` | 额外 skills 根 |
| `OPENCLAW_SESSION_DIR` | 会话与 memory |
