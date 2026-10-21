#pragma once

#include "openclaw/provider/provider.hpp"

#include <functional>
#include <string_view>
#include <string>

namespace openclaw::provider {

class DeepSeekProvider : public Provider {
public:
    DeepSeekProvider(std::string api_key, std::string base_url, std::string model);

    agent::CompletionResult Complete(const std::vector<agent::Message>& messages,
                                     const nlohmann::json& tools) override;

    // SSE streaming: calls on_delta for each content token; returns final aggregated result.
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages,
                                              const nlohmann::json& tools, DeltaFn on_delta) override {
        return CompleteStream(messages, tools, std::move(on_delta));
    }
    agent::CompletionResult CompleteStream(const std::vector<agent::Message>& messages,
                                           const nlohmann::json& tools, DeltaFn on_delta);

private:
    std::string api_key_;
    std::string base_url_;
    std::string model_;
};

}  // namespace openclaw::provider
