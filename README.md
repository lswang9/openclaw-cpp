# openclaw-cpp

OpenClaw **C++ 全量重写** · 对齐 **OpenClaw 2026.10.5**。

**1.2.0-dev — 未宣称全量对齐。** [PARITY.md](PARITY.md) · [docs/ci-github-actions.yml](docs/ci-github-actions.yml)

## 1.2 摘要

- **285** implemented methods
- **Slack / Discord inbound** webhooks（challenge / Interactions ping + 消息 → agent）
- **sessions.export / import**；**backup.restore**（含会话正文）
- **max_turns** + **rate_limit_per_minute** 经 config → agent loop
- CI YAML 仅在 `docs/`（不推 `.github/workflows`）

```bash
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
```

Env extras: `SLACK_VERIFY_TOKEN`, `DISCORD_VERIFY_TOKEN`, `FEISHU_VERIFY_TOKEN`, agent limits in config `raw.agent`.
