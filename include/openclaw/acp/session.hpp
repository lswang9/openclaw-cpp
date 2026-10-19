#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace openclaw::acp {

struct Session {
    std::string id;
    std::string agent_id;  // e.g. "codex", "claude"
    std::string status;    // idle | running | error
    std::vector<std::string> prompts;
};

class Registry {
public:
    std::string Create(const std::string& agent_id);
    bool AppendPrompt(const std::string& id, const std::string& prompt);
    nlohmann::json ListJson() const;
    nlohmann::json StatusJson() const;
    nlohmann::json GetJson(const std::string& id) const;

private:
    std::vector<Session> sessions_;
    int seq_ = 1;
};

}  // namespace openclaw::acp
