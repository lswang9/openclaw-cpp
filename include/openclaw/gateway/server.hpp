#pragma once

#include "openclaw/agent/loop.hpp"
#include "openclaw/provider/provider.hpp"
#include "openclaw/session/store.hpp"
#include "openclaw/tools/registry.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace openclaw::gateway {

struct ServerOptions {
    std::string bind_host = "127.0.0.1";
    std::uint16_t port = 18789;  // OpenClaw Gateway default
    bool verbose = false;
};

// Multiplex HTTP + WebSocket control plane (subset of OpenClaw 2026.10.5 Gateway).
// Routes (HTTP):
//   GET  /health
//   GET  /v1/models
//   GET  /v1/status
//   POST /v1/chat/completions   (OpenAI-compatible, runs agent loop when tools present)
//   POST /tools/invoke
//   GET  /                      (info JSON)
// WebSocket:
//   Upgrade → JSON-RPC-ish {type,method,params} echo/status (full protocol TBD)
class Server {
public:
    Server(ServerOptions opts,
           provider::Provider& provider,
           tools::Registry& registry,
           session::Store& sessions);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Blocking serve loop until Stop() from another thread / signal.
    void Run();
    void Stop();
    bool Running() const { return running_.load(); }
    std::uint16_t Port() const { return opts_.port; }

private:
    void HandleClient(int client_fd);
    void HandleHttp(int client_fd, const std::string& req);
    void HandleWebSocket(int client_fd, const std::string& req);

    ServerOptions opts_;
    provider::Provider& provider_;
    tools::Registry& registry_;
    session::Store& sessions_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
};

}  // namespace openclaw::gateway
