#include "openclaw/channels/discord.hpp"
#include <cstdlib>

namespace openclaw::channels {
namespace {
std::string Env(const char* k) {
    if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}
}  // namespace

DiscordAdapter::DiscordAdapter()
    : OutboundRecordAdapter("discord",
                            "Discord outbound + webhook inbound + Gateway WS (MESSAGE_CREATE)",
                            "/api/v10/channels/{id}/messages") {
    SetEnabled(true);
}

bool DiscordAdapter::Verify(const nlohmann::json& body) const {
    const auto expect = Env("DISCORD_VERIFY_TOKEN");
    if (expect.empty()) return true;
    return body.value("token", body.value("verify_token", "")) == expect;
}

std::string DiscordAdapter::ExtractText(const nlohmann::json& body) {
    if (body.contains("content") && body["content"].is_string()) return body.value("content", "");
    if (body.contains("text")) return body.value("text", "");
    if (body.contains("data") && body["data"].is_object()) {
        if (body["data"].contains("options") && body["data"]["options"].is_array()) {
            for (auto& o : body["data"]["options"]) {
                if (o.value("name", "") == "text" || o.value("name", "") == "message")
                    return o.value("value", "");
            }
        }
        if (body["data"].contains("custom_id")) return body["data"].value("custom_id", "");
        if (body["data"].contains("name")) return body["data"].value("name", "");
    }
    if (body.contains("message") && body["message"].is_object())
        return body["message"].value("content", "");
    return {};
}

nlohmann::json DiscordAdapter::Status() const {
    auto s = OutboundRecordAdapter::Status();
    s["type"] = "discord";
    s["inbound"] = inbound_;
    s["inbound_path"] = "/api/channels/discord/inbound";
    s["verify_token_set"] = !Env("DISCORD_VERIFY_TOKEN").empty();
    s["gateway"] = gateway_.Status();
    return s;
}

void DiscordAdapter::Start(DeliverFn deliver) {
    SetEnabled(true);
    (void)gateway_.Start(std::move(deliver));
}

void DiscordAdapter::Stop() { gateway_.Stop(); }

nlohmann::json DiscordAdapter::HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) {
    if (!Enabled()) return {{"ok", false}, {"error", "adapter disabled"}};
    if (!Verify(body)) return {{"ok", false}, {"error", "verify token mismatch"}, {"impl", "real"}};

    // Discord Interactions Ping
    if (body.value("type", 0) == 1) {
        return {{"ok", true}, {"type", 1}, {"impl", "real"}, {"mode", "discord_ping"}};
    }

    std::string text = ExtractText(body);
    if (text.empty()) {
        return {{"ok", true}, {"ignored", true}, {"note", "no message text"}, {"impl", "real"}};
    }

    InboundMessage msg;
    msg.channel = "discord";
    msg.text = text;
    msg.sender = "discord";
    if (body.contains("member") && body["member"].contains("user"))
        msg.sender = body["member"]["user"].value("id", msg.sender);
    else if (body.contains("author"))
        msg.sender = body["author"].value("id", body["author"].value("username", msg.sender));
    auto chan = body.value("channel_id", "");
    msg.session_key = chan.empty() ? body.value("session_key", "discord-default")
                                   : ("discord-" + chan);
    ++inbound_;
    std::string reply = deliver ? deliver(msg) : "";
    return {{"ok", true},
            {"channel", "discord"},
            {"session_key", msg.session_key},
            {"sender", msg.sender},
            {"reply", reply},
            {"inbound_total", inbound_},
            {"impl", "real"},
            // Discord interaction response shape (channel message)
            {"type", 4},
            {"data", {{"content", reply.empty() ? "(ack)" : reply}}}};
}

}  // namespace openclaw::channels
