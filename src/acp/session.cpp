#include "openclaw/acp/session.hpp"

namespace openclaw::acp {

std::string Registry::Create(const std::string& agent_id) {
    Session s;
    s.id = "acp-" + std::to_string(seq_++);
    s.agent_id = agent_id.empty() ? "openclaw" : agent_id;
    s.status = "idle";
    sessions_.push_back(s);
    return s.id;
}

bool Registry::AppendPrompt(const std::string& id, const std::string& prompt) {
    for (auto& s : sessions_) {
        if (s.id == id) {
            s.prompts.push_back(prompt);
            s.status = "idle";  // no live ACP runtime yet
            return true;
        }
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
    return {{"implemented", false},
            {"skeleton", true},
            {"sessions", ListJson()},
            {"gap_doc", "docs/ACP-GAP.md"},
            {"note", "ACP wire protocol / acpx adapters not implemented in openclaw-cpp yet"}};
}

}  // namespace openclaw::acp
