#pragma once
#include "openclaw/channels/outbound.hpp"

namespace openclaw::channels {

// Feishu/Lark: outbound (FEISHU_WEBHOOK_URL) + inbound webhook
// (challenge URL verification + message → agent). Verify via FEISHU_VERIFY_TOKEN.
class FeishuAdapter : public OutboundRecordAdapter {
public:
    FeishuAdapter();
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;

private:
    std::uint64_t inbound_ = 0;
    bool VerifyToken(const nlohmann::json& body) const;
    static std::string ExtractText(const nlohmann::json& body);
};

}  // namespace openclaw::channels
