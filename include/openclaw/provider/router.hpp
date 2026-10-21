#pragma once

#include "openclaw/provider/provider.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::provider {

// Navos-like routing: select provider by name or "provider/model" ref.
class Router : public Provider {
public:
    void Add(const std::string& name, std::shared_ptr<Provider> provider);
    bool Has(const std::string& name) const;
    void SetDefault(const std::string& name);
    std::string Default() const { return default_; }
    std::vector<std::string> Names() const;
    // route like "deepseek" or "openai/gpt-4o" — uses provider before '/'
    Provider* Resolve(const std::string& route) const;

    agent::CompletionResult Complete(const std::vector<agent::Message>& messages,
                                     const nlohmann::json& tools) override;
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages,
                                              const nlohmann::json& tools, DeltaFn on_delta) override;

    // Complete with explicit route (does not change default).
    agent::CompletionResult CompleteRoute(const std::string& route,
                                          const std::vector<agent::Message>& messages,
                                          const nlohmann::json& tools);

private:
    std::unordered_map<std::string, std::shared_ptr<Provider>> providers_;
    std::string default_;
};

}  // namespace openclaw::provider
