#include "openclaw/plugins/js_extensions.hpp"

namespace openclaw::plugins {

PluginChannelAdapter::PluginChannelAdapter(JsHost* host, std::string id, std::string label)
    : host_(host), id_(std::move(id)), label_(std::move(label)) {}

nlohmann::json PluginChannelAdapter::Status() const {
    return {{"id", id_}, {"label", label_}, {"type", "plugin"}, {"enabled", enabled_}, {"inbound", inbound_},
            {"sent", sent_}, {"inbound_path", "/api/channels/" + id_ + "/inbound"}, {"impl", "real"}};
}

nlohmann::json PluginChannelAdapter::SendText(const std::string& to, const std::string& text) {
    auto r = host_->Rpc("channel_send", {{"channel", id_}, {"to", to}, {"text", text}});
    if (!r.contains("error")) ++sent_;
    return r;
}

nlohmann::json PluginChannelAdapter::HandleHttpInbound(const nlohmann::json& body, const channels::DeliverFn& deliver) {
    if (!enabled_) return {{"ok", false}, {"error", "adapter disabled"}};
    auto parsed = host_->Rpc("channel_inbound", {{"channel", id_}, {"body", body}});
    if (parsed.contains("error")) return {{"ok", false}, {"error", parsed["error"]}};
    nlohmann::json results = nlohmann::json::array();
    for (auto& m : parsed.value("messages", nlohmann::json::array())) {
        channels::InboundMessage msg;
        msg.channel = id_;
        msg.sender = m.value("from", std::string("unknown"));
        msg.text = m.value("text", "");
        msg.chat_type = m.value("chatType", std::string("direct"));
        msg.conversation_id = m.value("conversationId", "");
        msg.thread_id = m.value("threadId", "");
        msg.peer_id = msg.sender;
        msg.session_key = id_ + "-" + (msg.conversation_id.empty() ? msg.sender : msg.conversation_id);
        ++inbound_;
        std::string reply = deliver ? deliver(msg) : "";
        nlohmann::json one{{"from", msg.sender}, {"reply", reply}};
        if (!reply.empty()) one["sent"] = SendText(msg.conversation_id.empty() ? msg.sender : msg.conversation_id, reply);
        results.push_back(one);
    }
    return {{"ok", true}, {"channel", id_}, {"results", results}, {"impl", "real"}};
}

PluginProvider::PluginProvider(JsHost* host, std::string id, std::string model)
    : host_(host), id_(std::move(id)), model_(std::move(model)) {}

agent::CompletionResult PluginProvider::Complete(const std::vector<agent::Message>& messages, const nlohmann::json& tools) {
    agent::CompletionResult out;
    nlohmann::json msgs = nlohmann::json::array();
    for (auto& m : messages) msgs.push_back(m.ToJson());
    auto r = host_->Rpc("provider_complete", {{"provider", id_}, {"model", model_}, {"messages", msgs},
                                              {"tools", tools.is_null() ? nlohmann::json::array() : tools}});
    if (r.contains("error")) {
        out.ok = false;
        out.finish_reason = "error";
        out.error = "plugin provider " + id_ + ": " + r["error"].value("message", r["error"].dump());
        return out;
    }
    out.message.role = "assistant";
    out.message.content = r.value("content", "");
    int i = 0;
    for (auto& tc : r.value("tool_calls", nlohmann::json::array())) {
        agent::ToolCall c;
        c.id = tc.value("id", "call_plugin_" + std::to_string(i++));
        c.name = tc.value("name", tc.contains("function") ? tc["function"].value("name", "") : "");
        auto a = tc.contains("arguments") ? tc["arguments"] : (tc.contains("function") ? tc["function"]["arguments"] : nlohmann::json::object());
        c.arguments_json = a.is_string() ? a.get<std::string>() : a.dump();
        out.tool_calls.push_back(c);
        out.message.tool_calls.push_back({{"id", c.id}, {"type", "function"},
                                          {"function", {{"name", c.name}, {"arguments", c.arguments_json}}}});
    }
    out.finish_reason = out.tool_calls.empty() ? "stop" : "tool_calls";
    return out;
}

}  // namespace openclaw::plugins
