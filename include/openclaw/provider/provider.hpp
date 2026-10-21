#pragma once

#include "openclaw/agent/types.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace openclaw::provider {

class Provider {
public:
    virtual ~Provider() = default;
    virtual agent::CompletionResult Complete(const std::vector<agent::Message>& messages,
                                             const nlohmann::json& tools) = 0;
    // Token-level streaming. on_delta gets each content fragment as it arrives; the return value is the full
    // result (tool calls included). Default: non-streaming Complete, content delivered as one delta.
    using DeltaFn = std::function<void(std::string_view)>;
    virtual agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages,
                                                      const nlohmann::json& tools, DeltaFn on_delta) {
        auto r = Complete(messages, tools);
        if (r.ok && on_delta && !r.message.content.empty()) on_delta(r.message.content);
        return r;
    }
};

// OpenAI-compatible SSE chat completion (content + tool_call deltas + finish_reason).
agent::CompletionResult StreamChatCompletion(const std::string& url, const std::string& bearer,
                                             const nlohmann::json& body, const Provider::DeltaFn& on_delta,
                                             const std::string& label);

}  // namespace openclaw::provider
