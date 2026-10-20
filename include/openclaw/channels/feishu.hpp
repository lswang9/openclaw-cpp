#pragma once
#include "openclaw/channels/feishu_ws.hpp"
#include "openclaw/channels/outbound.hpp"

namespace openclaw::channels {

// Feishu/Lark: outbound + HTTP webhook inbound + long-connection WS.
class FeishuAdapter : public OutboundRecordAdapter {
public:
    FeishuAdapter();
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;

    void Start(DeliverFn deliver) override;
    void Stop() override;

    FeishuLongConn& LongConn() { return longconn_; }
    const FeishuLongConn& LongConn() const { return longconn_; }

private:
    std::uint64_t inbound_ = 0;
    bool VerifyToken(const nlohmann::json& body) const;
    static std::string ExtractText(const nlohmann::json& body);
    FeishuLongConn longconn_;
};

}  // namespace openclaw::channels
