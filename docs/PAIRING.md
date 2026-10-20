# Device pairing / link flow (openclaw-cpp)

Local **pairing-code** flow with operator approval. Served at **`/pairing`**.
**Not** upstream OpenClaw cloud device-link, QR scan, push approval, or per-scope device tokens.

## Flow

1. Device: `pairing.link.start {device_name}` → `{link:{id, code "ABCD-EFGH", expires_ms}, poll_secret}`
2. Operator: open `/pairing` (or RPC) → **Approve** / **Deny** (`pairing.link.approve|deny {id|code}`)
3. Device polls `pairing.link.claim {id, poll_secret}` → pending / denied / expired, or on approval a bearer **token** (`ocd_…`) shown **once**
4. Services check `pairing.token.verify {token}`; operator can `pairing.token.revoke {token_id}`

## Storage

`$OPENCLAW_SESSION_DIR/device_links.json` (mode 0600). Only **SHA-256** of `poll_secret` and tokens is persisted.
TTL: `OPENCLAW_PAIR_TTL_SEC` (default 600).

## RPC

`pairing.status`, `pairing.link.start|status|list|approve|deny|claim`, `pairing.token.verify|revoke`.

## Gateway auth (1.23)

When gateway auth is on (`OPENCLAW_GATEWAY_TOKEN` or `OPENCLAW_GATEWAY_REQUIRE_AUTH=1`), HTTP `/v1/*`, `/v1/rpc`
and WS upgrade accept **either** the gateway token **or** a non-revoked `ocd_` device token
(`Authorization: Bearer …` or `X-OpenClaw-Token`). Revoked/unknown tokens → 401.
Exempt without token: `/health`, and `/v1/rpc` calls to `pairing.link.start|claim|status` (so an unpaired
device can request + poll). Approve/deny/list/revoke still require auth.

## Rate limiting

- Global claim window: `OPENCLAW_PAIR_CLAIM_MAX_PER_MIN` (default 30) → `rate_limited`
- Per-link bad secret lockout: `OPENCLAW_PAIR_MAX_BAD_SECRET` (default 5) → state `locked`

## Legacy RPCs

| Legacy | Now |
|--------|-----|
| `device.pair.request` | → `pairing.link.start` |
| `device.pair.approve`, `channels.pairing.approve` | → `pairing.link.approve` when id/code is a device link; else channel `PairingStore` |
| `device.pair.reject|remove`, `channels.pairing.dismiss` | → `pairing.link.deny` for links; else channel `PairingStore` |
| `*.pair.list` | channel pairing + `device_links` |
| `device.pair.rename|resolve|…`, `device.token.*` | **legacy** config records, flagged `legacy:true`; `device.token.*` values are **not** accepted by gateway auth |

## Gaps

- **No scopes/roles**: any accepted device token has full gateway access (incl. approve/revoke)
- Rate limit is in-process (resets on restart), global not per-IP
- No QR, push approval, cross-instance sync, or upstream cloud device-link
- Control UI pages (`/pairing`, `/board`…) load without a header-capable login when auth is on
