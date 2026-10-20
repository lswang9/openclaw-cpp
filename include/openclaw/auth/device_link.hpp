#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <mutex>
#include <cstdint>
#include <string>
#include <vector>

namespace openclaw::auth {

// Device link / pairing-code flow persisted to disk (JSON).
// start → (operator approve|deny) → claim issues a bearer token once (only SHA-256 stored).
// Local gate only: not upstream OpenClaw cloud device-link / QR / push.
class DeviceLinkStore {
public:
    explicit DeviceLinkStore(std::filesystem::path path);
    nlohmann::json Start(const nlohmann::json& params);   // {device_name, channel?}
    nlohmann::json Status(const std::string& link_id) const;
    nlohmann::json List() const;                          // no secrets
    nlohmann::json Approve(const std::string& id_or_code);
    nlohmann::json Deny(const std::string& id_or_code);
    nlohmann::json Claim(const std::string& link_id, const std::string& poll_secret);
    nlohmann::json Verify(const std::string& token) const;
    bool IsValidToken(const std::string& token) const;
    // Extract bearer / X-OpenClaw-Token and check against issued, non-revoked device tokens.
    bool AcceptsHeaders(const std::string& authorization, const std::string& x_token) const;
    nlohmann::json Revoke(const std::string& id_or_token_id);
    std::filesystem::path Path() const { return path_; }
    static std::string Sha256Hex(const std::string& s);

private:
    void Load();
    void Save() const;
    nlohmann::json* FindLink(const std::string& id_or_code);
    void ExpireLocked();
    bool ClaimRateLimitedLocked(std::int64_t now);
    std::vector<std::int64_t> claim_times_;
    mutable std::mutex mu_;
    std::filesystem::path path_;
    nlohmann::json data_;
};

}  // namespace openclaw::auth
