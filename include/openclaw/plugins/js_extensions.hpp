#pragma once
// Channels and model providers registered by JS plugins (api.registerChannel / api.registerProvider).
#include "openclaw/channels/adapter.hpp"
#include "openclaw/plugins/js_host.hpp"
#include "openclaw/provider/provider.hpp"

#include <string>

namespace openclaw::plugins {

// HTTP inbound at /api/channels/<id>/inbound → plugin inbound.parse(body) → agent;
// replies go out via plugin outbound.sendText({to, text}).
class PluginChannelAdapter : public channels::Adapter {
public:
    PluginChannelAdapter(JsHost* host, std::string id, std::string label);
    std::string Id() const override { return id_; }
    bool Enabled() const override { return enabled_; }
    void SetEnabled(bool on) override { enabled_ = on; }
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const channels::DeliverFn& deliver) override;
    nlohmann::json SendText(const std::string& to, const std::string& text);

private:
    JsHost* host_;
    std::string id_, label_;
    bool enabled_ = true;
    std::uint64_t inbound_ = 0, sent_ = 0;
};

// Model provider backed by plugin complete({model, messages, tools}) → {content, tool_calls}.
class PluginProvider : public provider::Provider {
public:
    PluginProvider(JsHost* host, std::string id, std::string model);
    agent::CompletionResult Complete(const std::vector<agent::Message>& messages, const nlohmann::json& tools) override;

private:
    JsHost* host_;
    std::string id_, model_;
};

}  // namespace openclaw::plugins
