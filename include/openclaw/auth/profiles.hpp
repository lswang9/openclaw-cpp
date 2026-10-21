#pragma once
// Upstream-style auth profiles: <stateDir>/agents/<agentId>/agent/auth-profiles.json (0600).
// {version:1, profiles:{"<provider>:<name>": {type:"api_key", provider, key} |
//                                             {type:"oauth", provider, access, refresh, expires(ms), email?}}}
#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <string>

namespace openclaw::auth {

std::filesystem::path DefaultProfilesPath(const std::string& agent_id = "main");

class ProfileStore {
public:
    explicit ProfileStore(std::filesystem::path path = DefaultProfilesPath());
    nlohmann::json Load() const;
    bool Upsert(const std::string& profile_id, const nlohmann::json& credential);
    bool Remove(const std::string& profile_id);
    std::optional<nlohmann::json> Get(const std::string& profile_id) const;
    // Preferred credential for a provider: `<provider>:default`, else the first `<provider>:*` profile.
    std::optional<std::pair<std::string, nlohmann::json>> ForProvider(const std::string& provider) const;
    nlohmann::json ListMasked() const;  // secrets redacted
    const std::filesystem::path& path() const { return path_; }

private:
    void Save(const nlohmann::json& doc) const;
    std::filesystem::path path_;
};

bool OAuthNeedsRefresh(const nlohmann::json& cred, long long skew_ms = 60000);

}  // namespace openclaw::auth
