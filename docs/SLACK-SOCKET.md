# Slack Socket Mode (openclaw-cpp)

Inbound via **WebSocket** (`apps.connections.open` + WSS envelopes), in addition to HTTP Events API webhook.

## Env

| Variable | Role |
|----------|------|
| `SLACK_APP_TOKEN` | App-level token `xapp-...` (connections.open + Socket Mode) |
| `SLACK_BOT_TOKEN` | Bot token `xoxb-...` (optional here; used for future outbound API) |
| `SLACK_VERIFY_TOKEN` | Legacy verification token for HTTP Events API |
| `SLACK_SOCKET_FORCE_MOCK=1` | Never open WSS (CI / offline). Use `slack.socket.feed` |

## RPC

- `slack.status` / `slack.socket.status` — adapter + socket counters
- `slack.socket.feed` — feed one envelope or `{ "frames": [ ... ] }` (mock/offline)
- `slack.socket.start` / `slack.socket.stop` — live loop when `SLACK_APP_TOKEN` set
- HTTP: `POST /api/channels/slack/inbound` (Events API) unchanged

## Envelope types handled

| type | Behavior |
|------|----------|
| `hello` | Ignored (no ack) |
| `events_api` | Same routing as webhook `event_callback` → agent |
| `slash_commands` | `command` + `text` → agent; optional response payload on ack |
| `interactive` | Ack only (no block-kit UI) |
| `disconnect` | Record reason; live loop exits |

Ack: `{"envelope_id":"..."}` (+ optional `payload` when `accepts_response_payload`).

## Honest gaps vs full Slack Socket Mode

- No interactive/block-kit / modal full handling
- No auto-reconnect with jittered backoff UI
- No rate-limit / warning envelope specialization
- Live WSS only when curl WebSocket + app token available
- Outbound still separate (record / future chat.postMessage)

`impl`: live connected → `real`; mock feed → `real-sim`.
