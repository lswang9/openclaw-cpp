#pragma once

#include "openclaw/provider/provider.hpp"

#include <string>

namespace openclaw::provider {

class DeepSeekProvider : public Provider {
public:
    DeepSeekProvider(std::string api_key, std::string base_url, std::string model);

    agent::CompletionResult Complete(const std::vector<agent::Message>& messages,
                                     const nlohmann::json& tools) override;

private:
    std::string api_key_;
    std::string base_url_;
    std::string model_;
};

}  // namespace openclaw::provider
