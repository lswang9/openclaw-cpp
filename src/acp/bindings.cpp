#include "openclaw/acp/bindings.hpp"

namespace openclaw::acp {

std::string BindingStore::MakeKey(const std::string& channel, const std::string& chat_id) {
    return channel + ":" + chat_id;
}

bool BindingStore::ParseInbound(const channels::InboundMessage& msg, std::string& channel,
                                std::string& chat_id, std::string& channel_key) {
    channel = msg.channel;
    chat_id.clear();
    if (channel.empty()) return false;
    const std::string& sk = msg.session_key;
    const std::string prefix_dash = channel + "-";
    const std::string prefix_colon = channel + ":";
    if (sk.rfind(prefix_dash, 0) == 0) chat_id = sk.substr(prefix_dash.size());
    else if (sk.rfind(prefix_colon, 0) == 0) chat_id = sk.substr(prefix_colon.size());
    else if (!sk.empty() && sk != channel + "-default") chat_id = sk;
    if (chat_id.empty()) chat_id = "default";
    channel_key = MakeKey(channel, chat_id);
    return true;
}

bool BindingStore::Bind(const ChannelBinding& b) {
    if (b.channel_key.empty() || b.acp_session_id.empty()) return false;
    std::lock_guard lock(mu_);
    map_[b.channel_key] = b;
    return true;
}

bool BindingStore::Unbind(const std::string& channel_key) {
    std::lock_guard lock(mu_);
    return map_.erase(channel_key) > 0;
}

std::optional<ChannelBinding> BindingStore::Find(const std::string& channel_key) const {
    std::lock_guard lock(mu_);
    auto it = map_.find(channel_key);
    if (it == map_.end()) return std::nullopt;
    return it->second;
}

std::vector<ChannelBinding> BindingStore::List() const {
    std::lock_guard lock(mu_);
    std::vector<ChannelBinding> out;
    out.reserve(map_.size());
    for (const auto& [_, b] : map_) out.push_back(b);
    return out;
}

nlohmann::json BindingStore::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& b : List()) {
        arr.push_back({{"channel_key", b.channel_key},
                       {"channel", b.channel},
                       {"chat_id", b.chat_id},
                       {"acp_session_id", b.acp_session_id},
                       {"wire_session_id", b.wire_session_id},
                       {"auto_created", b.auto_created}});
    }
    return arr;
}

nlohmann::json BindingStore::StatusJson() const {
    return {{"bindings", ListJson()}, {"count", List().size()}};
}

}  // namespace openclaw::acp
