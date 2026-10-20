# Feishu / Lark long-connection WebSocket (openclaw-cpp)

Inbound via **persistent WebSocket** in addition to HTTP event webhook.

Official Feishu docs prefer the **SDK-managed** long connection. This port implements a **JSON subset** plus mock/feed for CI, and a best-effort live path.

## Env

| Variable | Role |
|----------|------|
| `FEISHU_APP_ID` / `FEISHU_APP_SECRET` | Tenant access token |
| `FEISHU_WS_URL` | Optional explicit `wss://…` (skip discovery) |
| `FEISHU_API_BASE` | Default `https://open.feishu.cn` (Lark intl override) |
| `FEISHU_WS_FORCE_MOCK=1` | Never open WSS — use `feishu.ws.feed` |
| `FEISHU_VERIFY_TOKEN` | HTTP webhook verify |
| `FEISHU_WEBHOOK_URL` | Outbound webhook |

## RPC

- `feishu.status` / `feishu.ws.status`
- `feishu.ws.feed` — one payload or `{ "frames": [ ... ] }`
- `feishu.ws.start` / `stop`
- HTTP: `POST /api/channels/feishu/inbound` unchanged

## Frame shapes (mock / JSON live)

| type | Behavior |
|------|----------|
| `ping` | Reply `pong` |
| `event` | `data` = Events API body; `im.message.receive_v1` text → DeliverInbound/ACP |
| webhook-shaped | Same extract as HTTP inbound |

Ack: optional `{type:ack, message_id, code:0}` when `message_id` present.

## Live path

1. `POST …/auth/v3/tenant_access_token/internal`
2. `FEISHU_WS_URL` **or** best-effort `GET …/callback/ws/endpoint`
3. curl WebSocket; JSON ping/event only

## Honest gaps vs Feishu SDK

- Proprietary **binary/protobuf** framing used by official SDKs not fully decoded
- Card / url.preview callbacks not handled
- Enterprise self-built apps only (platform)
- Cluster mode (one of N clients)
- ~3s processing expectation on live

`impl`: live connected → `real`; mock feed → `real-sim`.
