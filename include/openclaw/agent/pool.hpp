#pragma once
// Per-agent isolation: sessions folder, tools (allow/deny/profile), workspace, memory index, sandbox.
#include "openclaw/agent/routing.hpp"
#include "openclaw/session/store.hpp"
#include "openclaw/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::agent {

struct ToolPolicy {
    std::string profile;  // minimal | coding | messaging | full | ""
    std::vector<std::string> allow;  // empty = all (then deny)
    std::vector<std::string> deny;
};

// Extend routing::AgentDef fields that drive isolation.
struct AgentIsolation {
    routing::AgentDef def;
    ToolPolicy tools;
    nlohmann::json sandbox = nlohmann::json::object();  // mode/backend/docker… (merged into exec policy)
    std::filesystem::path sessions_root;
    std::filesystem::path workspace_root;
    std::filesystem::path memory_index;
    std::filesystem::path agent_files_root;
};

std::filesystem::path AgentsRoot();
AgentIsolation ResolveIsolation(const nlohmann::json& raw, const std::string& agent_id);
// Profile → base allow set (empty allow = unrestricted before deny). Glob-ish: trailing * matches prefix.
bool ToolAllowed(const ToolPolicy& p, const std::string& name);
tools::Registry FilterTools(const tools::Registry& base, const ToolPolicy& p);

struct AgentRuntime {
    AgentIsolation iso;
    std::unique_ptr<session::Store> sessions;
    // shared_ptr so in-flight runs keep a live registry across SetConfig rebuilds
    std::shared_ptr<tools::Registry> tools;
};

// Lazily builds & caches per-agent session stores and tool registries.
class Pool {
public:
    explicit Pool(tools::Registry* base_tools, approvals::Store* approvals = nullptr);
    // Refresh isolation paths/policies after config reload (keeps live session stores).
    void SetConfig(nlohmann::json raw);
    // Get or create runtime for agent_id (default "main").
    AgentRuntime& Get(const std::string& agent_id);
    std::shared_ptr<tools::Registry> Tools(const std::string& agent_id);
    session::Store& Sessions(const std::string& agent_id);
    // Snapshot of known agents (ids).
    std::vector<std::string> Ids() const;

private:
    tools::Registry* base_ = nullptr;
    approvals::Store* approvals_ = nullptr;
    nlohmann::json raw_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<AgentRuntime>> runtimes_;
};

}  // namespace openclaw::agent
