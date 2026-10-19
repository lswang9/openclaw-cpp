#pragma once

#include "openclaw/agent/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace openclaw::context {

struct AssembleOptions {
    std::string system_prompt;
    std::size_t max_messages = 64;       // hard cap after prune
    std::size_t keep_recent = 24;        // always keep last N
    std::size_t max_tool_result_chars = 4000;
    bool strip_old_tool_results = true;  // clear content of older tool msgs
};

// Build model-facing history: optional system + user/history with pruning
// aligned with OpenClaw session-pruning concepts (keep recent, shrink tools).
std::vector<agent::Message> Assemble(const std::vector<agent::Message>& history,
                                     const AssembleOptions& opts = {});

// Compact transcript for storage: keep system + last keep_recent messages.
std::vector<agent::Message> Compact(const std::vector<agent::Message>& history,
                                    std::size_t keep_recent = 24);

}  // namespace openclaw::context
