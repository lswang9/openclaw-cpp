#include "openclaw/channels/discord.hpp"
#include "openclaw/channels/discord_commands.hpp"
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
    gateway_.SetInteractionHandler(
        [this](const nlohmann::json& in, const DeliverFn& d) { return HandleInteraction(in, d, true); });
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
    if (!Env("DISCORD_BOT_TOKEN").empty() && !Env("DISCORD_APPLICATION_ID").empty() && !specs_.empty()) (void)SyncCommands();
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

    if (body.value("type", 0) == 2 || body.value("type", 0) == 3) return HandleInteraction(body, deliver, false);

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
    msg.explicit_key = chan.empty() && body.contains("session_key");
    msg.conversation_id = chan;
    msg.chat_type = body.contains("guild_id") && !body["guild_id"].is_null() ? "channel" : "direct";
    if (body.contains("guild_id") && body["guild_id"].is_string()) msg.guild_id = body["guild_id"];
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

nlohmann::json DiscordAdapter::Api(const std::string& method, const std::string& path, const nlohmann::json& body) {
    nlohmann::json entry{{"api", method + " " + path}, {"body", body}, {"mode", "recorded"}};
    const auto token = Env("DISCORD_BOT_TOKEN");
    if (!token.empty()) {
        auto base = Env("DISCORD_API_BASE");
        if (base.empty()) base = "https://discord.com/api/v10";
        auto r = HttpJson(method, base + path, body, {"Authorization: Bot " + token});
        entry["mode"] = "live_http";
        entry["http"] = r;
        entry["delivered"] = r.value("http", 0) >= 200 && r.value("http", 0) < 300;
    }
    Record(entry);
    return entry;
}

nlohmann::json DiscordAdapter::SyncCommands() {
    const auto app = Env("DISCORD_APPLICATION_ID");
    const auto guild = Env("DISCORD_GUILD_ID");
    std::string path = "/applications/" + (app.empty() ? std::string("{application_id}") : app) +
                       (guild.empty() ? "/commands" : "/guilds/" + guild + "/commands");
    auto r = Api("PUT", path, specs_);
    r["count"] = specs_.size();
    if (app.empty()) r["note"] = "DISCORD_APPLICATION_ID unset: recorded only";
    return r;
}

nlohmann::json DiscordAdapter::HandleInteraction(const nlohmann::json& in, const DeliverFn& deliver, bool via_gateway) {
    std::string text = discord_commands::InteractionText(in);
    if (text.empty() && in.value("type", 0) == 3 && in.contains("data"))  // message component
        text = "[Discord interaction] clicked " + in["data"].value("custom_id", "");
    if (text.empty()) return {{"ok", true}, {"ignored", true}, {"type", 1}};
    InboundMessage msg;
    msg.channel = "discord";
    msg.text = text;
    if (in.contains("member") && in["member"].contains("user")) msg.sender = in["member"]["user"].value("id", "discord");
    else if (in.contains("user")) msg.sender = in["user"].value("id", "discord");
    else msg.sender = "discord";
    const auto chan = in.value("channel_id", "");
    msg.session_key = chan.empty() ? "discord-" + msg.sender : "discord-" + chan;
    msg.conversation_id = chan;
    const bool guild = in.contains("guild_id") && !in["guild_id"].is_null();
    msg.chat_type = guild ? "channel" : "direct";
    if (!guild) msg.peer_id = msg.sender;
    else if (in["guild_id"].is_string()) msg.guild_id = in["guild_id"];
    ++inbound_;
    const auto iid = in.value("id", ""), itok = in.value("token", "");
    nlohmann::json out{{"ok", true}, {"channel", "discord"}, {"command_text", text}, {"impl", "real"}};
    if (via_gateway && !iid.empty()) out["ack"] = Api("POST", "/interactions/" + iid + "/" + itok + "/callback", {{"type", 5}});
    std::string reply = deliver ? deliver(msg) : "";
    if (reply.size() > 2000) reply = reply.substr(0, 1997) + "...";
    out["session_key"] = msg.session_key;
    out["reply"] = reply;
    if (via_gateway) {
        const auto app = in.value("application_id", Env("DISCORD_APPLICATION_ID"));
        out["followup"] = Api("PATCH", "/webhooks/" + app + "/" + itok + "/messages/@original",
                              {{"content", reply.empty() ? "(done)" : reply}});
    } else {
        out["type"] = 4;  // CHANNEL_MESSAGE_WITH_SOURCE
        out["data"] = {{"content", reply.empty() ? "(done)" : reply}};
    }
    return out;
}

}  // namespace openclaw::channels
