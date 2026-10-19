# openclaw-cpp 系列

对齐官方 OpenClaw **2026.10.5** 的 C++ 全量重写系列。

| 仓库 | 角色 | 状态 |
|------|------|------|
| **[openclaw-cpp](https://github.com/lswang9/openclaw-cpp)** | 核心：CLI、agent loop、provider、session、内嵌 Gateway 初版 | 活跃（0.5.0-dev） |
| **[openclaw-cpp-gateway](https://github.com/lswang9/openclaw-cpp-gateway)** | Gateway 深挖（HTTP/WS 协议、通道多路、鉴权）— 可从核心迁出 | README 占位 |
| **[openclaw-cpp-tools](https://github.com/lswang9/openclaw-cpp-tools)** | 工具包 / skills / 执行器扩展 | README 占位 |

原则：

1. 先在核心仓做出可运行垂直切片，再拆 sibling，避免空转。
2. 共用 C++20 + Conan；稳定接口后再抽 `openclaw-cpp-common`。
3. 行为对齐 2026.10.5，不是 Node 包装层。详见 [PARITY.md](PARITY.md)。
