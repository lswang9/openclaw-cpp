#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <mutex>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

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
    // role: "device" (default, least privilege) or "operator" (may administer pairing).
    // scopes (optional, 1.83): restrict the token further. operator.read | operator.write | operator.approvals |
    // operator.pairing | operator.admin, or "rpc:<method-prefix>" for single RPC families. Empty = role default.
    nlohmann::json Approve(const std::string& id_or_code, const std::string& role = "device",
                           const nlohmann::json& scopes = nlohmann::json::array());
    static bool ValidScope(const std::string& s);
    // Scope check for one RPC method (scopes empty = unrestricted beyond the role).
    static bool ScopeAllows(const std::vector<std::string>& scopes, const std::string& method,
                            const nlohmann::json& params = nlohmann::json());
    // Upstream "dynamic" methods: required scopes derived from params (method-scopes.ts
    // resolveDynamicLeastPrivilegeOperatorScopesForMethod). Empty → static MethodScope applies.
    static std::vector<std::string> DynamicScopes(const std::string& method, const nlohmann::json& params);
    // Narrow session grant (operator.sessions.read|write) that may stand in for read/write on session-bound methods.
    static std::string SessionScope(const std::string& method, const nlohmann::json& params);
    static std::string MethodScope(const std::string& method);  // the operator.* scope a method needs
    std::vector<std::string> TokenScopes(const std::string& token) const;
    nlohmann::json Deny(const std::string& id_or_code);
    nlohmann::json Claim(const std::string& link_id, const std::string& poll_secret);
    nlohmann::json Verify(const std::string& token) const;
    bool IsValidToken(const std::string& token) const;
    // Extract bearer / X-OpenClaw-Token and check against issued, non-revoked device tokens.
    bool AcceptsHeaders(const std::string& authorization, const std::string& x_token) const;
    // "" if not a valid, non-revoked device token; else its role.
    std::string TokenRole(const std::string& token) const;
    static bool ValidRole(const std::string& role) { return role == "device" || role == "operator"; }
    // Browser sessions (1.25): cookie carries a random session id; only SHA-256(sid) + SHA-256(token) persisted.
    // Session role is re-derived from the backing credential on every request (revocation ends the session).
    struct SessionInfo { std::string role; std::string csrf; std::string oidc_sub, refresh_token; bool recheck_due = false; std::vector<std::string> scopes; std::string webauthn_cred; };
    // Returns {sid, csrf}; credential_sha256 = SHA-256 of the gateway token or ocd_ token used at login.
    std::pair<std::string, std::string> CreateSession(const std::string& credential_sha256, bool is_gateway_token);
    // gateway_token_sha256: current gateway token hash ("" if none). Empty role = invalid/expired/revoked.
    // OIDC-backed session (1.70): role fixed at login; valid only while OIDC stays configured.
    std::pair<std::string, std::string> CreateIdentitySession(const std::string& subject, const std::string& role,
                                                              const std::string& refresh_token = {});
    // After a successful refresh: new role/refresh token, next recheck scheduled (OPENCLAW_OIDC_RECHECK_SEC, default 3600).
    // Passkey-backed session (1.84): valid while the credential stays registered (checked by the gateway).
    std::pair<std::string, std::string> CreateWebAuthnSession(const std::string& credential_id, const std::string& role);
    void UpdateIdentitySession(const std::string& sid, const std::string& role, const std::string& refresh_token);
    SessionInfo SessionLookup(const std::string& sid, const std::string& gateway_token_sha256);
    void DeleteSession(const std::string& sid);
    // Per-IP claim rate limiting (persisted). Set by the gateway per connection thread.
    static void SetThreadClientIp(const std::string& ip);
    static std::string ThreadClientIp();
    nlohmann::json Revoke(const std::string& id_or_token_id);
    std::filesystem::path Path() const { return path_; }
    static std::string Sha256Hex(const std::string& s);

private:
    void Load();
    void Save() const;
    nlohmann::json* FindLink(const std::string& id_or_code);
    void ExpireLocked();
    bool ClaimRateLimitedLocked(std::int64_t now);
    mutable std::mutex mu_;
    std::filesystem::path path_;
    nlohmann::json data_;
};

}  // namespace openclaw::auth
