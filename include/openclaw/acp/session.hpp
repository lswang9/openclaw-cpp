#pragma once

#include "openclaw/acp/wire.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace openclaw::acp {

// Legacy lightweight session row (gateway acp.sessions.*).
struct Session {
    std::string id;
    std::string agent_id;
    std::string status;
    std::vector<std::string> prompts;
};

// Registry bridges gateway RPCs to the in-process ACP AgentEndpoint (JSON-RPC wire).
class Registry {
public:
    Registry();

    std::string Create(const std::string& agent_id);
    bool AppendPrompt(const std::string& id, const std::string& prompt);
    nlohmann::json ListJson() const;
    nlohmann::json StatusJson() const;
    nlohmann::json GetJson(const std::string& id) const;

    // Wire-facing helpers
    AgentEndpoint& Endpoint() { return *endpoint_; }
    const AgentEndpoint& Endpoint() const { return *endpoint_; }
    nlohmann::json WireHandle(const nlohmann::json& request);
    nlohmann::json AgentsListJson() const;

private:
    std::vector<Session> sessions_;
    int seq_ = 1;
    std::shared_ptr<AgentEndpoint> endpoint_;
};

}  // namespace openclaw::acp
