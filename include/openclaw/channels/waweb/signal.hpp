#pragma once
// Signal protocol offline layers shaped after libsignal v3 (as used by WhatsApp/Baileys): X3DH key agreement
// ("WhisperText"), Double Ratchet (root "WhisperRatchet", chain HMAC 0x01/0x02, "WhisperMessageKeys" → AES-256-CBC +
// HMAC-SHA256/8), SignalMessage / PreKeySignalMessage protobuf framing (version 0x33), skipped-key handling.
// Self-consistent and spec-faithful; NOT verified byte-for-byte against libsignal (no fixtures offline).
#include "openclaw/channels/waweb/noise.hpp"

#include <map>
#include <optional>
#include <string>

namespace openclaw::channels::waweb::signal {

Bytes SerializePub(const Bytes& pub32);  // 0x05 || pub

struct PreKeyBundle {
    std::uint32_t registration_id = 0, prekey_id = 0, signed_prekey_id = 0;
    Bytes identity_pub, signed_prekey_pub, signed_prekey_sig, prekey_pub;  // 32-byte raw keys; prekey optional
};

struct Identity {
    KeyPair identity;
    std::uint32_t registration_id = 0;
    KeyPair signed_prekey; std::uint32_t signed_prekey_id = 1;
    std::map<std::uint32_t, KeyPair> prekeys;
    PreKeyBundle Bundle(std::uint32_t prekey_id = 0) const;
};
Identity NewIdentity(std::uint32_t registration_id, int n_prekeys = 2);

struct Session {
    Bytes local_identity, remote_identity;  // raw 32
    Bytes root_key;
    KeyPair our_ratchet;
    Bytes their_ratchet;
    Bytes send_chain; std::uint32_t send_n = 0, prev_n = 0;
    Bytes recv_chain; std::uint32_t recv_n = 0;
    std::map<std::string, Bytes> skipped;  // hex(ratchet)+":"+n → message-key seed
    // Pending prekey header (alice until first reply)
    bool pending_prekey = false;
    std::uint32_t pk_reg = 0, pk_prekey_id = 0, pk_signed_id = 0;
    Bytes pk_base_key;
    std::uint32_t local_reg = 0;
};

// Alice: build a session from Bob's bundle. Verifies the signed-prekey XEdDSA signature when present.
std::optional<Session> InitiateOutgoing(const Identity& self, const PreKeyBundle& bob, std::string* err = nullptr);
// Encrypt → {type: "pkmsg"|"msg", body}. pkmsg while no reply received yet.
struct Envelope { std::string type; Bytes body; };
Envelope Encrypt(Session& s, const Bytes& plaintext);
// Bob: accept a pkmsg (creates session, consumes one-time prekey). Returns plaintext.
std::optional<Bytes> DecryptPreKey(Identity& self, std::optional<Session>& out_session, const Bytes& pkmsg, std::string* err = nullptr);
std::optional<Bytes> Decrypt(Session& s, const Bytes& msg, std::string* err = nullptr);

}  // namespace openclaw::channels::waweb::signal
