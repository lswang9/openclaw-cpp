#pragma once
// Upstream-style `exec` tool: host selection (sandbox|gateway), exec policy (security/ask/askFallback,
// tools.exec.mode), per-agent allowlists in an approvals document, strict inline-eval, safe bins,
// approval broker (exec.approval.requested / exec.approval.resolve), allow-always persistence.
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace openclaw::tools::exec {

struct AllowEntry {
    std::string id, pattern, arg_pattern, source, cwd;
};

struct Policy {
    std::string host = "gateway";      // effective: sandbox | gateway
    std::string security = "full";     // deny | allowlist | full
    std::string ask = "off";           // off | on-miss | always
    std::string ask_fallback = "deny";  // deny | allowlist | full
    bool strict_inline_eval = false;
    bool auto_allow_skills = false;
    std::vector<std::string> safe_bins;
    std::vector<AllowEntry> allowlist;
    std::string sandbox_backend;  // docker | bwrap | "" (unavailable)
    std::string sandbox_image = "debian:bookworm-slim";
    bool sandbox_network = false;
    int timeout_sec = 1800;
    int approval_timeout_ms = 120000;
    nlohmann::json sources = nlohmann::json::object();
};

// Legacy JSON path (OPENCLAW_EXEC_APPROVALS_PATH or …/exec-approvals.json). Prefer SqlitePath.
std::filesystem::path DefaultApprovalsPath();
// Upstream: $OPENCLAW_STATE_DIR/state/openclaw.sqlite (row key exec_approvals_config).
std::filesystem::path DefaultSqlitePath();
nlohmann::json LoadApprovals(const std::filesystem::path& p = {});
bool SaveApprovals(const std::filesystem::path& p, const nlohmann::json& doc);

// Stricter of config (tools.exec.* / agents.list[].tools.exec) and the approvals document.
// session_key decides sandbox for agents.defaults.sandbox.mode "non-main".
Policy ResolvePolicy(const nlohmann::json& cfg_raw, const nlohmann::json& approvals_doc, const std::string& agent_id,
                     const std::string& session_key, const std::string& requested_host = "");

struct Segment {
    std::vector<std::string> argv;
    std::string resolved;  // absolute executable path ("" if not found)
    bool via_path = false; // argv0 was a bare name resolved through PATH
};
struct ParsedCommand {
    std::vector<Segment> segments;
    bool complex = false;  // $(...), backticks, heredoc, redirection to subshells → never allowlisted
};
ParsedCommand Parse(const std::string& command, const std::string& cwd);
bool GlobMatch(const std::string& pattern, const std::string& text);  // *, **, ?; ~ expanded
bool IsInlineEval(const std::vector<std::string>& argv);
// All segments allowlisted (or safe bins). `cwd` binds allow-always entries.
bool AllowlistHit(const Policy& p, const ParsedCommand& pc, const std::string& cwd);
std::string FileSha256(const std::filesystem::path& path);
nlohmann::json SnapshotOperands(const ParsedCommand& pc, const std::filesystem::path& cwd = {});
std::string RevalidateOperands(const nlohmann::json& snap);

struct Decision {
    std::string verdict;  // allow | deny | ask
    std::string reason;
    bool allowlist_hit = false;
    bool inline_eval = false;
};
Decision Evaluate(const Policy& p, const ParsedCommand& pc, const std::string& cwd);

// Thread-safe approval broker shared by the exec tool and the gateway.
class ApprovalBroker {
public:
    static ApprovalBroker& Global();
    // Gateway hooks: is any approval UI connected; broadcast new requests.
    std::function<bool()> has_ui;
    std::function<void(const nlohmann::json&)> on_requested;
    std::function<void(const nlohmann::json&)> on_resolved;

    std::string Create(nlohmann::json request);
    // Blocks until resolved or timeout. Returns allow-once | allow-always | deny | timeout.
    std::string Wait(const std::string& id, std::chrono::milliseconds timeout);
    bool Resolve(const std::string& id, const std::string& decision);
    nlohmann::json Pending() const;
    nlohmann::json Get(const std::string& id) const;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, nlohmann::json> items_;
    int seq_ = 0;
};

// Per-run context (thread-local) set by the gateway around agent runs.
struct RunContext {
    std::string agent_id = "main";
    std::string session_key;
    std::string channel;   // originating chat channel ("" = API/WS)
    std::string chat_id;   // where to post approval prompts
    std::string sender;    // requester (default chat approver)
};
class ScopedRunContext {
public:
    explicit ScopedRunContext(RunContext c);
    ~ScopedRunContext();
private:
    RunContext prev_;
};
const RunContext& CurrentContext();

// Execute the tool call. args: command, workdir, env{}, timeout (sec), host, ask, security,
// yieldMs, background. cfg_raw: full config; workspace: agent workspace root.
nlohmann::json Run(const nlohmann::json& args, const nlohmann::json& cfg_raw, const std::filesystem::path& workspace);

// Companion `process` tool: list|poll|log|kill background exec sessions (per agent).
nlohmann::json ProcessTool(const nlohmann::json& args);

}  // namespace openclaw::tools::exec
