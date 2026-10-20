# WhatsApp Cloud API (openclaw-cpp)

Meta **Cloud API** webhook + outbound send. **Not Baileys** / multi-device WhatsApp Web.

## Env

| Variable | Role |
|----------|------|
| `WHATSAPP_VERIFY_TOKEN` | Webhook subscription verify |
| `WHATSAPP_TOKEN` or `WHATSAPP_ACCESS_TOKEN` | Graph API bearer |
| `WHATSAPP_PHONE_NUMBER_ID` | Phone number id for send |
| `WHATSAPP_API_BASE` | Default `https://graph.facebook.com` |
| `WHATSAPP_FORCE_MOCK=1` | Skip live outbound; use `whatsapp.feed` |

## HTTP

- **GET** `/api/channels/whatsapp/inbound?hub.mode=subscribe&hub.verify_token=…&hub.challenge=…`  
  → plain-text challenge (Meta verify)
- **POST** `/api/channels/whatsapp/inbound` — Cloud API webhook JSON → DeliverInbound / ACP bind

## RPC

- `whatsapp.status`
- `whatsapp.feed` — one payload or `{frames:[…]}` (CI)
- `whatsapp.send` — `{to, text}` via Graph when tokens set

## Outbound

`POST {API_BASE}/v21.0/{PHONE_NUMBER_ID}/messages` with `messaging_product=whatsapp`.

## Gaps vs Baileys / full WA

- No Baileys, QR login, or multi-device
- No media download/upload pipeline
- No template composer UI
- Text (+ button/list/caption markers) only

`impl:real` when token path used; mock feed `real-sim`.
