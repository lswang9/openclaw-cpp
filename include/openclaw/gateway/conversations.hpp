#pragma once
// Conversations (exact external channel addresses), gateway questions (ask_user) and the
// web_search / view_image / agents_wait agent tools (1.128).
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace openclaw::gateway {
using ConvSendFn = std::function<nlohmann::json(const std::string& channel, const std::string& to, const std::string& text, const nlohmann::json& extra)>;
void ConvSetRoot(const std::string& data_root);
void ConvSetSender(ConvSendFn fn);
// Record (or refresh) the external address behind an inbound message; returns its conversationRef.
std::string ConvRecord(const nlohmann::json& addr);
// Offer an inbound message to waiting conversations_turn calls and pending questions on that session.
// Returns true when it was consumed (no normal agent turn should start).
bool ConvConsumeInbound(const std::string& channel, const std::string& to, const std::string& session_key, const std::string& text);
bool IsConvRpc(const std::string& method, const nlohmann::json& params);
// {ok:true,payload} | {ok:false,error}
nlohmann::json ConvRpc(const std::string& method, const nlohmann::json& params);
// Agent tools implemented here; returns false when `name` is not one of them.
bool ExtraGatewayTool(const std::string& name, const nlohmann::json& args, const std::string& agent, const std::string& session_key,
                      const nlohmann::json& cfg, const std::string& tool_root, std::string* out);
std::vector<std::string> ExtraGatewayToolNames();
// Plugin web search providers (api.registerWebSearchProvider): (provider id or "", tool args) → {ok, provider, result}|{ok:false,error}.
void SetPluginWebSearch(std::function<nlohmann::json(const std::string& provider, const nlohmann::json& args)> fn);
}  // namespace openclaw::gateway
