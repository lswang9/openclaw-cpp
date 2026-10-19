# openclaw-cpp

OpenClaw **C++ 全量重写** · 对齐 **OpenClaw 2026.10.5**。

**1.0.0-dev — 未宣称全量对齐。** [PARITY.md](PARITY.md) · [docs/ACP-GAP.md](docs/ACP-GAP.md) · [docs/PLUGIN-SDK.md](docs/PLUGIN-SDK.md)

## 1.0 摘要

- **237** implemented methods（catalog 516）；ExtraDispatch 以 `impl:"real"` 本地状态为主
- **Daemon**：`gateway start|stop|status` + pidfile + 优雅 SIGTERM
- **Plugin SDK**：C ABI + `plugins/sample_echo` `.so`（`dlopen`）
- **Media**：`POST /v1/media` 上传，`GET /v1/media/:id` 取回
- 浏览器/computer/TTS 等：**不伪造**（`implemented:false`）

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
./build/build/Release/openclaw-cpp gateway --port 18789
./build/build/Release/openclaw-cpp gateway start   # background + pidfile
./build/build/Release/openclaw-cpp gateway stop
```
