#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>

#include "openclaw/channels/registry.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace openclaw::channels {

// Delivers inbound channel messages into the agent/session pipeline.
using DeliverFn = std::function<std::string(const InboundMessage& msg)>;

class Adapter {
public:
    virtual ~Adapter() = default;
    virtual std::string Id() const = 0;
    virtual bool Enabled() const = 0;
    virtual void SetEnabled(bool on) = 0;
    virtual nlohmann::json Status() const = 0;
    // Optional start/stop for long-lived adapters (bots etc.)
    virtual void Start(DeliverFn deliver) { (void)deliver; }
    virtual void Stop() {}
    // HTTP inbound (POST /api/channels/<id>/inbound)
    virtual bool SupportsHttpInbound() const { return false; }
    virtual nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) {
        (void)body; (void)deliver;
        return {{"ok", false}, {"error", "inbound not supported"}};
    }
};

// HTTP webhook inbound: Gateway POST /api/channels/{id}/inbound
// Body: {"text":"...","sender":"...","session_key":"..."}
class WebhookAdapter : public Adapter {
public:
    explicit WebhookAdapter(std::string id = "webhook");
    std::string Id() const override { return id_; }
    bool Enabled() const override { return enabled_; }
    void SetEnabled(bool on) override { enabled_ = on; }
    nlohmann::json Status() const override;
    bool SupportsHttpInbound() const override { return true; }
    nlohmann::json HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) override;
    // Back-compat alias
    nlohmann::json HandleInbound(const nlohmann::json& body, const DeliverFn& deliver) {
        return HandleHttpInbound(body, deliver);
    }

private:
    std::string id_;
    bool enabled_ = true;
    std::uint64_t received_ = 0;
};

// Placeholder adapters for future IM integrations.
class StubAdapter : public Adapter {
public:
    StubAdapter(std::string id, std::string todo_note);
    std::string Id() const override { return id_; }
    bool Enabled() const override { return enabled_; }
    void SetEnabled(bool on) override { enabled_ = on; }
    nlohmann::json Status() const override;

private:
    std::string id_;
    std::string todo_;
    bool enabled_ = false;
};

class AdapterHub {
public:
    void Add(std::unique_ptr<Adapter> adapter);
    Adapter* Get(const std::string& id);
    bool Remove(const std::string& id);
    std::vector<Adapter*> List();
    nlohmann::json StatusJson() const;
    static AdapterHub MakeDefault();

private:
    std::vector<std::unique_ptr<Adapter>> adapters_;
};

}  // namespace openclaw::channels
