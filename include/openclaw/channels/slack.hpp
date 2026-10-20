#pragma once
#include "openclaw/channels/adapter.hpp"
#include "openclaw/channels/slack_socket.hpp"
#include <cstdint>

namespace openclaw::channels {

// Slack Events API HTTP inbound + Socket Mode (WSS) client.
// Verify SLACK_VERIFY_TOKEN (or none in dev).
// url_verification → {challenge}; event_callback message → agent.
class SlackAdapter : public Adapter {
public:
    SlackAdapter();
    std::string Id() const override { return "slack"; }
    bool Enabled() const override { return enabled_; }
    void SetEnabled(bool on) override { enabled_ = on; }
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;

    void Start(DeliverFn deliver) override;
    void Stop() override;

    SlackSocketMode& Socket() { return socket_; }
    const SlackSocketMode& Socket() const { return socket_; }

private:
    bool enabled_ = true;
    std::uint64_t inbound_ = 0;
    bool Verify(const nlohmann::json& body) const;
    SlackSocketMode socket_;
};

}  // namespace openclaw::channels
