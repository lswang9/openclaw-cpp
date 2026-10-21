#include "openclaw/voice/inbound_audio.hpp"
#include "openclaw/channels/discord.hpp"
#include "openclaw/channels/dave.hpp"
#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/discord_commands.hpp"
#include <cstdlib>
#include "openclaw/channels/discord_voice.hpp"
#include "openclaw/voice/stt.hpp"
#include "openclaw/voice/tts.hpp"
#include <filesystem>
#include <fstream>

namespace openclaw::channels {
namespace {
void ExportIfEmpty(const char* env, const std::string& val) {
    if (val.empty()) return;
    if (const char* e = std::getenv(env); e && *e) return;
    setenv(env, val.c_str(), 0);
}

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
    const auto expect = ChannelCred(Config(), "discord", "verifyToken", "DISCORD_VERIFY_TOKEN");
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
    s["verify_token_set"] = !ChannelCred(Config(), "discord", "verifyToken", "DISCORD_VERIFY_TOKEN").empty();
    s["gateway"] = gateway_.Status();
    return s;
}

void DiscordAdapter::Start(DeliverFn deliver) {
    SetEnabled(true);
    ExportIfEmpty("DISCORD_BOT_TOKEN", ChannelCred(Config(), "discord", "token", "DISCORD_BOT_TOKEN"));
    ExportIfEmpty("DISCORD_APPLICATION_ID", ChannelCred(Config(), "discord", "applicationId", "DISCORD_APPLICATION_ID"));
    if (!ChannelCred(Config(), "discord", "token", "DISCORD_BOT_TOKEN").empty() && !ChannelCred(Config(), "discord", "applicationId", "DISCORD_APPLICATION_ID").empty() && !specs_.empty()) (void)SyncCommands();
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

    if (body.value("type", 0) == 2 || body.value("type", 0) == 3 || body.value("type", 0) == 4) return HandleInteraction(body, deliver, false);

    std::string text = ExtractText(body);
    auto audio = voice::ExtractAudioRefs(body);
    if (text.empty() && audio.empty()) {
        return {{"ok", true}, {"ignored", true}, {"note", "no message text"}, {"impl", "real"}};
    }

    InboundMessage msg;
    msg.channel = "discord";
    msg.text = text;
    msg.media = audio;
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
    auto voice = MaybeAutoVoice(msg.conversation_id, reply, !msg.media.empty());
    return {{"ok", true},
            {"channel", "discord"},
            {"voice", voice},
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
    const auto token = ChannelCred(Config(), "discord", "token", "DISCORD_BOT_TOKEN");
    if (!token.empty()) {
        auto base = ChannelCred(Config(), "discord", "apiBase", "DISCORD_API_BASE");
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
    const auto app = ChannelCred(Config(), "discord", "applicationId", "DISCORD_APPLICATION_ID");
    const auto guild = ChannelCred(Config(), "discord", "guildId", "DISCORD_GUILD_ID");
    std::string path = "/applications/" + (app.empty() ? std::string("{application_id}") : app) +
                       (guild.empty() ? "/commands" : "/guilds/" + guild + "/commands");
    auto r = Api("PUT", path, specs_);
    r["count"] = specs_.size();
    if (app.empty()) r["note"] = "DISCORD_APPLICATION_ID unset: recorded only";
    return r;
}

nlohmann::json DiscordAdapter::HandleInteraction(const nlohmann::json& in, const DeliverFn& deliver, bool via_gateway) {
    if (in.value("type", 0) == 4) {  // autocomplete: answer with choices, no agent turn
        auto f = discord_commands::FocusedOption(in);
        std::vector<std::string> cand;
        if (f && complete_) cand = complete_(f->command, f->option, f->value);
        nlohmann::json data{{"choices", discord_commands::Choices(cand, f ? f->value : std::string())}};
        nlohmann::json out{{"ok", true}, {"type", 8}, {"data", data}, {"autocomplete", true}};
        if (via_gateway) out["callback"] = Api("POST", "/interactions/" + in.value("id", std::string()) + "/" + in.value("token", std::string()) + "/callback", {{"type", 8}, {"data", data}});
        return out;
    }
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
        const auto app = in.value("application_id", ChannelCred(Config(), "discord", "applicationId", "DISCORD_APPLICATION_ID"));
        out["followup"] = Api("PATCH", "/webhooks/" + app + "/" + itok + "/messages/@original",
                              {{"content", reply.empty() ? "(done)" : reply}});
    } else {
        out["type"] = 4;  // CHANNEL_MESSAGE_WITH_SOURCE
        out["data"] = {{"content", reply.empty() ? "(done)" : reply}};
    }
    return out;
}


namespace {
std::string B64Decode(const std::string& in) {
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) {
        auto pos = tbl.find(static_cast<char>(c));
        if (pos == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(pos); bits += 6;
        if (bits >= 0) { out.push_back(static_cast<char>((val >> bits) & 0xff)); bits -= 8; }
    }
    return out;
}
}  // namespace

nlohmann::json DiscordAdapter::Voice(const nlohmann::json& p) {
    namespace dv = discord_voice;
    const auto action = p.value("action", std::string("play"));
    const auto guild = p.value("guild_id", std::string());
    const auto channel = p.value("channel_id", std::string());
    if (action == "status") return {{"ok", true}, {"opus", dv::HaveOpus()}, {"dave", {{"available", openclaw::channels::dave::Available()}, {"max_protocol_version", openclaw::channels::dave::MaxProtocolVersion()}}}, {"gateway", gateway_.Status()}, {"bot_user_id", gateway_.BotUserId()}};
    if (guild.empty()) return {{"ok", false}, {"error", "guild_id required"}};
    if (action == "leave") return gateway_.JoinVoice(guild, "");
    if (channel.empty()) return {{"ok", false}, {"error", "channel_id required"}};
    if (action == "join") return gateway_.JoinVoice(guild, channel, p.value("timeout_ms", 10000));
    if (action != "play") return {{"ok", false}, {"error", "unknown action"}};
    // Audio source → Opus packets.
    std::string raw, err;
    nlohmann::json tts_rep;
    if (p.contains("audio_path")) { std::ifstream f(p["audio_path"].get<std::string>(), std::ios::binary); raw.assign(std::istreambuf_iterator<char>(f), {}); }
    else if (p.contains("audio_b64")) raw = B64Decode(p["audio_b64"].get<std::string>());
    else if (p.contains("text")) {
        voice::TtsEngine tts;
        tts_rep = tts.SpeakStream(p["text"].get<std::string>(), p.value("voice", std::string()), [&](const std::string& c) { raw += c; });
    }
    if (raw.empty()) return {{"ok", false}, {"error", "no audio (audio_path | audio_b64 | text with a TTS provider)"}, {"tts", tts_rep}};
    dv::Bytes bytes(raw.begin(), raw.end());
    auto packets = dv::PacketsFromAudio(bytes, &err);
    if (packets.empty()) return {{"ok", false}, {"error", err.empty() ? "no audio packets" : err}};
    auto j = gateway_.JoinVoice(guild, channel, p.value("timeout_ms", 10000));
    if (!j.value("ok", false)) return j;
    dv::PlayRequest rq;
    rq.endpoint = j["endpoint"]; rq.server_id = guild; rq.user_id = j["user_id"]; rq.session_id = j["session_id"]; rq.token = j["token"]; rq.channel_id = channel;
    rq.packets = std::move(packets);
    rq.listen_ms = p.value("listen_ms", 0);
    if (rq.listen_ms > 0) rq.receive_dir = p.value("receive_dir", (std::filesystem::temp_directory_path() / "openclaw-discord-voice").string());
    auto rep = dv::Play(rq);
    if (p.value("transcribe", false) && rep.contains("received")) {
        for (auto& r : rep["received"]) {
            if (!r.contains("wav")) continue;
            std::ifstream f(r["wav"].get<std::string>(), std::ios::binary);
            std::string wav((std::istreambuf_iterator<char>(f)), {});
            auto tr = voice::DefaultStt().Transcribe(wav, "audio/wav", "voice.wav", "");
            r["transcript"] = tr.value("text", std::string());
            if (!tr.value("ok", true) || tr.contains("error")) r["stt"] = tr;
        }
    }
    if (p.value("leave", true)) (void)gateway_.JoinVoice(guild, "");
    rep["join"] = j;
    if (!tts_rep.is_null()) rep["tts"] = tts_rep;
    return rep;
}

}  // namespace openclaw::channels
