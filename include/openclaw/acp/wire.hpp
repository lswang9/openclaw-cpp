#pragma once

#include "openclaw/agent/loop.hpp"
#include "openclaw/agent/types.hpp"
#include "openclaw/provider/provider.hpp"
#include "openclaw/mcp/client.hpp"
#include "openclaw/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openclaw::acp {

// Agent-side ACP JSON-RPC (subset of Agent Client Protocol v2).
// session/prompt runs openclaw::agent::RunAgentLoop (default: mock provider).
//
// Implemented: initialize, session/new|list|prompt|cancel|close|resume, ping.
// Multimodal: text + image_base64 / image_url (+ optional file) on session/prompt.
// Session MCP: session/mcp/add|list|remove (mock/stdio); tools merged on prompt.
// Not implemented: auth/*, acpx adapters, audio/video.
// Channel bindings: see acp::Registry BindChannel / HandleChannelInbound.

constexpr int kAcpProtocolVersion = 2;

struct SessionMcpAttachment {
    std::string id;          // local attachment id (mcp_1, ...)
    std::string name;        // display / server name
    std::string transport;   // mock | stdio
    std::string prefix;      // tool name prefix, default mcp.
    std::vector<std::string> argv;  // stdio argv
    std::shared_ptr<mcp::Client> client;
    std::vector<std::string> tool_names;  // registered local names (prefix+remote)
};

struct WireSession {
    std::string id;
    std::string cwd;
    std::string status;  // idle | running
    std::vector<nlohmann::json> messages;  // ACP wire log {role, text, messageId}
    std::vector<agent::Message> history;   // agent-loop conversation
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    std::vector<SessionMcpAttachment> mcp_servers;
};

struct LoopBackend {
    std::shared_ptr<provider::Provider> provider;
    std::shared_ptr<tools::Registry> tools;
    agent::LoopOptions opts;
    std::string provider_name = "mock";
};

class AgentEndpoint {
public:
    explicit AgentEndpoint(std::string agent_name = "openclaw-cpp",
                           std::string agent_version = "1.19.0-dev");

    // Inject provider/tools (e.g. mock for tests, DeepSeek when configured).
    // If unset, EnsureDefaultBackend() installs MockProvider + empty tools.
    void SetLoopBackend(LoopBackend backend);
    const LoopBackend& GetLoopBackend() const { return backend_; }

    nlohmann::json Handle(const nlohmann::json& request);
    std::vector<nlohmann::json> DrainNotifications();

    nlohmann::json StatusJson() const;
    nlohmann::json ListSessionsJson() const;
    bool Initialized() const { return initialized_; }

    nlohmann::json NewSession(const nlohmann::json& params);
    nlohmann::json Prompt(const std::string& session_id, const nlohmann::json& prompt_blocks);

    // Attach MCP server tools to a wire session (merged into agent tools on prompt).
    nlohmann::json AddSessionMcp(const std::string& session_id, const nlohmann::json& spec);
    nlohmann::json ListSessionMcp(const std::string& session_id) const;
    nlohmann::json RemoveSessionMcp(const std::string& session_id, const std::string& mcp_id);

private:
    mutable std::mutex mu_;
    std::string name_;
    std::string version_;
    bool initialized_ = false;
    int client_protocol_ = 0;
    nlohmann::json client_info_ = nlohmann::json::object();
    std::vector<WireSession> sessions_;
    std::vector<nlohmann::json> pending_notifications_;
    int sess_seq_ = 1;
    int msg_seq_ = 1;
    LoopBackend backend_;
    bool backend_ready_ = false;

    void EnsureDefaultBackend();
    struct PromptParsed {
        std::string text;
        nlohmann::json openai_parts = nlohmann::json::array();  // provider wire
        nlohmann::json notify_parts = nlohmann::json::array();  // ACP session/update
        int images = 0;
        int files = 0;
    };

    // Runs loop; unlocks `lock` while looping so session/cancel can proceed.
    nlohmann::json RunPromptTurn(std::unique_lock<std::mutex>& lock, WireSession& session,
                                 const PromptParsed& parsed, const std::string& user_mid);

    nlohmann::json Ok(const nlohmann::json& id, nlohmann::json result);
    nlohmann::json Err(const nlohmann::json& id, int code, const std::string& message,
                       nlohmann::json data = nullptr);
    void Notify(const std::string& method, nlohmann::json params);
    WireSession* Find(const std::string& id);
    const WireSession* FindConst(const std::string& id) const;
    void AttachMcpServersFromParams(WireSession& session, const nlohmann::json& mcp_servers);
    nlohmann::json AddSessionMcpTo(WireSession& session, const nlohmann::json& spec);
    std::shared_ptr<tools::Registry> BuildTurnTools(const WireSession& session) const;
    int mcp_seq_ = 1;
    static std::string PromptText(const nlohmann::json& prompt_blocks);
    static PromptParsed ParsePrompt(const nlohmann::json& prompt_blocks);
};

using WriteLineFn = std::function<void(const std::string&)>;
using ReadLineFn = std::function<std::string()>;

class Client {
public:
    Client(WriteLineFn write, ReadLineFn read);

    nlohmann::json Request(const std::string& method, const nlohmann::json& params = {});
    bool Initialize(const nlohmann::json& client_info = {});
    nlohmann::json SessionNew(const std::string& cwd = ".",
                              const nlohmann::json& mcp_servers = nlohmann::json::array());
    nlohmann::json SessionList();
    nlohmann::json SessionPrompt(const std::string& session_id, const std::string& text);
    nlohmann::json SessionPromptParts(const std::string& session_id,
                                      const nlohmann::json& prompt_blocks);
    nlohmann::json SessionCancel(const std::string& session_id);
    nlohmann::json SessionClose(const std::string& session_id);

    static std::pair<std::shared_ptr<Client>, std::shared_ptr<AgentEndpoint>> MakeInProcessPair();

    const std::vector<nlohmann::json>& LastNotifications() const { return last_notifications_; }

private:
    WriteLineFn write_;
    ReadLineFn read_;
    int next_id_ = 1;
    std::vector<nlohmann::json> last_notifications_;
};

}  // namespace openclaw::acp
