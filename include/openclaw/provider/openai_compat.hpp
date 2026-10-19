#pragma once

#include "openclaw/provider/provider.hpp"

#include <string>

namespace openclaw::provider {

// Generic OpenAI-compatible /v1/chat/completions client (DeepSeek, OpenAI, local, …).
class OpenAiCompatProvider : public Provider {
public:
    OpenAiCompatProvider(std::string name, std::string api_key, std::string base_url,
                         std::string model);

    const std::string& Name() const { return name_; }
    const std::string& Model() const { return model_; }
    void SetModel(std::string model) { model_ = std::move(model); }

    agent::CompletionResult Complete(const std::vector<agent::Message>& messages,
                                     const nlohmann::json& tools) override;

private:
    std::string name_;
    std::string api_key_;
    std::string base_url_;
    std::string model_;
};

}  // namespace openclaw::provider
