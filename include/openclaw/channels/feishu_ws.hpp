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
// (FEISHU_WS_URL or /callback/ws/endpoint). Binary pbbp2 frames (ping/pong, event, multi-part, ack) since 1.82.
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
    // Card action callback (type=card frame or card.action.trigger): returns card-response JSON (toast + optional card).
    nlohmann::json HandleCardAction(const nlohmann::json& body, const DeliverFn& deliver);
public:
    nlohmann::json CardActionForTest(const nlohmann::json& body, const DeliverFn& d) { return HandleCardAction(body, d); }
private:
    int reconnect_count_ = -1, reconnect_interval_s_ = 120, reconnect_nonce_s_ = 30;
    std::uint64_t reconnects_ = 0, card_callbacks_ = 0, gzip_frames_ = 0;
    nlohmann::json SendReply(const std::string& tenant_token, const std::string& chat_id, const std::string& text);
    int ping_interval_s_ = 120;
    std::uint64_t pings_ = 0, binary_events_ = 0, replies_sent_ = 0;

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
