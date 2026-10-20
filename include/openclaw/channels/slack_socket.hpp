#pragma once

#include "openclaw/channels/adapter.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openclaw::channels {

// Slack Socket Mode (apps.connections.open + WSS envelopes).
// Offline: FeedEnvelope / RunMockFrames — no network.
// Live: Start() when SLACK_APP_TOKEN (xapp-...) present and not FORCE_MOCK.
class SlackSocketMode {
public:
    SlackSocketMode();
    ~SlackSocketMode();

    SlackSocketMode(const SlackSocketMode&) = delete;
    SlackSocketMode& operator=(const SlackSocketMode&) = delete;

    nlohmann::json Status() const;

    // Process one decoded Socket Mode JSON text frame. Returns ack object to send
    // (empty object if no ack needed). Routes events_api / slash_commands via deliver.
    nlohmann::json FeedEnvelope(const nlohmann::json& envelope, const DeliverFn& deliver);

    // Run a recorded session (hello + envelopes). Always offline; never opens sockets.
    nlohmann::json RunMockFrames(const std::vector<nlohmann::json>& frames, const DeliverFn& deliver);

    // Live WSS loop (background). No-op / returns false without tokens or when FORCE_MOCK.
    bool Start(DeliverFn deliver);
    void Stop();

    bool Running() const { return running_.load(); }

private:
    nlohmann::json HandleEventsApi(const nlohmann::json& payload, const DeliverFn& deliver);
    nlohmann::json HandleSlash(const nlohmann::json& payload, const DeliverFn& deliver);
    bool OpenConnectionUrl(std::string& out_url, std::string& err);
    void LiveLoop(DeliverFn deliver);

    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thr_;
    std::uint64_t envelopes_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t acked_ = 0;
    std::string last_error_;
    std::string mode_ = "idle";  // idle | mock | live | live-error
};

}  // namespace openclaw::channels
