#pragma once

#include "openclaw/channels/adapter.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace openclaw::channels {

// Discord Gateway (WSS) inbound — identify + MESSAGE_CREATE + heartbeats.
// Offline: FeedPayload / RunMockFrames. Live: DISCORD_BOT_TOKEN + curl WS.
class DiscordGateway {
public:
    DiscordGateway();
    ~DiscordGateway();

    DiscordGateway(const DiscordGateway&) = delete;
    DiscordGateway& operator=(const DiscordGateway&) = delete;

    nlohmann::json Status() const;

    // Process one decoded Gateway JSON payload. Returns optional frame(s) to send
    // (Identify after Hello, Heartbeat responses, etc.). Routes MESSAGE_CREATE via deliver.
    nlohmann::json FeedPayload(const nlohmann::json& payload, const DeliverFn& deliver);

    nlohmann::json RunMockFrames(const std::vector<nlohmann::json>& frames, const DeliverFn& deliver);

    bool Start(DeliverFn deliver);
    void Stop();
    bool Running() const { return running_.load(); }

private:
    nlohmann::json HandleDispatch(const std::string& event, const nlohmann::json& data,
                                  const DeliverFn& deliver);
    bool ResolveGatewayUrl(std::string& out_url, std::string& err);
    void LiveLoop(DeliverFn deliver);
    nlohmann::json MakeIdentify() const;
    nlohmann::json MakeHeartbeat() const;

    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thr_;
    std::uint64_t payloads_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t heartbeats_ = 0;
    std::optional<int> last_seq_;
    int heartbeat_interval_ms_ = 0;
    std::string session_id_;
    std::string last_error_;
    std::string mode_ = "idle";  // idle | mock | live | live-error
};

}  // namespace openclaw::channels
