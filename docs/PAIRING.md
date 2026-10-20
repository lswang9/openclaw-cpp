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

## Roles (1.24)

Chosen at approve time (`pairing.link.approve {id, role}` or the selector in `/pairing`), persisted on the link and token.

| Caller | Source | May |
|--------|--------|-----|
| `admin` | `OPENCLAW_GATEWAY_TOKEN`, or auth off | everything |
| `operator` | `ocd_` token approved with `role:"operator"` | pairing admin (list/approve/deny/revoke); **not** `secrets.*`, `config.set|patch|apply`, `update.*`, gateway stop/restart |
| `device` (default) | `ocd_` token approved with `role:"device"` | normal RPC/chat/tools; **not** `pairing.*` admin, `channels.pairing.*`, `device.pair*`, `device.token*`, `node.pair*`, `secrets.*`, `config.*`, `approvals.*`, plugin install, `acp.auth.*`, `GET /v1/config` |

Forbidden RPCs return `{"ok":false,"error":"forbidden: <role> role cannot call …","code":403}`. Applies to HTTP `/v1/rpc` and WS frames.
Tokens issued before 1.24 (no stored role) are treated as `device`. `GET /v1/whoami` reports the caller role.

## Control UI login (1.24, hardened 1.25)

- `/login` (public): paste gateway or `ocd_` token → `POST /login` validates and creates a **server-side session**. Cookies: `oc_session=ocs_…` (random id, **HttpOnly; SameSite=Strict**, 30 days, `OPENCLAW_SESSION_TTL_SEC`) and `oc_csrf` (readable by JS). The raw token is never put in a cookie; the store keeps only SHA-256(session id) and SHA-256(credential).
- Session role is re-derived on every request: revoking the `ocd_` token or rotating `OPENCLAW_GATEWAY_TOKEN` ends the session. `POST /logout` deletes it server-side. The 1.24 `oc_token` raw cookie is no longer accepted (cleared on login).
- **CSRF**: any non-GET request authenticated *only* by the session cookie must send `X-CSRF-Token` equal to the session's token (double-submit, checked server-side against the stored value) → else 403. UI pages get a small injected `fetch` wrapper that adds the header. Bearer / `X-OpenClaw-Token` requests are not CSRF-checked.
- **WS**: cookie-authenticated WebSocket upgrades require `Origin` matching `Host` (anti cross-site WS hijacking). Role checks apply to every WS RPC frame (smoke covers device/operator over WS).
- With auth on, `/`, `/board`, `/canvas`, `/pairing` redirect (302) to `/login?next=…` when unauthenticated; with the cookie they load and their same-origin `/v1/rpc` calls authenticate via the cookie.

## Gaps

- Roles are a fixed denylist (two roles), not fine-grained scopes; non-RPC HTTP endpoints besides `/v1/config` are not role-filtered
- Claim rate limit is per client IP (`OPENCLAW_PAIR_CLAIM_MAX_PER_MIN`, 60s window) persisted in `device_links.json`; IP is the TCP peer (no `X-Forwarded-For` trust, so behind a reverse proxy all clients share one bucket); single-process file store, no cross-instance sharing
- No session listing/admin UI, no idle timeout (absolute TTL only), no MFA/OIDC
- Rate limit is in-process (resets on restart), global not per-IP
- No QR, push approval, cross-instance sync, or upstream cloud device-link
- Control UI pages (`/pairing`, `/board`…) load without a header-capable login when auth is on
