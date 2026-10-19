#pragma once
#include "openclaw/config/config.hpp"
#include <string>
#include <vector>

namespace openclaw::config {

struct ValidationIssue {
    std::string path;
    std::string message;
};

std::vector<ValidationIssue> ValidateLight(const Snapshot& snap);
nlohmann::json ValidateLightJson(const Snapshot& snap);

}  // namespace openclaw::config
