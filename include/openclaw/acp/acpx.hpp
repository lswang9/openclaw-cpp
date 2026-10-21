#pragma once
// acpx: run an external ACP agent (Codex, Claude Code, Gemini CLI, another openclaw-cpp, ...) as a subprocess
// speaking newline-delimited JSON-RPC 2.0 on stdio (Agent Client Protocol v1). openclaw-cpp is the *client*:
// initialize → session/new → session/prompt, streaming session/update notifications, and answering the agent's
// requests: session/request_permission (policy), fs/read_text_file + fs/write_text_file (confined to cwd).
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace openclaw::acp::acpx {

struct RunSpec {
    std::string agent;                  // id for presets/config (codex | claude | gemini | openclaw | custom)
    std::vector<std::string> argv;      // explicit command; overrides agent lookup
    std::string cwd;                    // session cwd (absolute); fs/* confined here
    nlohmann::json prompt = nlohmann::json::array();  // ACP ContentBlocks
    std::string permission = "deny";    // deny | allow_once | allow_always
    bool fs_write = false;
    int timeout_ms = 300000;
    std::vector<std::string> env;       // extra KEY=VALUE
    // Client "terminal" capability (terminal/create|output|wait_for_exit|kill|release). Off by default; commands
    // run with cwd confined to the session root, output capped by outputByteLimit (default 1 MiB).
    bool terminal = false;
    // Further turns sent as additional session/prompt calls in the same session (multi-turn).
    std::vector<nlohmann::json> more_turns;
    // Resume: session/load when the agent advertises loadSession, else error.
    std::string load_session_id;
    // Agents that require login: when session/new fails with auth_required (-32000) or authMethods are advertised
    // and this is set, call authenticate{methodId} then retry. "auto" picks the first advertised method.
    std::string auth_method;
};

// Resolve agent id → argv from config `acp.agents.<id>.command` (string or array) / `.args`, else built-in presets.
std::vector<std::string> ResolveCommand(const nlohmann::json& config, const std::string& agent, std::string* err = nullptr);
nlohmann::json Presets();

// Audio content: if the agent advertises promptCapabilities.audio the block is passed through, else it is
// transcribed locally (configured STT) and replaced with text. Applied inside Run after initialize.
nlohmann::json Run(const RunSpec& spec);
// RPC/tool entry: {agent, prompt (string | ContentBlocks), audio_path?, cwd?, permission?, fs_write?, timeout_ms?}.
// Only configured/preset agent ids are runnable (no raw argv from RPC/tool callers).
// trusted_cli: the local CLI may enable terminal per call; RPC/tool callers need acp.agents.<id>.allowTerminal=true.
nlohmann::json RunFromParams(const nlohmann::json& config, const nlohmann::json& params, bool trusted_cli = false);

}  // namespace openclaw::acp::acpx
