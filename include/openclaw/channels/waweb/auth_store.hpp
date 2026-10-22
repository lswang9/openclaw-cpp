#pragma once
// 1.156: WhatsApp Web auth state (Baileys useMultiFileAuthState / initAuthCreds equivalent) persisted through the
// gateway state store (SQLite when enabled, else JSON files), XEdDSA signing for the signed pre-key, and the
// pre-key upload IQ (Baileys getNextPreKeysNode / uploadPreKeys) incl. server-count driven top-up.
#include "openclaw/channels/waweb/noise.hpp"
#include "openclaw/channels/waweb/signal.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace openclaw::channels::waweb {

// XEdDSA (Signal curve25519_sign): sign with an X25519 private key; verifiable by VerifyXEdDsa(pub, msg, sig).
Bytes SignXEdDsa(const Bytes& mont_priv, const Bytes& msg);

struct AuthCreds {
    KeyPair noise_key;
    KeyPair pairing_ephemeral;
    signal::Identity signal;          // identity, registration id, signed pre-key (+sig), one-time pre-keys
    Bytes signed_prekey_sig;
    Bytes adv_secret;                 // 32 bytes
    std::uint32_t next_prekey_id = 1;
    std::uint32_t first_unuploaded_prekey_id = 1;
    std::string me_jid, platform;
    bool registered = false;
};

AuthCreds InitAuthCreds();
// <dir>/creds.json and <dir>/keys.json (pre-keys + sessions) via persist::StateRead/StateWrite.
std::optional<AuthCreds> LoadAuthCreds(const std::filesystem::path& dir);
bool SaveAuthCreds(const std::filesystem::path& dir, const AuthCreds& c);
// Session records keyed by "<jid>.<device>".
bool SaveSession(const std::filesystem::path& dir, const std::string& addr, const signal::Session& s);
std::optional<signal::Session> LoadSession(const std::filesystem::path& dir, const std::string& addr);

// Generate `count` new one-time pre-keys (ids from next_prekey_id) and build
// <iq xmlns="encrypt" type="set" to="s.whatsapp.net" id=..><registration/><type/><identity/><list><key>..</key></list><skey/></iq>.
BinaryNode BuildPreKeyUploadNode(AuthCreds& c, int count, const std::string& iq_id);
// Parse <iq type="result"><count value="N"/></iq>; returns N.
std::optional<int> ParsePreKeyCount(const BinaryNode& n);
// Baileys MIN_PREKEY_COUNT=5 / INITIAL_PREKEY_COUNT=30 policy: how many to upload given the server count.
int PreKeysToUpload(int server_count);

}  // namespace openclaw::channels::waweb
