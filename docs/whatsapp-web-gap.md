# WhatsApp Web (Baileys-equivalent) — gap assessment (1.61)

Upstream OpenClaw's `whatsapp` channel logs in as a **linked device** through Baileys (WhatsApp Web multi-device
protocol): QR or pairing-code login, persistent auth state, DMs and groups, media, reactions, read receipts.
openclaw-cpp has only the **WhatsApp Cloud API** webhook/outbound subset (`src/channels/whatsapp.cpp`), which needs a
Meta Business number. That is a different product.

## Protocol layers and status

| Layer | What Baileys does | openclaw-cpp status |
|---|---|---|
| Transport | `wss://web.whatsapp.com/ws/chat`, binary frames: `WA 6 3` intro, then 3-byte BE length | **real** framing (`NoiseState::EncodeFrame/DecodeFrames`). The WebSocket client exists elsewhere (Slack/Discord) but is not wired here yet. |
| Noise handshake | `Noise_XX_25519_AESGCM_SHA256`; HandshakeMessage protobuf (clientHello/serverHello/clientFinish) | **real**: both roles, tested in-process; tampering is rejected. Not yet tested against the real WA server. |
| Server cert check | Verify the serverHello payload `CertChain` (WA root key, intermediate) | **missing** |
| ClientPayload | Protobuf: `UserAgent`, `WebInfo`, `DevicePairingRegistrationData` (reg id, signed pre-key, identity) for the first login, `username`/`device` afterwards | **missing**. Needs the WAProto schema (≈ 3k lines of .proto); we have only a varint/len-delimited codec. |
| WABinary nodes | XMPP-like `<iq>`/`<message>`/`<receipt>` trees, token dictionary v3 (single- and double-byte), JID encodings, nibble/hex packing | **missing**. Needs the token tables (Baileys `WAM/constants`). |
| Pairing | `pair-device` refs → QR `ref,noisePub,identityPub,advSecret`; `pair-success` → verify ADV HMAC, sign the device identity, reply `pair-device-sign` | QR payload format **real**. ADV signature/verification **missing**. |
| Signal protocol | X3DH session setup with pre-key bundles (`encrypt` iq), Double Ratchet (`pkmsg`/`msg`), group SenderKeys (`skmsg`), XEdDSA signatures | **missing**. This is the biggest piece. libsignal-protocol-c (GPL-3) could be linked, or we write our own (Curve25519/XEdDSA, AES-CBC, HMAC chains). |
| Pre-key management | Upload 30+ one-time pre-keys, rotate the signed pre-key, `count` iq | **missing** |
| App state sync | `app-state-sync-key-share`, LTHash mutations (contacts, mute, pin, archive) | **missing**. Not required for messaging. |
| History sync | Compressed `HistorySyncNotification` blobs | **missing**. Optional. |
| Media | HKDF("WhatsApp {Image,Video,Audio,Document} Keys") → AES-256-CBC + HMAC-SHA256/10; upload to `mmg.whatsapp.net` with `media_conn` auth | Crypto **real** (round trip, wrong-type MAC rejected). Upload/download hosts **missing**. |
| Auth persistence | `creds.json` + keys store (sessions, pre-keys, sender keys, app-state keys) | **missing**. The SQLite KV is available to reuse. |

## Why we stop at primitives now

- It cannot be tested end to end offline. A real WA login needs a phone to scan the QR and the live server. Our rules
  forbid real credentials in tests, so every higher layer can only be tested against our own fake server, which
  would mostly validate itself.
- Signal is security-critical. Shipping a homemade Double Ratchet without the official test vectors would be
  dishonest to tag as `real`.
- ToS and account risk: unofficial clients can get numbers banned. Upstream accepts this through Baileys; for us it
  is a user decision.

## Proposed path (ordered)

1. WAProto subset (`HandshakeMessage`, `ClientPayload`, `CertChain`, `ADVSignedDeviceIdentity*`, `Message` text
   fields) as a hand-written codec, plus server cert verification.
2. The WABinary codec with dictionary v3, tested with captured, public Baileys fixtures.
3. A `whatsapp-web login` CLI that runs a live QR login (needs the user: phone plus willingness to risk the number).
4. Signal: link libsignal (check GPL compatibility) or implement it against the libsignal test vectors.
5. Messaging: send and receive 1:1 text first, then groups (SenderKey), then media upload.

**Blocker needing the user:** a decision to accept the unofficial-client risk, and a phone number or device to pair
for any live validation (step 3 onward).
