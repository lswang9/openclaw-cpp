#pragma once
#include <condition_variable>
#include <queue>
#include <map>
#include <functional>
#include <chrono>
#include <filesystem>

#include "openclaw/agent/files.hpp"
#include "openclaw/acp/session.hpp"
#include "openclaw/voice/tts.hpp"
#include "openclaw/sim/backend.hpp"
#include "openclaw/mcp/client.hpp"
#include <memory>
#include <optional>
#include "openclaw/approvals/store.hpp"
#include "openclaw/auth/gateway_auth.hpp"
#include "openclaw/auth/device_link.hpp"
#include "openclaw/agent/loop.hpp"
#include "openclaw/channels/adapter.hpp"
#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/registry.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/cron/scheduler.hpp"
#include "openclaw/events/bus.hpp"
#include "openclaw/logging/tail.hpp"
#include "openclaw/media/store.hpp"
#include "openclaw/memory/store.hpp"
#include "openclaw/nodes/registry.hpp"
#include "openclaw/persist/store.hpp"
#include "openclaw/plugins/registry.hpp"
#include "openclaw/plugins/js_host.hpp"
#include "openclaw/board/store.hpp"
#include "openclaw/canvas/store.hpp"
#include "openclaw/provider/deepseek.hpp"
#include "openclaw/provider/provider.hpp"
#include "openclaw/provider/router.hpp"
#include "openclaw/secrets/store.hpp"
#include "openclaw/session/store.hpp"
#include "openclaw/skills/loader.hpp"
#include "openclaw/tools/registry.hpp"
#include "openclaw/agent/pool.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace openclaw::gateway {

struct ServerOptions {
    std::string bind_host = "127.0.0.1";
    std::uint16_t port = 18789;
    bool verbose = false;
};

struct ServerDeps {
    provider::Provider& provider;
    provider::Router* router = nullptr;
    provider::DeepSeekProvider* deepseek = nullptr;
    tools::Registry& tools;
    session::Store& sessions;  // legacy/default (= main); prefer agent_pool when set
    agent::Pool* agent_pool = nullptr;
    channels::Registry& channels;
    channels::AdapterHub& adapters;
    skills::Library& skills;
    plugins::Registry& plugins;
    cron::Scheduler& cron;
    auth::PairingStore& pairing;
    approvals::Store& approvals;
    secrets::Store& secrets;
    memory::Store& memory;
    nodes::Registry& nodes;
    media::Store& media;
    events::Bus& bus;
    agent::FileStore& agent_files;
    logging::TailBuffer& logs;
    persist::StateStore* persist = nullptr;
    acp::Registry* acp = nullptr;
    voice::TtsEngine* tts = nullptr;
    sim::SimulationBackend* sim = nullptr;
    plugins::JsHost* js_host = nullptr;
    board::Store* board = nullptr;
    canvas::Store* canvas = nullptr;
    auth::DeviceLinkStore* device_links = nullptr;
    std::shared_ptr<mcp::Client> mcp_client;
    config::Snapshot config;
    auth::AuthConfig auth;
    std::string system_prompt = "You are OpenClaw (C++ rewrite). Be concise and helpful.";
    // Hot-reload hook: re-scan skills (applying skills.entries) and return the rebuilt system prompt.
    std::function<std::string()> rebuild_system_prompt;
    // Per-agent system prompt (agents.list[].workspace); null → deps.system_prompt.
    std::function<std::string(const std::string& agent_id, const std::filesystem::path& workspace)> system_prompt_for_agent;
};

class Server {
public:
    Server(ServerOptions opts, ServerDeps deps);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void Run();
    void Stop();
    bool Running() const { return running_.load(); }
    std::atomic<bool>& AgentCancelFlag() { return cancel_agents_; }

private:
    bool HandleClient(int client_fd);
    void HandleHttp(int client_fd, const std::string& req);
    std::optional<std::string> GatewayAuthDeny(const std::string& req) const;
    std::string AuthRole(const std::string& req) const;
    void HandleWebSocket(int client_fd, const std::string& req);
    void HandleChatCompletions(int client_fd, const nlohmann::json& body, bool stream);
    nlohmann::json DispatchRpc(const nlohmann::json& msg);
    nlohmann::json Stub(const std::string& method, const nlohmann::json& params);
    std::string DeliverInbound(const channels::InboundMessage& msg);
    // Native + skill + plugin chat commands as {name, description} (for Discord slash sync, /help).
    nlohmann::json CommandCatalog() const;
    void RefreshDiscordCommands();
    // Register plugin-provided channels (adapters) and providers (router routes).
    void RefreshPluginExtensions();
    void PluginSessionHook(const std::string& event, const std::string& key, const std::string& reason);
    void Emit(const std::string& event, const nlohmann::json& payload);
    void PersistAll() const;
    // Hot reload (gateway.reload.mode != off): re-read config when its mtime changes (checked ≤1/s).
    void MaybeReloadConfig();

public:
    // Force a reload check now (tests / config.reload RPC). Returns {reloaded, error, restartRequired}.
    nlohmann::json ReloadConfigNow();

private:
    std::map<std::string, std::string> agent_prompts_;
    std::mutex agent_prompts_mu_;
    std::string AgentSystemPrompt(const std::string& agent_id);
    session::Store& SessionsFor(const std::string& agent_id);
    std::shared_ptr<tools::Registry> ToolsFor(const std::string& agent_id);
    // Shared with request handlers; unique during ReloadConfigNow.
    std::unique_lock<std::mutex> LockDeps() const;
    std::vector<std::string> plugin_channels_;
    std::set<std::string> plugin_providers_;
    mutable std::mutex deps_mu_;  // exclusive: reload vs request handlers that touch deps
    std::mutex reload_mu_;
    // Bounded worker pool for HTTP clients (unbounded detach OOMed under reload hammers).
    std::mutex workers_mu_;
    std::condition_variable workers_cv_;
    std::queue<std::pair<int, std::string>> workers_q_;  // fd, client ip
    std::vector<std::thread> workers_;
    bool workers_stop_ = false;
    void StartWorkers(std::size_t n);
    void StopWorkers();
    void EnqueueClient(int fd, std::string ip);
    std::filesystem::file_time_type cfg_mtime_{};
    std::chrono::steady_clock::time_point last_reload_check_{};

    ServerOptions opts_;
    ServerDeps deps_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_agents_{false};
    int listen_fd_ = -1;
    std::mutex ws_mu_;
    std::set<int> ws_clients_;
};

}  // namespace openclaw::gateway
