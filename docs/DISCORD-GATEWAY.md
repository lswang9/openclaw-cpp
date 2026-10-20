# Discord Gateway (openclaw-cpp)

Inbound via **Discord Gateway WebSocket** (identify + `MESSAGE_CREATE` + heartbeats), in addition to HTTP interactions webhook.

## Env

| Variable | Role |
|----------|------|
| `DISCORD_BOT_TOKEN` | Bot token for Identify + `GET /gateway/bot` |
| `DISCORD_GATEWAY_INTENTS` | Optional intents bitmask (default GUILDS\|GUILD_MESSAGES\|MESSAGE_CONTENT\|DIRECT_MESSAGES) |
| `DISCORD_GATEWAY_FORCE_MOCK=1` | Never open WSS (CI). Use `discord.gateway.feed` |
| `DISCORD_VERIFY_TOKEN` | Optional HTTP inbound verify |

## RPC

- `discord.status` / `discord.gateway.status`
- `discord.gateway.feed` — one payload or `{ "frames": [ ... ] }`
- `discord.gateway.start` / `stop` — live loop when token set
- HTTP: `POST /api/channels/discord/inbound` unchanged

## Opcode handling

| op | Role |
|----|------|
| 10 Hello | Record interval; queue Identify |
| 2 Identify | Sent after Hello (live + mock feed returns `send`) |
| 0 Dispatch | `READY`, `MESSAGE_CREATE` → agent (via DeliverInbound / ACP bind) |
| 1 / 11 | Heartbeat / ACK |
| 7 / 9 | Reconnect / invalid session (recorded) |

Bot authors ignored on `MESSAGE_CREATE`.

## Honest gaps

- No slash commands / component interactions over Gateway
- No voice
- No full resume / sharded reconnect UX
- `MESSAGE_CONTENT` is a privileged intent (must enable in Discord portal)
- Not full discord.js feature set

`impl`: live → `real`; mock feed → `real-sim`.
