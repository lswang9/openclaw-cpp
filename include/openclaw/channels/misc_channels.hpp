#pragma once
// 1.138: text transports for the 16 previously missing channel extensions
// (a2a, buzz, clickclack, irc, line, nextcloud-talk, nostr, raft, reef, sms,
//  synology-chat, tlon, twitch, x, zalo, zalouser). Webhook inbound + HTTP outbound;
// IRC native sockets and Nostr NIP-04 (when privateKey set) run in-process; relay I/O still uses the bridge URL.
#include <functional>
#include <utility>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "openclaw/channels/adapter.hpp"

namespace openclaw::channels {
bool IsMiscChannel(const std::string& id);
nlohmann::json MiscChannelSend(const std::string& id, const nlohmann::json& cfg, const std::string& chat_id,
                               const std::string& text, const nlohmann::json& extra);
using MiscSendFn = std::function<nlohmann::json(const std::string& chat_id, const std::string& text, const nlohmann::json& extra)>;
nlohmann::json MiscChannelInbound(const std::string& id, const nlohmann::json& cfg, const nlohmann::json& body,
                                  const DeliverFn& deliver, const MiscSendFn& send);
// Ids of the 16 channels (for registration / adapters).
const std::vector<std::pair<const char*, const char*>>& MiscChannelCatalog();
// X Account Activity CRC challenge (GET ?crc_token=). Returns {ok, response_token} or error.
nlohmann::json MiscChannelCrcGet(const std::string& id, const nlohmann::json& cfg, const std::string& crc_token);
}  // namespace openclaw::channels
