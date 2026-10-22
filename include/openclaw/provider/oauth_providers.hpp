#pragma once
// 1.141: GitHub Copilot (GitHub device flow → copilot_internal/user → CAPI with the GitHub token) and
// OpenAI Codex / ChatGPT OAuth (device code or browser PKCE → authorization_code exchange → refresh; chatgpt-account-id).
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
// Browser PKCE (localhost callback on OPENCLAW_OAUTH_CALLBACK_PORT, default 1455). show(authorize_url, "").
// OPENCLAW_OAUTH_FAKE_BROWSER=1 fetches the authorize URL (for offline fakes); OPENCLAW_OAUTH_CODE(+STATE) skips the browser.
nlohmann::json CodexBrowserPkceLogin(const std::function<void(const std::string&, const std::string&)>& show);

// Decode the ChatGPT account id from an id/access token JWT ("https://api.openai.com/auth".chatgpt_account_id).
std::string CodexAccountIdFromJwt(const std::string& jwt);

void ResetCachesForTest();

}  // namespace openclaw::provider::oauth
