#pragma once

#include "openclaw/channels/outbound.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace openclaw::channels {

// Meta WhatsApp Cloud API — webhook verify + inbound + outbound send.
// Not Baileys / multi-device. FORCE_MOCK + FeedPayload for CI.
class WhatsAppCloudAdapter : public OutboundRecordAdapter {
public:
    WhatsAppCloudAdapter();

    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;

    // Meta GET ?hub.mode=subscribe&hub.verify_token=&hub.challenge=
    nlohmann::json HandleVerifyGet(const std::string& verify_token, const std::string& challenge) const;

    // Offline feed (Cloud API webhook JSON shapes).
    nlohmann::json FeedPayload(const nlohmann::json& payload, const DeliverFn& deliver);
    nlohmann::json RunMockFrames(const std::vector<nlohmann::json>& frames, const DeliverFn& deliver);

private:
    std::uint64_t inbound_ = 0;
    std::uint64_t fed_ = 0;
    bool VerifyTokenValue(const std::string& got) const;
    static bool ExtractCloudMessage(const nlohmann::json& body, InboundMessage& out, std::string& note);
};

}  // namespace openclaw::channels
