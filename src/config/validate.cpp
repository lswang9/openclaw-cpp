#include "openclaw/config/validate.hpp"

namespace openclaw::config {

std::vector<ValidationIssue> ValidateLight(const Snapshot& snap) {
    std::vector<ValidationIssue> issues;
    if (snap.gateway_port == 0) {
        issues.push_back({"gateway.port", "port must be non-zero"});
    }
    if (snap.gateway_host.empty()) {
        issues.push_back({"gateway.host", "host must not be empty"});
    }
    if (snap.deepseek_model.empty()) {
        issues.push_back({"agents.defaults.model", "model must not be empty"});
    }
    if (snap.session_dir.empty()) {
        issues.push_back({"session_dir", "session_dir must not be empty"});
    }
    return issues;
}

nlohmann::json ValidateLightJson(const Snapshot& snap) {
    auto issues = ValidateLight(snap);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& i : issues) arr.push_back({{"path", i.path}, {"message", i.message}});
    return {{"ok", issues.empty()}, {"issues", arr}};
}

}  // namespace openclaw::config
