#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace openclaw::cli {

// Upstream v2026.10.5 local (non-gateway) commands: completion, setup (baseline), onboard, configure, migrate,
// tui (line-mode), crestodian (deprecated alias of setup). Prompts read lines from stdin, so every flow is
// scriptable (`printf 'answers\n' | openclaw-cpp onboard`). Returns -1 when argv[1] is not one of them, or
// kDelegateChat when the caller should run the chat REPL / one-shot instead (setup with an existing config).
constexpr int kDelegateChat = -2;
int MaybeLocalCommand(int argc, char** argv);
// Batch 3 (local_commands3.cpp): directory, proxy, database preflight/ownership, update wizard/cleanup/recover/...
int MaybeLocalCommand3(int argc, char** argv);

// Upstream command tree (generated from v2026.10.5 commander registrations): {cmd: {sub: {flags, args}}}.
const nlohmann::json& UpstreamCommandTree();
// Shell completion script for bash|zsh|fish|powershell over our native commands + the upstream tree.
std::string CompletionScript(const std::string& shell, const std::string& bin);

}  // namespace openclaw::cli
