#pragma once

#include "openclaw/agent/types.hpp"
#include "openclaw/provider/provider.hpp"
#include "openclaw/tools/registry.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace openclaw::agent {

struct LoopOptions {
    std::size_t max_turns = 8;
    bool verbose = false;
};

struct LoopResult {
    bool ok = false;
    std::string final_text;
    std::string error;
    std::vector<Message> messages;
    std::size_t turns = 0;
};

// Minimal OpenClaw-aligned agent loop:
// messages → model → optional tool_calls → tool results → model … → stop
LoopResult RunAgentLoop(provider::Provider& provider,
                        tools::Registry& registry,
                        std::vector<Message> messages,
                        LoopOptions opts = {});

}  // namespace openclaw::agent
