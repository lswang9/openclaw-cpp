#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace openclaw::agent {

// OpenAI-compatible chat message (aligned with OpenClaw provider wire format).
struct Message {
    std::string role;  // system | user | assistant | tool
    std::string content;
    std::optional<std::string> name;             // tool name when role=tool
    std::optional<std::string> tool_call_id;     // when role=tool
    nlohmann::json tool_calls = nlohmann::json::array();  // when assistant emits tools

    nlohmann::json ToJson() const {
        nlohmann::json j{{"role", role}, {"content", content}};
        if (name) {
            j["name"] = *name;
        }
        if (tool_call_id) {
            j["tool_call_id"] = *tool_call_id;
        }
        if (!tool_calls.empty()) {
            j["tool_calls"] = tool_calls;
        }
        return j;
    }

    static Message FromJson(const nlohmann::json& j) {
        Message m;
        m.role = j.value("role", "");
        if (j.contains("content") && !j["content"].is_null()) {
            if (j["content"].is_string()) {
                m.content = j["content"].get<std::string>();
            } else {
                m.content = j["content"].dump();
            }
        }
        if (j.contains("name") && j["name"].is_string()) {
            m.name = j["name"].get<std::string>();
        }
        if (j.contains("tool_call_id") && j["tool_call_id"].is_string()) {
            m.tool_call_id = j["tool_call_id"].get<std::string>();
        }
        if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
            m.tool_calls = j["tool_calls"];
        }
        return m;
    }
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;  // raw JSON object string
};

struct CompletionResult {
    Message message;
    std::string finish_reason;  // stop | tool_calls | length | error
    std::vector<ToolCall> tool_calls;
    bool ok = true;
    std::string error;       // human-readable
    int http_status = 0;     // 0 if local / mock
};

}  // namespace openclaw::agent
