#pragma once
// 1.143: native IRC client (plain TCP or TLS) for channels.irc (server/nick/channels) and Twitch chat over IRC
// (channels.twitch.mode = "irc": irc.chat.twitch.tv, PASS oauth:<accessToken>). Inbound PRIVMSG → adapter inbound;
// outbound replies are written as PRIVMSG on the live connection.
#include "openclaw/channels/adapter.hpp"

#include <atomic>
#include <nlohmann/json.hpp>
#include <string>

namespace openclaw::channels {

// Blocks until stop; returns immediately when the channel is not configured for a native connection.
void RunIrcClient(const std::string& id, const nlohmann::json& cfg, const std::atomic<bool>& stop, Adapter* ad, const DeliverFn& deliver);

// Send PRIVMSG over a live native connection for `id`; false when none is connected.
bool IrcNativeSend(const std::string& id, const std::string& target, const std::string& text);

}  // namespace openclaw::channels
