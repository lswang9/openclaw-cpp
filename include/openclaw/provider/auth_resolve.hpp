#pragma once
// Resolve API keys / OAuth access tokens for built-in providers from auth-profiles.json.
// Supports multi-profile rotation: auth.order.<provider> = ["provider:a","provider:b"] (config), else
// all profiles for that provider with ":default" first. On HTTP 401/429 the next profile is tried.
#include "openclaw/auth/profiles.hpp"
#include "openclaw/provider/provider.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::provider {

struct ResolvedAuth {
    std::string profile_id;
    std::string token;   // api key or oauth access
    std::string type;    // api_key | oauth | env
};

// Collect candidate profile ids for a provider (config auth.order, else store scan).
std::vector<std::string> ProfileOrder(const std::string& provider, const nlohmann::json& cfg_raw,
                                      const auth::ProfileStore& store);

// Resolve one profile to a bearer token (refreshes oauth via optional refresh callback).
// refresh: (cred) -> updated cred object or null.
using OAuthRefreshFn = std::function<nlohmann::json(const nlohmann::json& cred)>;
std::optional<ResolvedAuth> ResolveProfile(const std::string& profile_id, auth::ProfileStore& store,
                                           OAuthRefreshFn refresh = {});

// Provider that re-resolves credentials per call and rotates on 401/429.
// `inner_factory(token)` builds the underlying OpenAI-compat/DeepSeek client for that token.
class AuthRotatingProvider : public Provider {
public:
    using Factory = std::function<std::shared_ptr<Provider>(const std::string& token)>;
    AuthRotatingProvider(std::string provider_id, Factory factory, std::string env_fallback_token = {});
    void SetConfig(nlohmann::json cfg) { cfg_ = std::move(cfg); }
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages,
                                     const nlohmann::json& tools) override;
    // Last profile that succeeded (or was attempted).
    std::string LastProfileId() const { return last_profile_; }

private:
    std::string provider_id_;
    Factory factory_;
    std::string env_fallback_;
    nlohmann::json cfg_ = nlohmann::json::object();
    std::string last_profile_;
    std::string last_agent_;
public:
    std::string LastAgentStore() const { return last_agent_; }
};

}  // namespace openclaw::provider
