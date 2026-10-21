# Discord DAVE (voice E2EE) — implemented in 1.102.0-dev (assessment from 1.93 kept below)

## 1.102: implemented (libdave + mlspp)
- Build: `scripts/build-dave.sh` clones and builds pinned **libdave 8de72b1f8a2ac3c5a5270755bb8091a62e3c6169** and
  **mlspp 1cc50a124a3bc4e143a787ec934280dc70c1034d** (the revision libdave's openssl_3 overlay pins) with
  `MLS_CXX_NAMESPACE=mlspp`, `DISABLE_GREASE=ON`, against the same Conan OpenSSL/nlohmann_json as the main binary, into
  `third_party/dave/prefix` (git-ignored). CMake links it automatically when present (`OPENCLAW_WITH_DAVE`, default ON)
  and defines `OPENCLAW_HAVE_DAVE`; otherwise the old behaviour (max_dave_protocol_version 0 + honest 4017) stays.
- Client (`src/channels/dave.cpp`, used by `discord_voice::Play`): Identify advertises libdave's max protocol version;
  Session Description `dave_protocol_version` → session init (group id = voice channel id, self user id) + key
  package (op 26, binary); op 25 external sender (also accepted before op 4); op 27 proposals → commit/welcome (op 28);
  op 29 announce-commit / op 30 welcome → transition ready (op 23) → op 22 execute switches the encryptor/decryptors
  to the new key ratchets (transition 0 executes immediately); op 21 prepare-transition (incl. downgrade to 0), op 24
  prepare-epoch (epoch 1 → re-init), op 31 on a rejected commit/welcome followed by a fresh key package; op 11/13
  maintain the recognized roster. Media: Opus frames are DAVE-encrypted before RTP transport encryption and
  DAVE-decrypted per speaker (ssrc → user from Speaking) after transport decryption. Media is not sent until the
  group is established (`dave_timeout_ms`, default 8 s). `discord.voice.status` reports `dave.available`.
- Offline test `scripts/e2e-discord-dave.sh`: the Python fake voice server delegates MLS to `openclaw-dave-helper`
  (C++, mlspp + libdave): a real external sender signs the Add proposal, a real libdave peer joins via the client's
  welcome, decrypts every client frame (no plaintext leaks) and sends DAVE-encrypted audio that the client decrypts and
  Opus-decodes. Skips honestly when built without libdave.
- Gaps: not tested against live Discord (needs a bot token + voice channel); video codecs not wired (audio only);
  no pairwise fingerprint / verification-code UI; persistent signature keys off (transient per session); CI builds
  without libdave unless `scripts/build-dave.sh` runs first (workflows are not modified), so CI skips the DAVE test.

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
