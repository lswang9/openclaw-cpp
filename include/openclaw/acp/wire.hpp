#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openclaw::acp {

// Agent-side ACP JSON-RPC (subset of Agent Client Protocol v2).
// Transport-agnostic: Handle() processes one request object and returns a response.
// Notifications produced while handling (e.g. session/update) are queued for the peer.
//
// Implemented: initialize, session/new, session/list, session/prompt, session/cancel,
//              session/close, session/resume (thin), ping.
// Not implemented: auth/*, full MCP-in-session, rich multimodal prompt, acpx adapters.

constexpr int kAcpProtocolVersion = 2;

struct WireSession {
    std::string id;
    std::string cwd;
    std::string status;  // idle | running
    std::vector<nlohmann::json> messages;  // {role, text, messageId}
};

class AgentEndpoint {
public:
    explicit AgentEndpoint(std::string agent_name = "openclaw-cpp",
                           std::string agent_version = "1.7.0-dev");

    // Process one JSON-RPC request; returns response object (always has jsonrpc).
    nlohmann::json Handle(const nlohmann::json& request);

    // Drain notifications generated since last drain (session/update, …).
    std::vector<nlohmann::json> DrainNotifications();

    nlohmann::json StatusJson() const;
    nlohmann::json ListSessionsJson() const;
    bool Initialized() const { return initialized_; }

    // For gateway / Registry bridge: create or prompt without full JSON-RPC envelope.
    nlohmann::json NewSession(const nlohmann::json& params);
    nlohmann::json Prompt(const std::string& session_id, const nlohmann::json& prompt_blocks);

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

    nlohmann::json Ok(const nlohmann::json& id, nlohmann::json result);
    nlohmann::json Err(const nlohmann::json& id, int code, const std::string& message,
                       nlohmann::json data = nullptr);
    void Notify(const std::string& method, nlohmann::json params);
    WireSession* Find(const std::string& id);
    static std::string PromptText(const nlohmann::json& prompt_blocks);
};

// Client-side helper over newline (or any) line transport.
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
    nlohmann::json SessionClose(const std::string& session_id);

    // In-process: pair with AgentEndpoint (no threads).
    static std::pair<std::shared_ptr<Client>, std::shared_ptr<AgentEndpoint>> MakeInProcessPair();

    const std::vector<nlohmann::json>& LastNotifications() const { return last_notifications_; }

private:
    WriteLineFn write_;
    ReadLineFn read_;
    int next_id_ = 1;
    std::vector<nlohmann::json> last_notifications_;
};

}  // namespace openclaw::acp
