#pragma once

#include "openclaw/auth/gateway_auth.hpp"
#include "openclaw/agent/loop.hpp"
#include "openclaw/channels/registry.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/cron/scheduler.hpp"
#include "openclaw/plugins/registry.hpp"
#include "openclaw/provider/provider.hpp"
#include "openclaw/provider/deepseek.hpp"
#include "openclaw/session/store.hpp"
#include "openclaw/skills/loader.hpp"
#include "openclaw/tools/registry.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace openclaw::gateway {

struct ServerOptions {
    std::string bind_host = "127.0.0.1";
    std::uint16_t port = 18789;
    bool verbose = false;
};

struct ServerDeps {
    provider::Provider& provider;
    provider::DeepSeekProvider* deepseek = nullptr;  // for streaming when available
    tools::Registry& tools;
    session::Store& sessions;
    channels::Registry& channels;
    skills::Library& skills;
    plugins::Registry& plugins;
    cron::Scheduler& cron;
    auth::PairingStore& pairing;
    config::Snapshot config;
    auth::AuthConfig auth;
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

private:
    void HandleClient(int client_fd);
    void HandleHttp(int client_fd, const std::string& req);
    void HandleWebSocket(int client_fd, const std::string& req);
    void HandleChatCompletions(int client_fd, const nlohmann::json& body, bool stream);
    nlohmann::json DispatchRpc(const nlohmann::json& msg);
    nlohmann::json Stub(const std::string& method, const nlohmann::json& params);

    ServerOptions opts_;
    ServerDeps deps_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
};

}  // namespace openclaw::gateway
