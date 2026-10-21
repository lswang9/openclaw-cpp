#pragma once
// Real implementations of upstream v2026.10.5 RPCs that were stubs before 1.100:
//   sessions.companion.{ask,state,reset}   read-only side chat per session (in-memory, ≤24 exchanges)
//   sessions.{dispatch,move,reclaim}       worker placement ledger (device workers from the node registry)
//   claws.removalJournal                   agent-deletion journal (begin/rollback, digest + lease guarded)
//   projects.searchRemote                  GitHub repository search (exact, affiliated, global; 60 s cache)
//   sessions.github.{publish,confirm}      commit session workspace → push branch → open pull request
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "openclaw/gateway/extra.hpp"
#include "openclaw/gateway/server.hpp"

namespace openclaw::gateway {

// handled=false when the method is not one of the subsystem methods.
ExtraResult TrySubsystemDispatch(ServerDeps& deps, const std::filesystem::path& data_root,
                                 const std::string& method, const nlohmann::json& params);

// Shared with the local `claws removal-journal` CLI so both sides compute identical values.
std::string ClawDigest(const nlohmann::json& value);  // "sha256:<hex>" of key-sorted compact JSON
nlohmann::json ClawRemovalBinding(const std::filesystem::path& data_root);
nlohmann::json ClawStateIdentity(const std::filesystem::path& data_root);  // creates the state file if missing
nlohmann::json ClawStateRead(const std::filesystem::path& data_root);

std::filesystem::path SubsystemDataRoot(ServerDeps& deps);
// Requester identity for the current RPC (thread-local): principal + whether it has full operator write.
void SetRpcRequester(const std::string& principal, bool full_write);
std::string CurrentRpcPrincipal();
// chat.send / sessions.send record who prompted a session (co-author credit for GitHub publication).
void RecordSessionParticipant(const std::filesystem::path& data_root, const std::string& agent, const std::string& key);
// Lets in-process agent tools (github_publish) reach the subsystems; set by the Server constructor.
void SetSubsystemContext(ServerDeps* deps, const std::filesystem::path& data_root);
std::string AgentGitHubPublish(const nlohmann::json& args, const std::string& agent, const std::string& session_key);

// Runs the turn on the session's device worker (node host) when the session placement is active there:
// worker.launch.v1 → worker.status.v1 (long-poll) → receipt.resultJson; the transcript stays on the gateway.
// nullopt = not placed (run locally). Empty json + *err = placed but the worker run failed.
std::optional<nlohmann::json> RunTurnOnPlacedWorker(ServerDeps& d, const std::filesystem::path& data_root, const std::string& agent,
                                                    const std::string& key, const std::string& text, std::string* err);

// GitHub API base (gateway.github.apiBaseUrl / OPENCLAW_GITHUB_API_BASE_URL); https only unless loopback http.
std::string GitHubApiBase(const nlohmann::json& raw_config);

}  // namespace openclaw::gateway
