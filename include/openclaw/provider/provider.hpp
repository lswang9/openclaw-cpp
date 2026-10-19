#pragma once

#include "openclaw/agent/types.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace openclaw::provider {

class Provider {
public:
    virtual ~Provider() = default;
    virtual agent::CompletionResult Complete(const std::vector<agent::Message>& messages,
                                             const nlohmann::json& tools) = 0;
};

}  // namespace openclaw::provider
