#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openclaw::auth {

struct AuthConfig {
    std::string gateway_token;  // OPENCLAW_GATEWAY_TOKEN; empty = auth off
    bool require_auth = false;  // true when token non-empty
};

AuthConfig FromEnv();

// nullopt = allowed; else error message.
std::optional<std::string> CheckHttpAuth(const AuthConfig& cfg,
                                         std::string_view authorization_header,
                                         std::string_view x_openclaw_token);

struct PairingRequest {
    std::string id;
    std::string channel;
    std::string code;
    std::string sender;
    bool approved = false;
};

class PairingStore {
public:
    void Add(PairingRequest req);
    bool Approve(const std::string& id);
    bool Dismiss(const std::string& id);
    std::vector<PairingRequest> List() const;
    nlohmann::json ListJson() const;

private:
    std::vector<PairingRequest> pending_;
};

}  // namespace openclaw::auth
