#include "openclaw/provider/auth_resolve.hpp"

#include "openclaw/config/config.hpp"

namespace openclaw::provider {

std::vector<std::string> ProfileOrder(const std::string& provider, const nlohmann::json& cfg_raw,
                                      const auth::ProfileStore& store) {
    std::vector<std::string> out;
    if (auto order = config::GetPath(cfg_raw, "auth.order." + provider); order && order->is_array()) {
        for (auto& x : *order) if (x.is_string()) out.push_back(x.get<std::string>());
    }
    if (out.empty()) {
        auto all = store.Load();
        const std::string def = provider + ":default";
        if (all["profiles"].contains(def)) out.push_back(def);
        for (auto& [k, v] : all["profiles"].items()) {
            if (k == def) continue;
            if (v.is_object() && (v.value("provider", "") == provider || k.rfind(provider + ":", 0) == 0))
                out.push_back(k);
        }
    }
    return out;
}

std::optional<ResolvedAuth> ResolveProfile(const std::string& profile_id, auth::ProfileStore& store,
                                           OAuthRefreshFn refresh) {
    auto cred = store.Get(profile_id);
    if (!cred) return std::nullopt;
    if (auth::OAuthNeedsRefresh(*cred) && refresh) {
        auto next = refresh(*cred);
        if (next.is_object()) {
            if (!next.contains("type")) next["type"] = "oauth";
            if (!next.contains("provider")) next["provider"] = cred->value("provider", "");
            store.Upsert(profile_id, next);
            cred = next;
        }
    }
    ResolvedAuth r;
    r.profile_id = profile_id;
    r.type = cred->value("type", "api_key");
    if (r.type == "oauth") r.token = cred->value("access", "");
    else r.token = cred->value("key", "");
    if (r.token.empty()) return std::nullopt;
    return r;
}

AuthRotatingProvider::AuthRotatingProvider(std::string provider_id, Factory factory, std::string env_fallback)
    : provider_id_(std::move(provider_id)), factory_(std::move(factory)), env_fallback_(std::move(env_fallback)) {}

agent::CompletionResult AuthRotatingProvider::Complete(const std::vector<agent::Message>& messages,
                                                       const nlohmann::json& tools) {
    auth::ProfileStore store;
    auto order = ProfileOrder(provider_id_, cfg_, store);
    std::vector<ResolvedAuth> candidates;
    for (auto& id : order) {
        if (auto r = ResolveProfile(id, store)) candidates.push_back(*r);
    }
    if (candidates.empty() && !env_fallback_.empty() && env_fallback_ != "missing") {
        candidates.push_back({"env", env_fallback_, "env"});
    }
    agent::CompletionResult last;
    last.ok = false;
    last.finish_reason = "error";
    last.error = "no auth profile or env key for provider " + provider_id_;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        last_profile_ = candidates[i].profile_id;
        auto inner = factory_(candidates[i].token);
        if (!inner) continue;
        last = inner->Complete(messages, tools);
        if (last.ok) return last;
        // Rotate on auth / rate-limit failures when more profiles remain.
        if ((last.http_status == 401 || last.http_status == 429 || last.http_status == 402) && i + 1 < candidates.size())
            continue;
        return last;
    }
    return last;
}

}  // namespace openclaw::provider
