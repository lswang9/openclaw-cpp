#include "openclaw/auth/gateway_auth.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace openclaw::auth {

AuthConfig FromEnv() {
    AuthConfig c;
    if (const char* t = std::getenv("OPENCLAW_GATEWAY_TOKEN"); t && *t) {
        c.gateway_token = t;
        c.require_auth = true;
    }
    if (const char* r = std::getenv("OPENCLAW_GATEWAY_REQUIRE_AUTH"); r && std::string(r) == "1") {
        c.require_auth = true;
    }
    return c;
}

std::optional<std::string> CheckHttpAuth(const AuthConfig& cfg,
                                         std::string_view authorization_header,
                                         std::string_view x_openclaw_token) {
    if (!cfg.require_auth) {
        return std::nullopt;
    }
    if (cfg.gateway_token.empty()) {
        return "gateway auth required but OPENCLAW_GATEWAY_TOKEN is empty";
    }

    std::string bearer;
    const std::string auth(authorization_header);
    std::string lower = auth;
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (lower.rfind("bearer ", 0) == 0) {
        bearer = auth.substr(7);
        while (!bearer.empty() && (bearer.front() == ' ' || bearer.front() == '\t')) {
            bearer.erase(bearer.begin());
        }
    }

    if (bearer == cfg.gateway_token || std::string(x_openclaw_token) == cfg.gateway_token) {
        return std::nullopt;
    }
    return "unauthorized: provide Authorization: Bearer <OPENCLAW_GATEWAY_TOKEN> or X-OpenClaw-Token";
}

void PairingStore::Add(PairingRequest req) { pending_.push_back(std::move(req)); }

bool PairingStore::Approve(const std::string& id) {
    for (auto& p : pending_) {
        if (p.id == id) {
            p.approved = true;
            return true;
        }
    }
    return false;
}

bool PairingStore::Dismiss(const std::string& id) {
    const auto n = pending_.size();
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const PairingRequest& p) { return p.id == id; }),
                   pending_.end());
    return pending_.size() != n;
}

std::vector<PairingRequest> PairingStore::List() const { return pending_; }

nlohmann::json PairingStore::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : pending_) {
        arr.push_back({{"id", p.id},
                       {"channel", p.channel},
                       {"code", p.code},
                       {"sender", p.sender},
                       {"approved", p.approved}});
    }
    return arr;
}

}  // namespace openclaw::auth
