#pragma once
// 1.129: Signal (signal-cli REST), Matrix (client-server API), Microsoft Teams (Bot Framework),
// Google Chat (HTTP app events) and Mattermost (REST + outgoing webhooks) channel transports.
#include <functional>
#include <string>
#include <nlohmann/json.hpp>
#include "openclaw/channels/adapter.hpp"

namespace openclaw::channels {
bool IsExtChannel(const std::string& id);
// Live send; returns null when the channel has no credentials configured (record-only).
nlohmann::json ExtChannelSend(const std::string& id, const nlohmann::json& cfg, const std::string& chat_id, const std::string& text,
                              const nlohmann::json& extra);
using ExtSendFn = std::function<nlohmann::json(const std::string& chat_id, const std::string& text, const nlohmann::json& extra)>;
nlohmann::json ExtChannelInbound(const std::string& id, const nlohmann::json& cfg, const nlohmann::json& body, const DeliverFn& deliver,
                                 const ExtSendFn& send);
}  // namespace openclaw::channels
