#pragma once
#include <functional>

#include "openclaw/agent/types.hpp"
#include "openclaw/provider/provider.hpp"
#include "openclaw/tools/registry.hpp"

#include <atomic>
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
    std::size_t rate_limit_per_minute = 0;
    // Wall-clock deadline for the whole loop (0 = no timeout).
    std::chrono::milliseconds timeout{0};
    // Optional cooperative cancel flag (e.g. gateway shutdown).
    std::atomic<bool>* cancel = nullptr;
    // Plugin hooks. before_tool_call may return {block, blockReason, params}; after is fire-and-forget.
    std::function<nlohmann::json(const std::string& tool, const nlohmann::json& params)> before_tool_call;
    std::function<void(const std::string& tool, const nlohmann::json& params, const std::string& result,
                       long long duration_ms, const std::string& error)> after_tool_call;
};

class RateBucket {
public:
    static RateBucket& Global();
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
    bool cancelled = false;
    bool timed_out = false;
};

LoopResult RunAgentLoop(provider::Provider& provider,
                        tools::Registry& registry,
                        std::vector<Message> messages,
                        LoopOptions opts = {});

}  // namespace openclaw::agent
