#include "openclaw/context/assemble.hpp"

namespace openclaw::context {

namespace {

agent::Message TruncateTool(agent::Message m, std::size_t max_chars) {
    if (m.role == "tool" && m.content.size() > max_chars) {
        m.content = m.content.substr(0, max_chars) + "…[truncated]";
    }
    return m;
}

}  // namespace

std::vector<agent::Message> Assemble(const std::vector<agent::Message>& history,
                                     const AssembleOptions& opts) {
    std::vector<agent::Message> out;
    if (!opts.system_prompt.empty()) {
        out.push_back({.role = "system", .content = opts.system_prompt});
    }

    // Copy history, optionally stripping old tool payloads.
    std::vector<agent::Message> body = history;
    if (!body.empty() && body.front().role == "system" && !opts.system_prompt.empty()) {
        // drop duplicate system from history when we inject
        body.erase(body.begin());
    }

    const std::size_t n = body.size();
    const std::size_t keep_from = n > opts.keep_recent ? n - opts.keep_recent : 0;
    for (std::size_t i = 0; i < n; ++i) {
        agent::Message m = body[i];
        if (opts.strip_old_tool_results && i < keep_from && m.role == "tool") {
            m.content = "[pruned tool result]";
        } else {
            m = TruncateTool(std::move(m), opts.max_tool_result_chars);
        }
        out.push_back(std::move(m));
    }

    if (out.size() > opts.max_messages) {
        // preserve leading system if present
        std::size_t start = 0;
        std::vector<agent::Message> trimmed;
        if (!out.empty() && out.front().role == "system") {
            trimmed.push_back(out.front());
            start = 1;
        }
        const std::size_t need = opts.max_messages - trimmed.size();
        const std::size_t from = out.size() > need + start ? out.size() - need : start;
        for (std::size_t i = from; i < out.size(); ++i) trimmed.push_back(out[i]);
        out = std::move(trimmed);
    }
    return out;
}

std::vector<agent::Message> Compact(const std::vector<agent::Message>& history,
                                    std::size_t keep_recent) {
    if (history.size() <= keep_recent) return history;
    std::vector<agent::Message> out;
    if (!history.empty() && history.front().role == "system") {
        out.push_back(history.front());
    }
    const std::size_t from = history.size() - keep_recent;
    for (std::size_t i = from; i < history.size(); ++i) {
        if (!out.empty() && i == 0) continue;
        out.push_back(history[i]);
    }
    // Avoid duplicating system if it was inside the recent window
    if (out.size() >= 2 && out[0].role == "system" && out[1].role == "system") {
        out.erase(out.begin() + 1);
    }
    return out;
}

}  // namespace openclaw::context
