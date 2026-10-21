#pragma once

#include "openclaw/provider/provider.hpp"

#include <deque>
#include <string>

namespace openclaw::provider {

// Scripted provider for offline agent-loop tests (no network).
class MockProvider : public Provider {
public:
    void Enqueue(agent::CompletionResult result, int delay_ms = 0);
    // Streams content word by word (delay spread across fragments) — offline test double for SSE.
    agent::CompletionResult CompleteStreaming(const std::vector<agent::Message>& messages,
                                              const nlohmann::json& tools, DeltaFn on_delta) override;
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages,
                                     const nlohmann::json& tools) override;
    std::size_t Remaining() const { return script_.size(); }
    const std::vector<agent::Message>& LastMessages() const { return last_messages_; }

private:
    std::deque<agent::CompletionResult> script_;
    std::deque<int> delays_;
    int next_delay_ = 0;
    std::vector<agent::Message> last_messages_;
};

}  // namespace openclaw::provider
