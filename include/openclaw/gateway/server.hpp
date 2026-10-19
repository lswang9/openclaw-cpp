#pragma once

#include "openclaw/agent/loop.hpp"
#include "openclaw/channels/registry.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/provider/provider.hpp"
#include "openclaw/session/store.hpp"
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

class Server {
public:
    Server(ServerOptions opts,
           provider::Provider& provider,
           tools::Registry& registry,
           session::Store& sessions,
           channels::Registry& channels,
           config::Snapshot config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void Run();
    void Stop();
    bool Running() const { return running_.load(); }
    std::uint16_t Port() const { return opts_.port; }

private:
    void HandleClient(int client_fd);
    void HandleHttp(int client_fd, const std::string& req);
    void HandleWebSocket(int client_fd, const std::string& req);
    nlohmann::json DispatchRpc(const nlohmann::json& msg);

    ServerOptions opts_;
    provider::Provider& provider_;
    tools::Registry& registry_;
    session::Store& sessions_;
    channels::Registry& channels_;
    config::Snapshot config_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
};

}  // namespace openclaw::gateway
