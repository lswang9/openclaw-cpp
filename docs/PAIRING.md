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

## Gaps

- Issued device tokens are **not yet** accepted by the HTTP gateway auth (`OPENCLAW_GATEWAY_TOKEN` still governs `/v1/*`)
- No scopes / roles, no QR, no rate-limit on claim, no cross-instance sync
- Legacy `channels.pairing.*` / `device.pair.*` stubs unchanged
