#include "openclaw/channels/registry.hpp"

namespace openclaw::channels {

void Registry::Register(ChannelDescriptor desc) {
    channels_[desc.id] = std::move(desc);
}

std::vector<ChannelDescriptor> Registry::List() const {
    std::vector<ChannelDescriptor> out;
    out.reserve(channels_.size());
    for (const auto& [_, d] : channels_) {
        out.push_back(d);
    }
    return out;
}

nlohmann::json Registry::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : List()) {
        arr.push_back({{"id", d.id}, {"description", d.description}, {"enabled", d.enabled}});
    }
    return arr;
}

Registry MakeDefaultChannels() {
    Registry r;
    r.Register({"cli", "Local CLI chat entry (openclaw-cpp chat)", true});
    r.Register({"http", "Gateway HTTP /v1/chat/completions + /tools/invoke", true});
    r.Register({"webhook", "HTTP inbound POST /api/channels/webhook/inbound", true});
    r.Register({"websocket", "Gateway WebSocket control plane (partial RPC)", true});
    r.Register({"telegram", "Telegram Bot API (TODO)", false});
    r.Register({"discord", "Discord inbound+outbound", true});
    r.Register({"slack", "Slack Events API inbound", true});
    r.Register({"whatsapp", "WhatsApp (TODO)", false});
    r.Register({"feishu", "Feishu/Lark inbound+outbound", true});
    return r;
}

}  // namespace openclaw::channels
