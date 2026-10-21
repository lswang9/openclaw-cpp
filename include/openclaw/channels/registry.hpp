#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::channels {

// Inbound message from a channel (WhatsApp/Telegram/… later).
struct InboundMessage {
    std::string channel;  // e.g. "cli", "http", "telegram"
    std::string sender;
    std::string text;
    std::string session_key;
    std::string chat_type;  // direct | group | channel ("" = infer from session_key)
    std::string peer_id;          // DM peer (defaults to sender)
    std::string conversation_id;  // group/channel id
    std::string thread_id;        // thread / topic id (appended as :thread:<id>)
    bool explicit_key = false;    // caller supplied session_key verbatim → never canonicalised
};

using InboundHandler = std::function<void(const InboundMessage&)>;

struct ChannelDescriptor {
    std::string id;
    std::string description;
    bool enabled = true;
};

// Channel plugin registry — stub toward OpenClaw channels/*.
class Registry {
public:
    void Register(ChannelDescriptor desc);
    std::vector<ChannelDescriptor> List() const;
    nlohmann::json ListJson() const;

private:
    std::unordered_map<std::string, ChannelDescriptor> channels_;
};

Registry MakeDefaultChannels();

}  // namespace openclaw::channels
