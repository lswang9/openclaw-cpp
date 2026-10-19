#include "openclaw/agent/loop.hpp"

#include <iostream>

namespace openclaw::agent {

LoopResult RunAgentLoop(provider::Provider& provider,
                        tools::Registry& registry,
                        std::vector<Message> messages,
                        LoopOptions opts) {
    LoopResult out;
    out.messages = std::move(messages);
    const auto tools = registry.OpenAiToolsArray();

    std::string last_sig;
    std::size_t same_count = 0;

    for (std::size_t turn = 0; turn < opts.max_turns; ++turn) {
        ++out.turns;
        if (opts.verbose) {
            std::cerr << "[agent] turn " << out.turns << " messages=" << out.messages.size()
                      << '\n';
        }

        auto completion = provider.Complete(out.messages, tools);
        if (!completion.ok) {
            out.ok = false;
            out.error = completion.error.empty() ? "provider failed" : completion.error;
            return out;
        }

        out.messages.push_back(completion.message);

        if (completion.tool_calls.empty() || completion.finish_reason == "stop") {
            out.ok = true;
            out.final_text = completion.message.content;
            return out;
        }

        for (const auto& tc : completion.tool_calls) {
            const std::string sig = tc.name + "|" + tc.arguments_json;
            if (sig == last_sig) {
                ++same_count;
            } else {
                last_sig = sig;
                same_count = 1;
            }

            if (same_count == opts.identical_tool_warn_after) {
                const std::string w =
                    "repeated identical tool call soft-warn: " + tc.name + " x" +
                    std::to_string(same_count);
                out.warnings.push_back(w);
                if (opts.verbose) std::cerr << "[agent] " << w << '\n';
            }
            if (same_count >= opts.identical_tool_break_after) {
                out.ok = false;
                out.error = "loop detection: identical tool call repeated " +
                            std::to_string(same_count) + " times (" + tc.name + ")";
                out.final_text = completion.message.content;
                return out;
            }

            nlohmann::json args = nlohmann::json::object();
            try {
                if (!tc.arguments_json.empty()) args = nlohmann::json::parse(tc.arguments_json);
            } catch (const std::exception& e) {
                args = {{"_parse_error", e.what()}, {"_raw", tc.arguments_json}};
            }

            if (opts.verbose) {
                std::cerr << "[agent] tool " << tc.name << " id=" << tc.id << '\n';
            }

            const std::string result = registry.Invoke(tc.name, args);
            Message tool_msg;
            tool_msg.role = "tool";
            tool_msg.tool_call_id = tc.id;
            tool_msg.name = tc.name;
            tool_msg.content = result;
            out.messages.push_back(std::move(tool_msg));
        }
    }

    out.ok = false;
    out.error = "max turns exceeded (" + std::to_string(opts.max_turns) + ")";
    if (!out.messages.empty() && out.messages.back().role == "assistant") {
        out.final_text = out.messages.back().content;
    }
    return out;
}

}  // namespace openclaw::agent
