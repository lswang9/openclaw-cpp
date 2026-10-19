#pragma once

#include "openclaw/agent/types.hpp"
#include "openclaw/provider/provider.hpp"
#include "openclaw/tools/registry.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace openclaw::agent {

struct LoopOptions {
    std::size_t max_turns = 8;
    bool verbose = false;
    std::size_t identical_tool_warn_after = 2;
    std::size_t identical_tool_break_after = 4;
    std::string provider_route;
    // 0 = unlimited. Counts Complete() calls across loops sharing the same RateBucket.
    std::size_t rate_limit_per_minute = 0;
};

// Process-wide sliding window for gateway agent Completes.
class RateBucket {
public:
    static RateBucket& Global();
    // Returns false if over limit (and does not record). limit 0 = always allow.
    bool TryAcquire(std::size_t limit_per_minute);
    std::size_t RecentCount() const;

private:
    mutable std::mutex mu_;
    std::deque<std::chrono::steady_clock::time_point> hits_;
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
