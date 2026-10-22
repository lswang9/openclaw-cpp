#pragma once
// 1.164: Signal Sender Keys for WhatsApp group messaging (Baileys/libsignal-shaped). Self-consistent offline;
// AES-256-CBC + chain HMAC 0x01/0x02 + HKDF("WhisperGroup"); messages signed with XEdDSA.
#include "openclaw/channels/waweb/auth_store.hpp"

#include <map>
#include <optional>
#include <string>

namespace openclaw::channels::waweb {

struct SenderKeyState {
    std::uint32_t key_id = 0;
    std::uint32_t iteration = 0;
    Bytes chain_key;           // 32
    KeyPair signing;           // X25519; private empty on receiver-only copies
    std::map<std::uint32_t, Bytes> skipped;  // iteration → message-key seed
};

struct SenderKeyRecord {
    std::string group_id;      // e.g. "120363-group@g.us"
    std::string sender_addr;   // e.g. "1555:0" (jid user:device)
    std::vector<SenderKeyState> states;  // newest last; we keep ≤5
};

// Create a fresh sender state (for our own sends into a group).
SenderKeyState CreateSenderKey(std::uint32_t key_id = 0);
// Encode/decode SenderKeyDistributionMessage (version 0x33 + protobuf id/iteration/chainKey/signingKey).
Bytes EncodeSkdm(const SenderKeyState& s);
std::optional<SenderKeyState> DecodeSkdm(const Bytes& raw, std::string* err = nullptr);
// Process a received distribution message into a receiver-only state (no signing private).
SenderKeyState ProcessSkdm(const Bytes& raw, std::string* err = nullptr);

struct SkmEnvelope { Bytes body; std::uint32_t key_id = 0, iteration = 0; };
// Encrypt as the group's sender; advances state. Requires signing.priv.
SkmEnvelope SenderKeyEncrypt(SenderKeyState& s, const Bytes& plaintext);
// Decrypt; handles out-of-order within MAX_SKIP=2000. Verifies XEdDSA signature.
std::optional<Bytes> SenderKeyDecrypt(SenderKeyState& s, const Bytes& skm, std::string* err = nullptr);

// Persist under waweb-keys.json → "sender-key"."<group>|<sender>".
bool SaveSenderKey(const std::filesystem::path& dir, const SenderKeyRecord& r);
std::optional<SenderKeyRecord> LoadSenderKey(const std::filesystem::path& dir, const std::string& group_id,
                                             const std::string& sender_addr);

}  // namespace openclaw::channels::waweb
