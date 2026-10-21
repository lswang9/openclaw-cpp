#pragma once
// WebAuthn (passkeys) relying party for the gateway login (1.84). Registration: an already-authenticated principal
// gets creation options, the browser's attestation is parsed (CBOR attestationObject → authData → COSE key), and the
// credential is stored with the registering principal's role (no escalation). Authentication: public assertion
// options; the assertion is verified (type, single-use challenge, origin, rpIdHash, UP flag, signCount, ES256 /
// EdDSA / RS256 signature over authData || SHA-256(clientDataJSON)) and yields a session with the stored role.
// Attestation (1.92): fmt none / packed (self + x5c basic, cert requirements, aaguid ext) / fido-u2f are verified;
// an invalid statement is always rejected. OPENCLAW_WEBAUTHN_ATTESTATION=none (default: accept none/unsupported
// formats as unverified) | required (self or basic) | trusted (x5c chains to OPENCLAW_WEBAUTHN_TRUST_ANCHORS PEM).
// tpm / android-key / apple / android-safetynet are not verified.
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace openclaw::auth::webauthn {

struct Rp {
    std::string id;      // OPENCLAW_WEBAUTHN_RP_ID, else Host without port
    std::string origin;  // OPENCLAW_WEBAUTHN_ORIGIN, else http://<Host>
    std::string name = "openclaw-cpp";
};
Rp RpFor(const std::string& host_header);

// Minimal CBOR (RFC 8949) decoder: maps (int keys → decimal strings), arrays, byte strings (json binary), text,
// ints, simple values. Returns nullopt on malformed input; *used = bytes consumed.
std::optional<nlohmann::json> DecodeCbor(const std::string& in, std::size_t* used = nullptr);
std::string B64UrlEncode(const std::string& raw);
std::string B64UrlDecode(const std::string& s);

class Store {
public:
    explicit Store(std::filesystem::path path);
    nlohmann::json BeginRegistration(const Rp& rp, const std::string& user, const std::string& role);
    nlohmann::json FinishRegistration(const Rp& rp, const nlohmann::json& credential);
    nlohmann::json BeginAuthentication(const Rp& rp);
    // ok → {ok, credential_id, user, role, sign_count}
    nlohmann::json FinishAuthentication(const Rp& rp, const nlohmann::json& credential);
    bool Active(const std::string& credential_id) const;
    nlohmann::json List() const;
    nlohmann::json Remove(const std::string& credential_id);
    bool Enabled() const;  // at least one credential registered

private:
    void Load();
    void Save() const;
    std::optional<nlohmann::json> TakeChallenge(const std::string& challenge_b64u, const std::string& type);
    mutable std::mutex mu_;
    std::filesystem::path path_;
    nlohmann::json data_;
};

}  // namespace openclaw::auth::webauthn
