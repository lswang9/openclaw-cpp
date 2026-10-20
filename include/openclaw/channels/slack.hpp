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

    SlackSocketMode& Socket() { return socket_; }
    const SlackSocketMode& Socket() const { return socket_; }

private:
    std::uint64_t inbound_ = 0;
    bool Verify(const nlohmann::json& body) const;
    SlackSocketMode socket_;
};

}  // namespace openclaw::channels
