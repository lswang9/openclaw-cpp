#pragma once
#include "openclaw/channels/discord_gateway.hpp"
#include "openclaw/channels/outbound.hpp"

namespace openclaw::channels {

// Discord: outbound (DISCORD_BOT_TOKEN) + inbound webhook/interactions + Gateway WS.
class DiscordAdapter : public OutboundRecordAdapter {
public:
    DiscordAdapter();
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;

    void Start(DeliverFn deliver) override;
    void Stop() override;

    DiscordGateway& Gateway() { return gateway_; }
    const DiscordGateway& Gateway() const { return gateway_; }

private:
    std::uint64_t inbound_ = 0;
    bool Verify(const nlohmann::json& body) const;
    static std::string ExtractText(const nlohmann::json& body);
    DiscordGateway gateway_;
};

}  // namespace openclaw::channels
