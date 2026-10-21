#pragma once
// OpenID Connect authorization-code + PKCE (S256) login for the gateway UI. Env-configured:
// OPENCLAW_OIDC_ISSUER, OPENCLAW_OIDC_CLIENT_ID, [OPENCLAW_OIDC_CLIENT_SECRET], [OPENCLAW_OIDC_REDIRECT_URI],
// OPENCLAW_OIDC_ALLOWED_EMAILS / OPENCLAW_OIDC_ALLOWED_SUBS / OPENCLAW_OIDC_ALLOWED_DOMAINS (comma lists; at least one
// required — fail closed), OPENCLAW_OIDC_ROLE (operator|admin, default operator), OPENCLAW_OIDC_SCOPES.
// id_token: RS256 via JWKS, iss/aud/azp/exp/iat/nonce checked; email_verified required for email allowlists.
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace openclaw::auth::oidc {

struct Config {
    std::string issuer, client_id, client_secret, redirect_uri, role = "operator", scopes = "openid email profile";
    std::string allowed_emails, allowed_subs, allowed_domains;
    bool Enabled() const { return !issuer.empty() && !client_id.empty(); }
};
Config FromEnv();

struct Start { std::string url, state; std::string error; };
// redirect_uri_default is used when OPENCLAW_OIDC_REDIRECT_URI is unset (derived from the request Host).
Start BeginLogin(const Config& c, const std::string& redirect_uri_default);

struct Result {
    bool ok = false;
    std::string sub, email, role, error;
};
Result FinishLogin(const Config& c, const std::string& code, const std::string& state);

// Exposed for tests: verify a compact JWS (RS256) id_token against a JWKS document.
std::optional<nlohmann::json> VerifyRs256(const std::string& jwt, const nlohmann::json& jwks, std::string* err);
std::string PkceChallenge(const std::string& verifier);

}  // namespace openclaw::auth::oidc
