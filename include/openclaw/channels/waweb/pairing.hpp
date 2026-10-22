#pragma once
// 1.162: ClientPayload (first-login registration + subsequent login) and ADV pair-success verification /
// pair-device-sign reply, shaped after Baileys validate-connection.ts. Offline only.
#include "openclaw/channels/waweb/auth_store.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::channels::waweb {

// ADV signature prefixes (Baileys Defaults).
inline const Bytes kAdvAccountSigPrefix{6, 0};
inline const Bytes kAdvDeviceSigPrefix{6, 1};
inline const Bytes kAdvHostedAccountSigPrefix{6, 5};

struct ClientPayloadOpts {
    std::array<int, 3> version{2, 3000, 1043857760};
    std::string browser_os = "Mac OS";       // DeviceProps.os / UserAgent device platform hint
    std::string browser_name = "Chrome";    // PlatformType CHROME=1
    std::string country = "US";
    std::string push_name;
    bool sync_full_history = false;
};

// Encode ClientPayload for a registered companion (username + device).
Bytes EncodeClientPayloadLogin(std::uint64_t username, std::uint32_t device, const ClientPayloadOpts& o = {});
// Encode ClientPayload for the first Noise finish (devicePairingData filled from AuthCreds).
Bytes EncodeClientPayloadRegistration(const AuthCreds& c, const ClientPayloadOpts& o = {});
// Decode a subset of ClientPayload fields (username, device, passive, pull, has pairing data).
struct ClientPayloadInfo {
    std::uint64_t username = 0;
    std::uint32_t device = 0;
    bool passive = false, pull = false, has_pairing = false;
    Bytes e_ident, e_skey_val, e_skey_sig, build_hash, device_props;
    std::uint32_t registration_id = 0, signed_prekey_id = 0;
};
std::optional<ClientPayloadInfo> DecodeClientPayload(const Bytes& raw);

struct AdvAccount {
    Bytes details;                 // ADVDeviceIdentity protobuf
    Bytes account_signature_key;   // phone identity pub (32)
    Bytes account_signature;       // 64
    Bytes device_signature;        // 64 (filled by ConfigureSuccessfulPairing)
    std::uint32_t key_index = 0;
    bool hosted = false;
};

struct PairSuccessResult {
    bool ok = false;
    std::string error;
    std::string me_jid, me_lid, platform, biz_name;
    AdvAccount account;
    BinaryNode reply;  // <iq type=result><pair-device-sign><device-identity key-index=N>…</device-identity></pair-device-sign></iq>
};

// Verify ADV HMAC + account signature, sign the device identity, build the pair-device-sign reply.
// Updates creds.me_jid / platform / registered on success (caller should SaveAuthCreds).
PairSuccessResult ConfigureSuccessfulPairing(const BinaryNode& iq, AuthCreds& creds);

// Offline helper: build a pair-success iq that ConfigureSuccessfulPairing will accept (signs with account_priv).
BinaryNode BuildPairSuccessIq(const AuthCreds& companion, const KeyPair& account_identity, const std::string& jid,
                              const std::string& lid = "", const std::string& platform = "smba",
                              const std::string& iq_id = "pair1", bool hosted = false);

}  // namespace openclaw::channels::waweb
