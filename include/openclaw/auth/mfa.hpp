#pragma once
// TOTP (RFC 6238 / RFC 4226, HMAC-SHA1, 30 s, 6 digits) second factor for gateway /login, plus single-use
// recovery codes. Secret: OPENCLAW_MFA_TOTP_SECRET (base32) or <state>/auth/mfa.json written by `auth mfa enroll`.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::auth::mfa {

using Bytes = std::vector<std::uint8_t>;
std::string Base32Encode(const Bytes& b);
std::optional<Bytes> Base32Decode(const std::string& s);
std::string Hotp(const Bytes& key, std::uint64_t counter, int digits = 6);
std::string Totp(const Bytes& key, std::uint64_t unix_s, int step = 30, int digits = 6);

std::string DefaultPath();

struct EnrollResult {
    std::string secret_b32;
    std::string otpauth_uri;
    std::vector<std::string> recovery_codes;
};

class Store {
public:
    explicit Store(std::string path = DefaultPath());
    bool Enabled() const;            // env secret or enrolled file
    bool FromEnv() const;
    EnrollResult Enroll(const std::string& account = "openclaw");
    bool Disable();
    // Accepts a 6-digit TOTP (±1 step, replay-protected: counter must exceed the last accepted one) or an unused
    // recovery code (consumed). Persists state. now_s = 0 → current time.
    bool Verify(const std::string& code, std::uint64_t now_s = 0, std::string* how = nullptr);
    std::string CurrentCode(std::uint64_t now_s = 0) const;
    int RecoveryRemaining() const;

private:
    std::string path_;
};

}  // namespace openclaw::auth::mfa
