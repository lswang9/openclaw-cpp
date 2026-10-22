#pragma once
// 1.141: GitHub Copilot (GitHub device flow → copilot_internal/user → CAPI with the GitHub token) and
// OpenAI Codex / ChatGPT OAuth (device code → authorization_code exchange → refresh; chatgpt-account-id).
// Endpoints are overridable for offline fakes: OPENCLAW_GITHUB_BASE (github.com), OPENCLAW_GITHUB_API_BASE
// (api.github.com), OPENCLAW_OPENAI_AUTH_BASE (auth.openai.com).
#include "openclaw/provider/native.hpp"

#include <functional>
#include <string>

namespace openclaw::provider::oauth {

// Fill base_url / api_key / headers for auth "github-copilot" or "openai-codex". false + why on failure.
bool ApplyRuntimeAuth(const nlohmann::json& cfg_raw, ProviderSpec* s, std::string* why);

// Device-code logins. `show(url, user_code)` is called once; returns the credential to store
// ({type, provider, ...}) or {error}. poll_interval_ms_override < 0 keeps the server interval.
nlohmann::json CopilotDeviceLogin(const std::function<void(const std::string&, const std::string&)>& show, int poll_interval_ms_override = -1);
nlohmann::json CodexDeviceLogin(const std::function<void(const std::string&, const std::string&)>& show, int poll_interval_ms_override = -1);

// Decode the ChatGPT account id from an id/access token JWT ("https://api.openai.com/auth".chatgpt_account_id).
std::string CodexAccountIdFromJwt(const std::string& jwt);

void ResetCachesForTest();

}  // namespace openclaw::provider::oauth
