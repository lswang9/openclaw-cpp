#pragma once

#include "openclaw/agent/loop.hpp"
#include "openclaw/agent/types.hpp"
#include "openclaw/provider/provider.hpp"
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
// Not implemented: auth/*, multimodal, session MCP, acpx adapters, channel-bound ACP.

constexpr int kAcpProtocolVersion = 2;

struct WireSession {
    std::string id;
    std::string cwd;
    std::string status;  // idle | running
    std::vector<nlohmann::json> messages;  // ACP wire log {role, text, messageId}
    std::vector<agent::Message> history;   // agent-loop conversation
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
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
                           std::string agent_version = "1.8.0-dev");

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
    // Runs loop; unlocks `lock` while looping so session/cancel can proceed.
    nlohmann::json RunPromptTurn(std::unique_lock<std::mutex>& lock, WireSession& session,
                                 const std::string& text, const std::string& user_mid);

    nlohmann::json Ok(const nlohmann::json& id, nlohmann::json result);
    nlohmann::json Err(const nlohmann::json& id, int code, const std::string& message,
                       nlohmann::json data = nullptr);
    void Notify(const std::string& method, nlohmann::json params);
    WireSession* Find(const std::string& id);
    static std::string PromptText(const nlohmann::json& prompt_blocks);
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
