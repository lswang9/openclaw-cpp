# Discord DAVE (voice E2EE) — assessment (1.93.0-dev)

## Facts (checked 2026-10-21 against Discord docs/blog)
- DAVE = Discord Audio & Video End-to-End Encryption: MLS (RFC 9420) group key agreement over the voice gateway
  + per-frame AES-128-GCM media encryption (truncated tag, ULEB128 nonce, unencrypted-range supplemental data,
  0xFAFA magic marker) applied to Opus frames *inside* the existing transport encryption.
- Since **2026-03-01** Discord only allows E2EE calls in DMs, group DMs, voice channels and Go Live. Clients/bots that
  do not support DAVE cannot join; the voice gateway closes with **4017 "E2EE/DAVE protocol required"**.
  Stage channels stay transport-encrypted only (no DAVE).
- Voice gateway opcodes 21–31 (prepare/execute transition, transition ready, prepare epoch, MLS external sender (25,
  binary), key package (26, binary), proposals (27, binary), commit/welcome (28, binary), announce commit
  transition (29, binary), welcome (30, binary), invalid commit/welcome (31)).
- Reference implementation: github.com/discord/libdave (C++ + JS). C++ needs **mlspp** (built with
  `MLS_CXX_NAMESPACE=mlspp`, `DISABLE_GREASE=ON`) and OpenSSL 1.1/3.0 or BoringSSL. Neither libdave nor mlspp is
  on ConanCenter; both would have to be vendored (FetchContent / git submodule) and built from source.

## What openclaw-cpp does today
- Voice transport (1.77/1.80): Identify → Ready → IP discovery → Select Protocol (aead_aes256_gcm_rtpsize /
  xchacha20) → Session Description → Opus RTP send/receive. Identify advertises `max_dave_protocol_version: 0`.
- 1.93: a 4017 close (or a Session Description announcing `dave_protocol_version > 0`) is surfaced as
  `{"dave_required": true, "close_code": 4017}` with this document referenced, instead of a generic timeout.
- **Consequence: live Discord voice (join/play/receive) does not work in DMs, group DMs and normal voice channels
  since 2026-03-01. It can still work in Stage channels.** Offline e2e (fake voice server) remains green because the
  fake does not demand DAVE unless `FAKE_DAVE_REQUIRED=1`.
- Text, slash commands, context menus, voice *messages* (Ogg/Opus attachments, 1.91) are unaffected by DAVE.

## Cost to implement
1. Vendor + build mlspp and libdave (C++20 OK; OpenSSL 3 already a dependency). Est. +2 large deps, CI time.
2. Voice gateway: binary WS frames (opcodes 25–30 are `seq(u16) | opcode(u8) | payload`), JSON opcodes 21–24/31,
   transition state machine (protocol version 0↔1 downgrade/upgrade, epochs, pending transitions).
3. Media: wrap outgoing Opus frames with libdave `Encryptor` before RTP transport encryption; unwrap incoming frames
   with per-user `Decryptor` (SSRC → user id map from opcode 11/13 + Speaking).
4. Testing: a fake voice gateway must act as MLS external sender and commit proposals — feasible in C++ against
   mlspp/libdave themselves (no Python MLS), so a self-test rather than a Python fake.
Rough size: 1.5–3k lines incl. tests; mostly integration glue. No user credentials needed to test offline.

## Recommendation
Implement as a dedicated slice (vendored libdave + mlspp behind `OPENCLAW_WITH_DAVE`), keep the honest 4017
reporting until then. PARITY tag for Discord voice: **partial (offline real; live blocked by DAVE except Stage)**.
