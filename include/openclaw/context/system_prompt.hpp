#pragma once
#include "openclaw/skills/loader.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace openclaw::context {

struct SystemPromptOptions {
    std::string base;                         // identity/base instructions
    std::filesystem::path workspace;          // agent workspace (bootstrap files live here)
    std::vector<skills::Skill> skills;        // already gated
    std::vector<std::string> tool_names;      // advertised tools
    std::size_t bootstrap_max_chars = 20000;  // per-file cap (upstream agents.defaults.bootstrapMaxChars)
    bool include_bootstrap = true;
};

// Upstream bootstrap files injected into "Project Context", in order.
const std::vector<std::string>& BootstrapFileNames();

// Build the agent system prompt: base + tooling + skills block + workspace + bootstrap files.
std::string BuildSystemPrompt(const SystemPromptOptions& o);

// Report which bootstrap files were found / truncated (for `openclaw-cpp context`).
nlohmann::json DescribeBootstrap(const std::filesystem::path& workspace, std::size_t max_chars = 20000);

}  // namespace openclaw::context
