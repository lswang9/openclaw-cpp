# OpenClaw 2026.10.5 → openclaw-cpp 对齐表

**不宣称全量对齐。** 当前：**0.8.0-dev**。

| 上游模块 | C++ 状态 | 备注 |
|----------|----------|------|
| Gateway methods | **部分** | 广告 **516**；**实现清单 202**（核心深路径 + 分类 honest handlers；多数非 UI/媒体类仍为部分行为） |
| Provider routing | **部分** | `providers.list` / `providers.setDefault` / `models.setDefault` 热切换 |
| Control UI | **骨架** | 网关 `GET /` 内嵌最小 HTML：health/status + chat POST |
| Secrets at rest | **部分** | `OPENCLAW_SECRETS_PASSPHRASE` → AES-256-GCM（OpenSSL）；未设则明文+文件权限 |
| Multi-provider / persist / loop guard / MCP / channels | **部分** | 见 0.5–0.7 |
| Full Control UI / live IM / ACP / sandbox | TODO | |

## 里程碑

- ~~0.2–0.7~~ …
- **0.8.0-dev**：RPC 清单扩到 **202**、provider 热切换、嵌入式 Control UI、secrets AES-GCM
- 下一刀：把 honest handlers 逐个加深为真实逻辑；Control UI 接 WS；更多 channel 出站实网

诚实说明：`ImplementedMethods` 含大量 **category honest** 响应（返回本地 state + `honest:true`），不等于上游逐 API 完备。
