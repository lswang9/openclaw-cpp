# openclaw-cpp

OpenClaw **C++ 重写**，长期对齐 **OpenClaw 2026.10.5**（**未宣称全量对齐**）。

**1.51.0-dev** · 文档入口：

| 文档 | 说明 |
|------|------|
| **[STATUS.md](STATUS.md)** | 中文：构建、mock gateway、Control UI、已知限制 |
| **[PARITY.md](PARITY.md)** | 可用 / 仿真 / 缺失（诚实对齐表） |
| **[docs/RELEASE.md](docs/RELEASE.md)** | 自测、冒烟、如何打非 `-dev` tag |
| **[SERIES.md](SERIES.md)** | 版本里程碑 |

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
export OPENCLAW_DEFAULT_PROVIDER=mock
./build/build/Release/openclaw-cpp gateway --port 18789
# 浏览器打开 http://127.0.0.1:18789/
./scripts/integration-smoke.sh
```

1.5.1：修正 ExtraDispatch 本地 store 键生命周期；文档（STATUS + PARITY 执行摘要）收紧。无新大子系统。
