#pragma once
// 1.134: the upstream core agent tools that were still missing: computer, dashboard, decision_evaluate, gateway,
// github_identity_status, mobile_ui, personal_instructions, plugins, portal, progress_card, screen,
// skill_workshop, terminal. Most are thin wrappers over gateway RPCs (as upstream); computer/mobile_ui forward to
// a connected node declaring computer.act / mobile.ui.* commands; skill_workshop edits versioned learned skills.
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace openclaw::provider { class Router; }

namespace openclaw::gateway {
std::vector<std::string> CoreGatewayToolNames();
bool CoreGatewayTool(const std::string& name, const nlohmann::json& args, const std::string& agent, const std::string& session_key,
                     const nlohmann::json& cfg, std::string* out);
void SetModelRouter(provider::Router* router);
// <state>/agents/<agent>/agent/workshop-skills (upstream resolveWorkshopSkillsDir).
std::string WorkshopSkillsDir(const std::string& agent);
}  // namespace openclaw::gateway
