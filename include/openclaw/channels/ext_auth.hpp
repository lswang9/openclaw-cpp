#pragma once
// 1.132: inbound authentication + live listeners for Signal / Matrix / MS Teams / Google Chat / Mattermost.
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>
#include "openclaw/channels/adapter.hpp"

namespace openclaw::channels {
// RS256 JWT check against a JWKS URL (cached). Validates signature, exp/nbf (5 min skew), iss and aud.
bool VerifyJwtRs256(const std::string& jwt, const std::string& jwks_url, const std::vector<std::string>& issuers,
                    const std::string& audience, std::string* why, nlohmann::json* claims = nullptr);
// Header lookup (case-insensitive) supplied by the HTTP layer.
using HeaderFn = std::function<std::string(const std::string& name)>;
// Returns true when the request may proceed. Only enforced when the channel's auth settings are configured:
//  msteams: appId set (and verifyJwt != false) → Bot Framework JWT (openIdMetadataUrl, iss api.botframework.com, aud appId)
//  googlechat: audience set → Google Chat JWT (iss chat@system.gserviceaccount.com, aud = audience)
//  matrix: hsToken set → app-service "Authorization: Bearer <hs_token>" or ?access_token=
//  signal: webhookSecret set → X-Webhook-Secret header
//  mattermost: webhookToken is checked in the body by the adapter
bool VerifyExtInbound(const std::string& id, const nlohmann::json& cfg, const HeaderFn& header, const std::string& query, std::string* why);
// Long-lived pollers (Matrix /sync, Signal /v1/receive, Mattermost channel posts) until stop is set.
void RunExtListeners(const nlohmann::json& cfg, const std::filesystem::path& state_dir, const std::atomic<bool>& stop,
                     const std::function<Adapter*(const std::string&)>& adapter, const DeliverFn& deliver);
}  // namespace openclaw::channels
