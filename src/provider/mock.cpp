#include "openclaw/provider/mock.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace openclaw::provider {

void MockProvider::Enqueue(agent::CompletionResult result, int delay_ms) {
    script_.push_back(std::move(result));
    delays_.push_back(delay_ms);
}

agent::CompletionResult MockProvider::CompleteStreaming(const std::vector<agent::Message>& messages,
                                                        const nlohmann::json& tools, DeltaFn on_delta) {
    auto r = Complete(messages, tools);
    const int delay = next_delay_;
    std::vector<std::string> parts;
    const std::string& c = r.message.content;
    for (std::size_t i = 0; i < c.size();) {
        std::size_t j = c.find(' ', i);
        j = j == std::string::npos ? c.size() : j + 1;
        parts.push_back(c.substr(i, j - i));
        i = j;
    }
    if (parts.empty() && delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    for (auto& p : parts) {
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay / static_cast<int>(parts.size())));
        if (on_delta) on_delta(p);
    }
    return r;
}

agent::CompletionResult MockProvider::Complete(const std::vector<agent::Message>& messages,
                                               const nlohmann::json& /*tools*/) {
    last_messages_ = messages;
    if (script_.empty()) {
        agent::CompletionResult r;
        r.ok = true;
        r.finish_reason = "stop";
        r.message.role = "assistant";
        int images = 0;
        int files = 0;
        std::string last_text;
        if (!messages.empty()) {
            const auto& last = messages.back();
            images = last.ImagePartCount();
            last_text = last.content;
            if (last.HasMultimodal()) {
                for (const auto& p : last.multimodal_parts) {
                    if (!p.is_object()) continue;
                    auto t = p.value("type", "");
                    if (t == "file" || t == "input_file") ++files;
                }
            }
        }
        if (images > 0 || files > 0) {
            r.message.content = "mock-saw-image:" + std::to_string(images) +
                                (files ? (" file:" + std::to_string(files)) : "");
            if (!last_text.empty()) r.message.content += " text:" + last_text;
        } else if (const char* e = std::getenv("OPENCLAW_MOCK_ECHO"); e && *e == '1') {
            // 1.142 test aid: reveal what the model was actually sent (count, system prompt, last message).
            std::string sys; for (const auto& m : messages) if (m.role == "system") { sys = m.content; break; }
            if (sys.size() > 400) sys = sys.substr(sys.size() - 400);
            r.message.content = "mock-echo n=" + std::to_string(messages.size()) + " sys=[" + sys + "] last=[" + last_text + "]";
        } else {
            r.message.content = "mock-default";
        }
        return r;
    }
    auto r = std::move(script_.front());
    script_.pop_front();
    next_delay_ = delays_.empty() ? 0 : delays_.front();
    if (!delays_.empty()) delays_.pop_front();
    return r;
}

}  // namespace openclaw::provider
