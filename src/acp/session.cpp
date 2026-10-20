#include "openclaw/acp/session.hpp"

namespace openclaw::acp {

Registry::Registry() : endpoint_(std::make_shared<AgentEndpoint>()) {}

std::string Registry::Create(const std::string& agent_id) {
    Session s;
    s.id = "acp-" + std::to_string(seq_++);
    s.agent_id = agent_id.empty() ? "openclaw" : agent_id;
    s.status = "idle";
    sessions_.push_back(s);
    // Also open a wire session for ACP protocol consumers
    auto wr = endpoint_->NewSession({{"cwd", "."}});
    // stash mapping in prompts metadata via status field note — keep simple: store wire id as first prompt tag
    if (wr.contains("sessionId")) {
        s.prompts.push_back(std::string("wire:") + wr["sessionId"].get<std::string>());
        sessions_.back().prompts = s.prompts;
    }
    return s.id;
}

bool Registry::AppendPrompt(const std::string& id, const std::string& prompt) {
    for (auto& s : sessions_) {
        if (s.id != id) continue;
        s.prompts.push_back(prompt);
        // Prefer wire session if we recorded wire:sess_*
        std::string wire_id;
        for (const auto& p : s.prompts) {
            if (p.rfind("wire:", 0) == 0) wire_id = p.substr(5);
        }
        if (!wire_id.empty()) {
            auto blocks = nlohmann::json::array({{{"type", "text"}, {"text", prompt}}});
            auto r = endpoint_->Prompt(wire_id, blocks);
            s.status = r.contains("error") ? "error" : "idle";
        } else {
            s.status = "idle";
        }
        return true;
    }
    return false;
}

nlohmann::json Registry::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : sessions_) {
        arr.push_back({{"id", s.id},
                       {"agentId", s.agent_id},
                       {"status", s.status},
                       {"prompts", s.prompts.size()}});
    }
    return arr;
}

nlohmann::json Registry::GetJson(const std::string& id) const {
    for (const auto& s : sessions_) {
        if (s.id == id) {
            return {{"id", s.id},
                    {"agentId", s.agent_id},
                    {"status", s.status},
                    {"prompts", s.prompts}};
        }
    }
    return {{"error", "not found"}, {"id", id}};
}

nlohmann::json Registry::StatusJson() const {
    auto st = endpoint_->StatusJson();
    st["skeleton"] = false;
    st["implemented"] = true;
    st["gateway_sessions"] = ListJson();
    st["wire_sessions"] = endpoint_->ListSessionsJson();
    return st;
}

nlohmann::json Registry::WireHandle(const nlohmann::json& request) {
    return endpoint_->Handle(request);
}

nlohmann::json Registry::AgentsListJson() const {
    return nlohmann::json::array({
        {{"id", "openclaw-cpp"},
         {"name", "openclaw-cpp"},
         {"title", "openclaw-cpp ACP agent"},
         {"protocolVersion", kAcpProtocolVersion},
         {"transport", nlohmann::json::array({"in-process", "stdio"})},
         {"impl", "real"},
         {"note", "built-in agent — session/prompt → RunAgentLoop (default mock); not acpx"}},
    });
}

}  // namespace openclaw::acp
