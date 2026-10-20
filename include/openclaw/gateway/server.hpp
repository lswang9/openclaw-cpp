#pragma once

#include "openclaw/agent/files.hpp"
#include "openclaw/acp/session.hpp"
#include "openclaw/voice/tts.hpp"
#include "openclaw/sim/backend.hpp"
#include "openclaw/mcp/client.hpp"
#include <memory>
#include "openclaw/approvals/store.hpp"
#include "openclaw/auth/gateway_auth.hpp"
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
    session::Store& sessions;
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
    std::shared_ptr<mcp::Client> mcp_client;
    config::Snapshot config;
    auth::AuthConfig auth;
    std::string system_prompt = "You are OpenClaw (C++ rewrite). Be concise and helpful.";
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
    void HandleWebSocket(int client_fd, const std::string& req);
    void HandleChatCompletions(int client_fd, const nlohmann::json& body, bool stream);
    nlohmann::json DispatchRpc(const nlohmann::json& msg);
    nlohmann::json Stub(const std::string& method, const nlohmann::json& params);
    std::string DeliverInbound(const channels::InboundMessage& msg);
    void Emit(const std::string& event, const nlohmann::json& payload);
    void PersistAll() const;

    ServerOptions opts_;
    ServerDeps deps_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_agents_{false};
    int listen_fd_ = -1;
    std::mutex ws_mu_;
    std::set<int> ws_clients_;
};

}  // namespace openclaw::gateway
