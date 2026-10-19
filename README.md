# openclaw-cpp

OpenClaw **C++ 全量重写** · 对齐 **OpenClaw 2026.10.5**。

**1.1.0-dev — 未宣称全量对齐。** [PARITY.md](PARITY.md) · [docs/PLUGIN-SDK.md](docs/PLUGIN-SDK.md) · [docs/ACP-GAP.md](docs/ACP-GAP.md)

## 1.1 摘要

- **266** implemented methods；更多 controlUi / catalog / agent / TTS / voicewake 本地 **real**
- **TTS**：`OPENCLAW_TTS_URL` / `OPENCLAW_TTS_API_KEY` → HTTP；否则 record-only
- **Tools**：`http_get`（curl）；`browser` 明确 not configured
- **Feishu**：inbound URL verification + message → agent；outbound via `FEISHU_WEBHOOK_URL`
- **CI**：workflow at `docs/ci-github-actions.yml` (enable as `.github/workflows/ci.yml` with `workflow` scope)

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
./build/build/Release/openclaw-cpp gateway --port 18789
```
