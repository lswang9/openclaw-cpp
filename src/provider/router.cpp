#include "openclaw/provider/router.hpp"

namespace openclaw::provider {

void Router::Add(const std::string& name, std::shared_ptr<Provider> provider) {
    providers_[name] = std::move(provider);
    if (default_.empty()) default_ = name;
}

bool Router::Has(const std::string& name) const {
    return providers_.find(name) != providers_.end();
}

void Router::SetDefault(const std::string& name) {
    if (Has(name)) default_ = name;
}

std::vector<std::string> Router::Names() const {
    std::vector<std::string> n;
    for (const auto& [k, _] : providers_) n.push_back(k);
    return n;
}

Provider* Router::Resolve(const std::string& route) const {
    if (route.empty()) {
        auto it = providers_.find(default_);
        return it == providers_.end() ? nullptr : it->second.get();
    }
    auto slash = route.find('/');
    const std::string name = slash == std::string::npos ? route : route.substr(0, slash);
    auto it = providers_.find(name);
    if (it != providers_.end()) return it->second.get();
    it = providers_.find(default_);
    return it == providers_.end() ? nullptr : it->second.get();
}

agent::CompletionResult Router::Complete(const std::vector<agent::Message>& messages,
                                         const nlohmann::json& tools) {
    return CompleteRoute(default_, messages, tools);
}

agent::CompletionResult Router::CompleteRoute(const std::string& route,
                                              const std::vector<agent::Message>& messages,
                                              const nlohmann::json& tools) {
    auto* p = Resolve(route);
    if (!p) {
        agent::CompletionResult r;
        r.ok = false;
        r.error = "no provider for route: " + route;
        return r;
    }
    return p->Complete(messages, tools);
}

}  // namespace openclaw::provider
