#include "openclaw/provider/mock.hpp"

namespace openclaw::provider {

void MockProvider::Enqueue(agent::CompletionResult result) {
    script_.push_back(std::move(result));
}

agent::CompletionResult MockProvider::Complete(const std::vector<agent::Message>& messages,
                                               const nlohmann::json& /*tools*/) {
    last_messages_ = messages;
    if (script_.empty()) {
        agent::CompletionResult r;
        r.ok = true;
        r.finish_reason = "stop";
        r.message.role = "assistant";
        r.message.content = "mock-default";
        return r;
    }
    auto r = std::move(script_.front());
    script_.pop_front();
    return r;
}

}  // namespace openclaw::provider
