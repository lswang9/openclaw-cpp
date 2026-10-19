#pragma once
#include "openclaw/channels/outbound.hpp"

namespace openclaw::channels {

// Discord: outbound (DISCORD_BOT_TOKEN) + inbound webhook/interactions.
// type==1 PING → {type:1}; message-like JSON → agent. Optional DISCORD_VERIFY_TOKEN.
class DiscordAdapter : public OutboundRecordAdapter {
public:
    DiscordAdapter();
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;

private:
    std::uint64_t inbound_ = 0;
    bool Verify(const nlohmann::json& body) const;
    static std::string ExtractText(const nlohmann::json& body);
};

}  // namespace openclaw::channels
