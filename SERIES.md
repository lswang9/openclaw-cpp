# openclaw-cpp 系列规划

一套仓库、分阶段落地；对齐官方 OpenClaw **2026.10.5**。

| 仓库 | 角色 | 状态 |
|------|------|------|
| **openclaw-cpp**（本仓） | 核心：CLI / 会话入口 / LLM 往返 | 第一刀：DeepSeek chat completions |
| openclaw-cpp-gateway | Gateway：通道接入、路由、鉴权 | 规划中 |
| openclaw-cpp-tools | Tools：工具协议、执行器、与 agent 集成 | 规划中 |

原则：

1. 先核心、后外围；本仓不依赖尚未开源的 sibling。
2. 共用 C++20 + Conan；接口在稳定后再抽公共库。
3. 行为对齐 2026.10.5，不是 Node 包装层。
