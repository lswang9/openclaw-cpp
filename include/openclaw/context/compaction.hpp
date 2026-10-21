#pragma once
#include "openclaw/agent/types.hpp"
#include "openclaw/provider/provider.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace openclaw::context {

std::size_t EstimateTokens(const std::vector<agent::Message>& msgs);  // ~chars/4 + per-message overhead

struct CompactionPolicy {
    std::size_t context_tokens = 64000;   // agents.defaults.contextTokens
    std::size_t reserve_tokens = 16000;   // agents.defaults.compaction.reserveTokens
    std::size_t keep_recent = 12;         // messages kept verbatim
    std::string mode = "default";         // default | off
};
CompactionPolicy CompactionFromConfig(const nlohmann::json& raw);
bool NeedsCompaction(const CompactionPolicy& p, const std::vector<agent::Message>& msgs);

struct CompactionResult {
    bool compacted = false;
    bool summarized = false;  // false => fell back to truncation
    std::string summary;
    std::size_t before_tokens = 0, after_tokens = 0, dropped = 0;
    std::vector<agent::Message> messages;
};

// Upstream-style compaction: summarize older turns with the model into one summary message,
// keep the recent tail (never starting on an orphan tool result). Falls back to truncation on error.
CompactionResult CompactWithSummary(provider::Provider& prov, const std::vector<agent::Message>& history,
                                    std::size_t keep_recent, const std::string& instructions = "");

bool ParseCompactCommand(const std::string& text, std::string& instructions);

inline constexpr const char* kSummaryPrefix = "[Summary of earlier conversation]\n";

}  // namespace openclaw::context
