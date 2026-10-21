#pragma once
#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/slack_socket.hpp"
#include <cstdint>

namespace openclaw::channels {

// Slack: Events API HTTP inbound + Socket Mode + outbound (SLACK_BOT_TOKEN).
class SlackAdapter : public OutboundRecordAdapter {
public:
    SlackAdapter();
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;

    void Start(DeliverFn deliver) override;
    void Stop() override;

    // Slack Web API call (views.open, chat.postMessage, ...). Live when SLACK_BOT_TOKEN set
    // (base overridable via SLACK_API_BASE for tests); always recorded in the outbound log.
    nlohmann::json CallApi(const std::string& method, const nlohmann::json& body);
    // Handle an interactive payload (block_actions / view_submission / shortcut).
    nlohmann::json HandleInteractive(const nlohmann::json& payload, const DeliverFn& deliver);

    SlackSocketMode& Socket() { return socket_; }
    const SlackSocketMode& Socket() const { return socket_; }

private:
    std::uint64_t inbound_ = 0;
    bool Verify(const nlohmann::json& body) const;
    SlackSocketMode socket_;
};

}  // namespace openclaw::channels
