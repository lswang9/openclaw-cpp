#pragma once
#include "openclaw/channels/adapter.hpp"
#include <cstdint>
#include <string>

namespace openclaw::channels {

// Minimal HTTP inbound used by WhatsApp Cloud-style & iMessage bridge webhooks.
// Body: {text|message|body.text} + optional challenge/hub.challenge.
// Verify env: <PREFIX>_VERIFY_TOKEN (e.g. WHATSAPP_VERIFY_TOKEN, IMESSAGE_VERIFY_TOKEN).
class SimpleInboundAdapter : public Adapter {
public:
    SimpleInboundAdapter(std::string id, std::string verify_env, std::string note);
    std::string Id() const override { return id_; }
    bool Enabled() const override { return enabled_; }
    void SetEnabled(bool on) override { enabled_ = on; }
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;

private:
    std::string id_;
    std::string verify_env_;
    std::string note_;
    bool enabled_ = true;
    std::uint64_t inbound_ = 0;
    bool Verify(const nlohmann::json& body) const;
    std::string ExtractText(const nlohmann::json& body) const;
};

}  // namespace openclaw::channels
