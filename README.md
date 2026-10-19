# openclaw-cpp

OpenClaw **C++ 全量重写** · 对齐 **OpenClaw 2026.10.5**。

**1.3.0-dev — 未宣称全量对齐。** [PARITY.md](PARITY.md)

## 1.3 摘要

- **549** implemented methods（前缀桶 real 本地行为；board/computer 等仍 stub）
- WhatsApp / iMessage HTTP inbound（与 Feishu/Slack 同模式）
- `logs.export` / `otel.export` → data/exports JSONL（可选 OTEL endpoint 标记）
- Control UI：tools **Invoke**、session **Export**

```bash
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
./build/build/Release/openclaw-cpp gateway --port 18789
```
