# openclaw-cpp

OpenClaw **C++ 全量重写** · 对齐 **OpenClaw 2026.10.5**。

**0.8.0-dev — 未宣称全量对齐。** [PARITY.md](PARITY.md) · [SERIES.md](SERIES.md)

## 0.8 摘要

- **202** methods in implemented list (core + category honest handlers)
- Provider hot-switch: `providers.list` / `providers.setDefault` / `models.setDefault`
- **Control UI** at `http://127.0.0.1:18789/` (health + chat)
- Secrets **AES-256-GCM** when `OPENCLAW_SECRETS_PASSPHRASE` is set

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
./build/build/Release/openclaw-cpp gateway --port 18789
# open http://127.0.0.1:18789/
```
