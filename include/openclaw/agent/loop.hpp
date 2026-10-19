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
    // Soft-warn / break if the same tool+args repeats this many times consecutively.
    std::size_t identical_tool_warn_after = 2;
    std::size_t identical_tool_break_after = 4;
    std::string provider_route;  // optional router route
};

struct LoopResult {
    bool ok = false;
    std::string final_text;
    std::string error;
    std::vector<Message> messages;
    std::size_t turns = 0;
    std::vector<std::string> warnings;
};

LoopResult RunAgentLoop(provider::Provider& provider,
                        tools::Registry& registry,
                        std::vector<Message> messages,
                        LoopOptions opts = {});

}  // namespace openclaw::agent
