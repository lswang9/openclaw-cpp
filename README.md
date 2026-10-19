# openclaw-cpp

OpenClaw **C++ 全量重写** · 对齐 **OpenClaw 2026.10.5**。

**0.9.0-dev — 未宣称全量对齐。** [PARITY.md](PARITY.md) · [docs/ACP-GAP.md](docs/ACP-GAP.md) · [SERIES.md](SERIES.md)

## 0.9 摘要

- **206** methods in `ImplementedMethods`（catalog 仍广告 516）
- ExtraDispatch：大量路径标 `impl:"real"`（本地 state）；余下前缀桶标 `impl:"honest"`（不虚报成功）
- **Control UI**：sessions 列表、tools 列表、SSE 流式 chat（`GET /`）
- **ACP**：内存 session skeleton + `acp.*` RPC；完整协议见 gap doc
- **Outbound**：Telegram / Discord / Feishu — `TELEGRAM_BOT_TOKEN` / `DISCORD_BOT_TOKEN` / `FEISHU_WEBHOOK_URL` 存在时 libcurl 实发，否则 `record_only`

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
./build/build/Release/openclaw-cpp gateway --port 18789
# open http://127.0.0.1:18789/
```

Env（可选）：`DEEPSEEK_*` / `OPENAI_*` / `OPENCLAW_*` / IM tokens / `OPENCLAW_SECRETS_PASSPHRASE`。
