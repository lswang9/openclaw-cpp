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
        } else {
            r.message.content = "mock-default";
        }
        return r;
    }
    auto r = std::move(script_.front());
    script_.pop_front();
    return r;
}

}  // namespace openclaw::provider
