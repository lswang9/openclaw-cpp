#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace openclaw::agent {

// OpenAI-compatible chat message (aligned with OpenClaw provider wire format).
// Multimodal: when multimodal_parts is a non-empty array, ToJson emits content as
// an OpenAI vision-style array (text + image_url parts).
struct Message {
    std::string role;  // system | user | assistant | tool
    std::string content;
    std::optional<std::string> name;             // tool name when role=tool
    std::optional<std::string> tool_call_id;     // when role=tool
    nlohmann::json tool_calls = nlohmann::json::array();  // when assistant emits tools
    // OpenAI-style parts: [{type:text,text:...},{type:image_url,image_url:{url:...}}, ...]
    nlohmann::json multimodal_parts = nlohmann::json::array();

    bool HasMultimodal() const {
        return multimodal_parts.is_array() && !multimodal_parts.empty();
    }

    int ImagePartCount() const {
        if (!HasMultimodal()) return 0;
        int n = 0;
        for (const auto& p : multimodal_parts) {
            if (!p.is_object()) continue;
            auto t = p.value("type", "");
            if (t == "image_url" || t == "image" || t == "input_image") ++n;
        }
        return n;
    }

    nlohmann::json ToJson() const {
        nlohmann::json j{{"role", role}};
        if (HasMultimodal()) {
            j["content"] = multimodal_parts;
        } else {
            j["content"] = content;
        }
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
            } else if (j["content"].is_array()) {
                m.multimodal_parts = j["content"];
                // Flatten text for legacy consumers
                std::string flat;
                for (const auto& p : m.multimodal_parts) {
                    if (!p.is_object()) continue;
                    if (p.value("type", "") == "text" || p.contains("text")) {
                        if (!flat.empty()) flat += "\n";
                        flat += p.value("text", "");
                    }
                }
                m.content = flat;
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
