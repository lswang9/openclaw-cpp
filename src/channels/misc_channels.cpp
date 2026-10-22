#include "openclaw/channels/misc_channels.hpp"
#include "openclaw/channels/irc_native.hpp"

#include <curl/curl.h>

#include <map>
#include <mutex>

#include "openclaw/channels/outbound.hpp"
#include "openclaw/config/config.hpp"

namespace openclaw::channels {
namespace {
using json = nlohmann::json;
size_t Wcb(char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
json Http(const std::string& method, const std::string& url, const std::string& body, const std::vector<std::string>& headers) {
    CURL* c = curl_easy_init(); if (!c) return {{"http", 0}, {"error", "curl init"}};
    std::string resp; curl_slist* h = nullptr; for (auto& x : headers) h = curl_slist_append(h, x.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (method != "GET") curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Wcb); curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    const auto rc = curl_easy_perform(c); long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    json out{{"http", code}, {"body", resp}};
    if (rc != CURLE_OK) out["error"] = curl_easy_strerror(rc);
    auto j = json::parse(resp, nullptr, false); if (!j.is_discarded()) out["json"] = j;
    return out;
}
json PostJson(const std::string& url, const json& b, std::vector<std::string> hdrs) {
    hdrs.push_back("Content-Type: application/json"); return Http("POST", url, b.dump(), hdrs);
}
std::string Trim(std::string s) { while (!s.empty() && s.back() == '/') s.pop_back(); return s; }
std::string Cred(const json& cfg, const std::string& ch, const char* key, const char* env, const char* def = "") {
    auto v = ChannelCred(cfg, ch, key, env); return v.empty() ? def : v;
}
std::string S(const json& j, const char* k) { return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); }
std::string FirstS(const json& j, std::initializer_list<const char*> keys) {
    for (auto* k : keys) { auto v = S(j, k); if (!v.empty()) return v; } return {};
}
json Ignored(const std::string& why) { return {{"ok", true}, {"ignored", true}, {"reason", why}}; }

// LINE reply tokens last ~30s; stash by chat for reply→push fallback.
std::mutex g_mu;
std::map<std::string, std::string> g_line_reply;  // chat → replyToken

const std::vector<std::pair<const char*, const char*>> kCatalog = {
    {"a2a", "A2A agent-to-agent HTTP bridge (POST inbound webhook; outbound POST {apiBase}/message)"},
    {"buzz", "Buzz workspace chat (webhook inbound; outbound POST {apiBase}/v1/messages)"},
    {"clickclack", "ClickClack channel (webhook inbound; outbound POST {apiBase}/send)"},
    {"irc", "IRC via HTTP bridge (inbound PRIVMSG webhook; outbound POST {apiBase}/privmsg)"},
    {"line", "LINE Messaging API (webhook events; reply/push)"},
    {"nextcloud-talk", "Nextcloud Talk (webhook inbound; outbound chat message OCS)"},
    {"nostr", "Nostr DMs via HTTP bridge (NIP-04 handled by the bridge; POST {apiBase}/dm)"},
    {"raft", "Raft channel (webhook inbound; outbound POST {apiBase}/send)"},
    {"reef", "Reef channel (webhook inbound; outbound POST {apiBase}/send)"},
    {"sms", "Twilio SMS/MMS (inbound webhook; outbound Messages.json)"},
    {"synology-chat", "Synology Chat (incoming webhook; outbound incoming webhook URL)"},
    {"tlon", "Tlon / Urbit (webhook inbound; outbound POST {apiBase}/dm)"},
    {"twitch", "Twitch chat (EventSub/webhook inbound; Helix send-chat-message outbound)"},
    {"x", "X (Twitter) DMs (Account Activity inbound; POST DM events outbound)"},
    {"zalo", "Zalo Bot API (webhook inbound; sendMessage outbound)"},
    {"zalouser", "Zalo user-bot bridge (webhook inbound; outbound POST {apiBase}/send)"},
};
}  // namespace

const std::vector<std::pair<const char*, const char*>>& MiscChannelCatalog() { return kCatalog; }

bool IsMiscChannel(const std::string& id) {
    for (auto& [k, _] : kCatalog) if (id == k) return true;
    return false;
}

json MiscChannelSend(const std::string& id, const json& cfg, const std::string& to, const std::string& text, const json& ex_in) {
    const json ex = ex_in.is_object() ? ex_in : json::object();
    if (to.empty()) return nullptr;
    if (id == "line") {
        const auto tok = Cred(cfg, "line", "channelAccessToken", "LINE_CHANNEL_ACCESS_TOKEN");
        if (tok.empty()) return nullptr;
        std::string base = Trim(Cred(cfg, "line", "apiBase", "LINE_API_BASE", "https://api.line.me"));
        std::string reply;
        { std::lock_guard<std::mutex> l(g_mu); auto it = g_line_reply.find(to); if (it != g_line_reply.end()) { reply = it->second; g_line_reply.erase(it); } }
        if (reply.empty()) reply = S(ex, "replyToken");
        json msg = json::array({{{"type", "text"}, {"text", text}}});
        if (!reply.empty()) return PostJson(base + "/v2/bot/message/reply", {{"replyToken", reply}, {"messages", msg}}, {"Authorization: Bearer " + tok});
        return PostJson(base + "/v2/bot/message/push", {{"to", to}, {"messages", msg}}, {"Authorization: Bearer " + tok});
    }
    if (id == "zalo") {
        const auto tok = Cred(cfg, "zalo", "botToken", "ZALO_BOT_TOKEN");
        if (tok.empty()) return nullptr;
        std::string base = Trim(Cred(cfg, "zalo", "apiBase", "ZALO_API_BASE", "https://bot-api.zaloplatforms.com"));
        return PostJson(base + "/bot" + tok + "/sendMessage", {{"chat_id", to}, {"text", text}}, {});
    }
    if (id == "sms") {
        const auto sid = Cred(cfg, "sms", "accountSid", "TWILIO_ACCOUNT_SID"), token = Cred(cfg, "sms", "authToken", "TWILIO_AUTH_TOKEN");
        const auto from = Cred(cfg, "sms", "from", "TWILIO_FROM");
        if (sid.empty() || token.empty() || from.empty()) return nullptr;
        std::string base = Trim(Cred(cfg, "sms", "apiBase", "TWILIO_API_BASE", "https://api.twilio.com"));
        auto esc = [](const std::string& s) {
            CURL* c = curl_easy_init(); char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
            std::string r = e ? e : ""; curl_free(e); curl_easy_cleanup(c); return r;
        };
        auto b64 = [](const std::string& raw) {
            static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string o;
            for (size_t i = 0; i < raw.size(); i += 3) {
                unsigned v = (unsigned char)raw[i] << 16;
                if (i + 1 < raw.size()) v |= (unsigned char)raw[i + 1] << 8;
                if (i + 2 < raw.size()) v |= (unsigned char)raw[i + 2];
                o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
                o += (i + 1 < raw.size()) ? t[(v >> 6) & 63] : '=';
                o += (i + 2 < raw.size()) ? t[v & 63] : '=';
            }
            return o;
        };
        const std::string body = "To=" + esc(to) + "&From=" + esc(from) + "&Body=" + esc(text);
        return Http("POST", base + "/2010-04-01/Accounts/" + sid + "/Messages.json", body,
                    {"Content-Type: application/x-www-form-urlencoded", "Authorization: Basic " + b64(sid + ":" + token)});
    }
    if (id == "synology-chat") {
        const auto url = Cred(cfg, "synology-chat", "incomingUrl", "SYNOLOGY_CHAT_INCOMING_URL");
        if (url.empty()) return nullptr;
        return PostJson(url, {{"text", text}, {"user_ids", json::array({to})}}, {});
    }
    // 1.143: a live native IRC connection (channels.irc.server, or Twitch mode "irc") carries replies directly.
    if ((id == "irc" || id == "twitch") && IrcNativeSend(id, id == "twitch" && !to.empty() && to[0] != '#' ? "#" + to : to, text))
        return json{{"http", 200}, {"ok", true}, {"via", "irc-native"}, {"target", to}};
    if (id == "twitch") {
        const auto tok = Cred(cfg, "twitch", "accessToken", "TWITCH_ACCESS_TOKEN");
        const auto cid = Cred(cfg, "twitch", "clientId", "TWITCH_CLIENT_ID");
        const auto sender = Cred(cfg, "twitch", "senderId", "TWITCH_SENDER_ID");
        if (tok.empty() || cid.empty()) return nullptr;
        std::string base = Trim(Cred(cfg, "twitch", "apiBase", "TWITCH_API_BASE", "https://api.twitch.tv"));
        json b{{"message", text}, {"broadcaster_id", to}};
        if (!sender.empty()) b["sender_id"] = sender;
        return PostJson(base + "/helix/chat/messages", b, {"Authorization: Bearer " + tok, "Client-Id: " + cid});
    }
    if (id == "x") {
        const auto tok = Cred(cfg, "x", "accessToken", "X_ACCESS_TOKEN");
        if (tok.empty()) return nullptr;
        std::string base = Trim(Cred(cfg, "x", "apiBase", "X_API_BASE", "https://api.x.com"));
        return PostJson(base + "/2/dm_conversations/with/" + to + "/messages", {{"text", text}}, {"Authorization: Bearer " + tok});
    }
    if (id == "nextcloud-talk") {
        const auto base = Trim(Cred(cfg, "nextcloud-talk", "baseUrl", "NEXTCLOUD_BASE_URL"));
        const auto user = Cred(cfg, "nextcloud-talk", "user", "NEXTCLOUD_USER");
        const auto pass = Cred(cfg, "nextcloud-talk", "appPassword", "NEXTCLOUD_APP_PASSWORD");
        if (base.empty() || user.empty() || pass.empty()) return nullptr;
        std::string auth = user + ":" + pass;
        static const char* t="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64; for (size_t i=0;i<auth.size();i+=3){ unsigned v=(unsigned char)auth[i]<<16; if(i+1<auth.size())v|=(unsigned char)auth[i+1]<<8; if(i+2<auth.size())v|=(unsigned char)auth[i+2];
          b64+=t[(v>>18)&63]; b64+=t[(v>>12)&63]; b64+=(i+1<auth.size())?t[(v>>6)&63]:'='; b64+=(i+2<auth.size())?t[v&63]:'='; }
        return PostJson(base + "/ocs/v2.php/apps/spreed/api/v1/chat/" + to, {{"message", text}},
                        {"Authorization: Basic " + b64, "OCS-APIRequest: true"});
    }
    if (id == "irc" || id == "nostr" || id == "a2a" || id == "buzz" || id == "clickclack" || id == "raft" || id == "reef" ||
        id == "tlon" || id == "zalouser") {
        const auto base = Trim(Cred(cfg, id, "apiBase", (std::string(id == "irc" ? "IRC_API_BASE" : id == "nostr" ? "NOSTR_API_BASE" : id == "a2a" ? "A2A_API_BASE" : id == "buzz" ? "BUZZ_API_BASE" : id == "clickclack" ? "CLICKCLACK_API_BASE" : id == "raft" ? "RAFT_API_BASE" : id == "reef" ? "REEF_API_BASE" : id == "tlon" ? "TLON_API_BASE" : "ZALOUSER_API_BASE")).c_str()));
        const auto tok = Cred(cfg, id, "token", (std::string(id == "irc" ? "IRC_TOKEN" : id == "nostr" ? "NOSTR_TOKEN" : id == "a2a" ? "A2A_TOKEN" : id == "buzz" ? "BUZZ_TOKEN" : id == "clickclack" ? "CLICKCLACK_TOKEN" : id == "raft" ? "RAFT_TOKEN" : id == "reef" ? "REEF_TOKEN" : id == "tlon" ? "TLON_TOKEN" : "ZALOUSER_TOKEN")).c_str());
        if (base.empty()) return nullptr;
        std::string path = id == "irc" ? "/privmsg" : id == "nostr" ? "/dm" : id == "buzz" ? "/v1/messages" : id == "a2a" ? "/message" : id == "tlon" ? "/dm" : "/send";
        json b{{"to", to}, {"text", text}, {"message", text}};
        if (id == "irc") b = {{"target", to}, {"message", text}};
        if (id == "nostr") b = {{"pubkey", to}, {"content", text}};
        std::vector<std::string> h;
        if (!tok.empty()) h.push_back("Authorization: Bearer " + tok);
        return PostJson(base + path, b, h);
    }
    return nullptr;
}

json MiscChannelInbound(const std::string& id, const json& cfg, const json& body, const DeliverFn& deliver, const MiscSendFn& send) {
    auto ignored = [](const std::string& w) { return Ignored(w); };
    InboundMessage m; m.channel = id; m.chat_type = "direct";
    json ex = json::object();
    std::string to;

    // Shared webhook token gate (channels.<id>.webhookToken)
    const auto want = Cred(cfg, id, "webhookToken", "");
    if (!want.empty()) {
        const auto got = FirstS(body, {"token", "webhookToken", "secret"});
        if (got != want) return {{"ok", false}, {"error", id + " webhook token mismatch"}, {"status", 401}};
    }

    if (id == "line") {
        // LINE webhook: {events:[{type:"message", replyToken, source:{userId,type}, message:{type,text}}]}
        json events = body.contains("events") ? body["events"] : json::array({body});
        if (!events.is_array() || events.empty()) return ignored("no events");
        const auto& ev = events[0];
        if (S(ev, "type") != "message") return ignored("non-message");
        const auto& msg = ev.value("message", json::object());
        if (S(msg, "type") != "text") return ignored("non-text");
        m.text = S(msg, "text");
        const auto& src = ev.value("source", json::object());
        m.sender = FirstS(src, {"userId", "groupId", "roomId"});
        to = m.sender;
        if (S(src, "type") == "group" || S(src, "type") == "room") { m.chat_type = "group"; to = FirstS(src, {"groupId", "roomId"}); m.conversation_id = to; }
        const auto reply = S(ev, "replyToken");
        if (!reply.empty() && !to.empty()) { std::lock_guard<std::mutex> l(g_mu); g_line_reply[to] = reply; ex["replyToken"] = reply; }
    } else if (id == "zalo") {
        // Zalo Bot API style: {message:{chat:{id}, from:{id}, text}} or {chat_id, from_id, text}
        const auto& msg = body.contains("message") ? body["message"] : body;
        m.text = FirstS(msg, {"text", "message"});
        m.sender = msg.contains("from") ? FirstS(msg["from"], {"id", "user_id"}) : FirstS(msg, {"from_id", "sender"});
        to = msg.contains("chat") ? FirstS(msg["chat"], {"id"}) : FirstS(msg, {"chat_id", "to"});
        if (to.empty()) to = m.sender;
    } else if (id == "sms") {
        // Twilio webhook form-as-json: From, To, Body
        m.text = FirstS(body, {"Body", "body", "text"});
        m.sender = FirstS(body, {"From", "from"});
        to = m.sender;
    } else if (id == "twitch") {
        m.text = FirstS(body, {"message", "text", "content"});
        m.sender = FirstS(body, {"user_id", "chatter_user_id", "from", "sender"});
        to = FirstS(body, {"broadcaster_user_id", "broadcaster_id", "channel", "to"});
        if (to.empty()) to = Cred(cfg, "twitch", "channel", "TWITCH_CHANNEL");
        m.chat_type = "channel"; m.conversation_id = to;
    } else if (id == "x") {
        m.text = body.contains("message_create") ? FirstS(body["message_create"].value("message_data", json::object()), {"text"})
                                                 : FirstS(body, {"text", "message"});
        m.sender = body.contains("message_create") ? FirstS(body["message_create"], {"sender_id"}) : FirstS(body, {"sender_id", "from"});
        to = m.sender;
    } else if (id == "synology-chat") {
        m.text = FirstS(body, {"text", "message"});
        m.sender = FirstS(body, {"user_id", "username", "from"});
        to = m.sender;
    } else if (id == "nextcloud-talk") {
        m.text = FirstS(body, {"message", "text"});
        m.sender = FirstS(body, {"actorId", "actorDisplayName", "from"});
        to = FirstS(body, {"token", "roomToken", "conversation", "to"});
        if (to.empty()) to = m.sender;
        m.chat_type = "group"; m.conversation_id = to;
    } else if (id == "irc") {
        m.text = FirstS(body, {"message", "text", "content"});
        m.sender = FirstS(body, {"nick", "from", "sender"});
        to = FirstS(body, {"target", "channel", "to"});
        if (!to.empty() && to[0] == '#') { m.chat_type = "channel"; m.conversation_id = to; }
        else { to = m.sender; }
    } else if (id == "nostr") {
        m.text = FirstS(body, {"content", "text", "message"});
        m.sender = FirstS(body, {"pubkey", "from", "sender"});
        to = m.sender;
    } else {
        // Generic: a2a, buzz, clickclack, raft, reef, tlon, zalouser
        m.text = FirstS(body, {"text", "message", "content", "body"});
        m.sender = FirstS(body, {"from", "sender", "user", "user_id", "author"});
        to = FirstS(body, {"to", "chat_id", "channel", "room", "conversation"});
        if (to.empty()) to = m.sender;
        if (!FirstS(body, {"channel", "room", "conversation"}).empty()) { m.chat_type = "group"; m.conversation_id = to; }
    }

    if (m.text.empty()) return ignored("empty text");
    if (m.sender.empty()) m.sender = "unknown";
    if (m.chat_type == "direct") m.peer_id = m.sender;
    if (to.empty()) to = m.sender;
    m.session_key = id + "-" + to;
    auto reply = deliver ? deliver(m) : std::string();
    json out{{"ok", true}, {"channel", id}, {"reply", reply}, {"impl", "real"}, {"to", to}};
    if (reply.empty()) return out;
    json sent = send ? send(to, reply, ex) : json();
    out["sent"] = sent;
    return out;
}

}  // namespace openclaw::channels
