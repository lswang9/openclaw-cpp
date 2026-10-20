# openclaw-cpp 系列

对齐官方 OpenClaw **2026.10.5** 的 C++ 全量重写系列。

| 仓库 | 角色 | 状态 |
|------|------|------|
| **[openclaw-cpp](https://github.com/lswang9/openclaw-cpp)** | 核心：CLI、agent loop、provider、session、内嵌 Gateway 初版 | 活跃（0.8.0-dev） |
| **[openclaw-cpp-gateway](https://github.com/lswang9/openclaw-cpp-gateway)** | Gateway 深挖（HTTP/WS 协议、通道多路、鉴权）— 可从核心迁出 | README 占位 |
| **[openclaw-cpp-tools](https://github.com/lswang9/openclaw-cpp-tools)** | 工具包 / skills / 执行器扩展 | README 占位 |

原则：

1. 先在核心仓做出可运行垂直切片，再拆 sibling，避免空转。
2. 共用 C++20 + Conan；稳定接口后再抽 `openclaw-cpp-common`。
3. 行为对齐 2026.10.5，不是 Node 包装层。详见 [PARITY.md](PARITY.md)。

- **0.9.0-dev**: real local-state ExtraDispatch, Control UI sessions/SSE/tools, ACP skeleton+gap, live TG/Discord/Feishu curl when tokens

- **1.0.0-dev**: real ExtraDispatch bulk, daemon pidfile start/stop, plugin C SDK+.so, media upload/serve

- **1.1.0-dev**: TTS HTTP/record, http_get+browser stub, Feishu inbound, catalog real expand, GHA CI

- **1.2.0-dev**: Slack/Discord inbound, session export/import, backup restore, max_turns+rate_limit

- **1.3.0-dev**: catalog→549 real-prefix coverage, WA/iMessage inbound, log/otel export, Control UI invoke+export

- **1.4.0-dev**: SimulationBackend real-sim, browser.open_url, mcp.app client bridge, honest PARITY

- **1.5.0-dev**: loop timeout/cancel, session locks, HTTP /v1/rpc, integration-smoke, RELEASE.md

- **1.5.1-dev**: docs STATUS.md (zh), PARITY executive summary, EnsureObj store_key lifetime fix

- **1.12.0-dev**: JS plugin host (Node stdio bridge + sample_js); FORCE_MOCK; docs/JS-PLUGIN-HOST.md

- **1.11.0-dev**: Slack Socket Mode (WSS envelopes + mock feed); SLACK_APP_TOKEN; docs/SLACK-SOCKET.md

- **1.10.0-dev**: computer.click/type/move via xdotool (impl:real); FORCE_SIM envs; COMPUTER-CAPTURE.md

- **1.9.0-dev**: computer.screenshot live capture (ffmpeg/scrot/import/grim) with honest impl tags; force-sim for CI

- **1.8.0-dev**: ACP session/prompt → RunAgentLoop (mock default) + session/cancel; ACP-GAP refresh

- **1.7.0-dev**: ACP JSON-RPC wire subset (initialize/session/*), acp-stdio, in-process client self-test; ACP-GAP.md refresh

- **1.6.0-dev**: browser CDP client (navigate/evaluate/screenshot) + curl-session fallback; BROWSER-CDP.md; fix ImplementedSet begin/end double-temporary UB (computer/browser RPC segfault)
