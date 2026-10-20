#pragma once

#include "openclaw/channels/registry.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::acp {

struct ChannelBinding {
    std::string channel_key;     // e.g. slack:C123
    std::string channel;         // slack | feishu | discord
    std::string chat_id;         // C123 / chat id
    std::string acp_session_id;  // gateway acp-* id
    std::string wire_session_id;
    bool auto_created = false;
};

// channel_key → ACP session. IM inbound can drive session/prompt.
class BindingStore {
public:
    static std::string MakeKey(const std::string& channel, const std::string& chat_id);
    static bool ParseInbound(const channels::InboundMessage& msg, std::string& channel,
                             std::string& chat_id, std::string& channel_key);

    bool Bind(const ChannelBinding& b);
    bool Unbind(const std::string& channel_key);
    std::optional<ChannelBinding> Find(const std::string& channel_key) const;
    std::vector<ChannelBinding> List() const;
    nlohmann::json ListJson() const;
    nlohmann::json StatusJson() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, ChannelBinding> map_;
};

}  // namespace openclaw::acp
