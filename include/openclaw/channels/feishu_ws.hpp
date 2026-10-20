#pragma once

#include "openclaw/channels/adapter.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openclaw::channels {

// Feishu/Lark long-connection (WebSocket) event inbound.
// Offline: FeedPayload / RunMockFrames. Live: app_id/secret → tenant token → WS
// (FEISHU_WS_URL or discovered endpoint). Proprietary SDK binary frames = gap.
class FeishuLongConn {
public:
    FeishuLongConn();
    ~FeishuLongConn();

    FeishuLongConn(const FeishuLongConn&) = delete;
    FeishuLongConn& operator=(const FeishuLongConn&) = delete;

    nlohmann::json Status() const;

    // Process one JSON frame (ping / event / webhook-shaped). Returns optional
    // frames to send (e.g. pong / ack).
    nlohmann::json FeedPayload(const nlohmann::json& payload, const DeliverFn& deliver);

    nlohmann::json RunMockFrames(const std::vector<nlohmann::json>& frames, const DeliverFn& deliver);

    bool Start(DeliverFn deliver);
    void Stop();
    bool Running() const { return running_.load(); }

private:
    nlohmann::json HandleEventBody(const nlohmann::json& body, const DeliverFn& deliver);
    bool FetchTenantToken(std::string& out_token, std::string& err);
    bool ResolveWsUrl(const std::string& tenant_token, std::string& out_url, std::string& err);
    void LiveLoop(DeliverFn deliver);

    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thr_;
    std::uint64_t payloads_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t pongs_ = 0;
    std::string last_error_;
    std::string mode_ = "idle";
    std::string tenant_token_hint_;  // set / unset only
};

}  // namespace openclaw::channels
