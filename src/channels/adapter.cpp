#include "openclaw/channels/adapter.hpp"
#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/feishu.hpp"
#include "openclaw/channels/slack.hpp"
#include "openclaw/channels/discord.hpp"
#include "openclaw/channels/simple_inbound.hpp"
#include "openclaw/channels/whatsapp.hpp"

namespace openclaw::channels {

WebhookAdapter::WebhookAdapter(std::string id) : id_(std::move(id)) {}

nlohmann::json WebhookAdapter::Status() const {
    return {{"id", id_},
            {"enabled", enabled_},
            {"type", "webhook"},
            {"received", received_},
            {"inbound_path", "/api/channels/" + id_ + "/inbound"}};
}

nlohmann::json WebhookAdapter::HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) {
    if (!enabled_) {
        return {{"ok", false}, {"error", "adapter disabled"}};
    }
    InboundMessage msg;
    msg.channel = id_;
    msg.text = body.value("text", body.value("message", ""));
    msg.sender = body.value("sender", body.value("from", "webhook"));
    msg.session_key = body.value("session_key", body.value("sessionKey", id_ + "-default"));
    if (msg.text.empty()) {
        return {{"ok", false}, {"error", "missing text"}};
    }
    ++received_;
    std::string reply;
    if (deliver) {
        reply = deliver(msg);
    }
    return {{"ok", true},
            {"channel", msg.channel},
            {"session_key", msg.session_key},
            {"sender", msg.sender},
            {"reply", reply},
            {"received_total", received_}};
}

StubAdapter::StubAdapter(std::string id, std::string todo_note)
    : id_(std::move(id)), todo_(std::move(todo_note)) {}

nlohmann::json StubAdapter::Status() const {
    return {{"id", id_},
            {"enabled", enabled_},
            {"type", "stub"},
            {"todo", todo_}};
}

void AdapterHub::Add(std::unique_ptr<Adapter> adapter) {
    adapters_.push_back(std::move(adapter));
}

Adapter* AdapterHub::Get(const std::string& id) {
    for (auto& a : adapters_) {
        if (a->Id() == id) {
            return a.get();
        }
    }
    return nullptr;
}

std::vector<Adapter*> AdapterHub::List() {
    std::vector<Adapter*> out;
    for (auto& a : adapters_) {
        out.push_back(a.get());
    }
    return out;
}

nlohmann::json AdapterHub::StatusJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& a : adapters_) {
        arr.push_back(a->Status());
    }
    return arr;
}

AdapterHub AdapterHub::MakeDefault() {
    AdapterHub hub;
    hub.Add(std::make_unique<WebhookAdapter>("webhook"));
    hub.Add(std::make_unique<OutboundRecordAdapter>(
        "telegram",
        "TODO: live Telegram Bot API; outbound payloads recorded locally",
        "/bot<token>/sendMessage"));
    hub.Add(std::make_unique<DiscordAdapter>());
    hub.Add(std::make_unique<SlackAdapter>());
    hub.Add(std::make_unique<FeishuAdapter>());
    hub.Add(std::make_unique<WhatsAppCloudAdapter>());
    hub.Add(std::make_unique<SimpleInboundAdapter>(
        "imessage", "IMESSAGE_VERIFY_TOKEN",
        "iMessage bridge inbound webhook; outbound TODO"));
    return hub;
}

}  // namespace openclaw::channels
