#pragma once
// Multi-agent routing (upstream agents.list + bindings).
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace openclaw::agent::routing {

struct AgentDef {
    std::string id;
    std::string name;
    std::filesystem::path workspace;  // empty = default workspace
    std::string model;                // "provider/model" or ""
    std::string system_prompt;        // optional extra identity/prompt
    bool is_default = false;
};

// agents.list (falls back to a single "main" agent). Ids lowercased.
std::vector<AgentDef> ListAgents(const nlohmann::json& raw);
// Default agent id: first with default:true, else first listed, else "main".
std::string DefaultAgentId(const nlohmann::json& raw);
const AgentDef* FindAgent(const std::vector<AgentDef>& agents, const std::string& id);

struct RouteInput {
    std::string channel;
    std::string account_id;
    std::string peer_kind;  // direct | group | channel
    std::string peer_id;    // DM sender or group/channel id
    std::string guild_id;   // Discord
    std::string team_id;    // Slack
};

struct RouteResult {
    std::string agent_id;
    std::string matched_by;  // binding.peer | binding.guild | binding.team | binding.account | binding.channel | default
    int binding_index = -1;
};

// Most-specific binding wins: peer > guildId > teamId > accountId > channel-only; ties → first listed.
// Bindings whose agentId is not in agents.list are ignored.
RouteResult ResolveRoute(const nlohmann::json& raw, const RouteInput& in);

// "agent:<id>:..." → <id> ("" if not an agent key).
std::string AgentFromSessionKey(const std::string& key);

}  // namespace openclaw::agent::routing
