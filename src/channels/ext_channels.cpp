#include "openclaw/channels/ext_channels.hpp"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <regex>

#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/registry.hpp"

namespace openclaw::channels {
namespace {
using json = nlohmann::json;
size_t Wcb(char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
json Http(const std::string& method, const std::string& url, const std::string& body, const std::vector<std::string>& headers) {
    CURL* c = curl_easy_init(); if (!c) return {{"http", 0}, {"error", "curl init"}};
    std::string resp; curl_slist* h = nullptr; for (auto& x : headers) h = curl_slist_append(h, x.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str()); curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Wcb); curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L); curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    const auto rc = curl_easy_perform(c); long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    json out{{"http", code}, {"body", resp}};
    if (rc != CURLE_OK) out["error"] = curl_easy_strerror(rc);
    auto j = json::parse(resp, nullptr, false); if (!j.is_discarded()) out["json"] = j;
    return out;
}
json PostJson(const std::string& url, const json& b, std::vector<std::string> hdrs) { hdrs.push_back("Content-Type: application/json"); return Http("POST", url, b.dump(), hdrs); }
std::string Trim(std::string s) { while (!s.empty() && s.back() == '/') s.pop_back(); return s; }
std::string Esc(const std::string& s) { CURL* c = curl_easy_init(); char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size())); std::string r = e ? e : ""; curl_free(e); curl_easy_cleanup(c); return r; }
std::string Cred(const json& cfg, const std::string& ch, const char* key, const char* env, const char* def = "") { auto v = ChannelCred(cfg, ch, key, env); return v.empty() ? def : v; }
std::string S(const json& j, const char* k) { return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); }
long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// Teams: Bot Framework serviceUrl per conversation, and the cached client-credentials token.
std::mutex g_mu;
std::map<std::string, std::string> g_teams_service;  // conversation id → serviceUrl
std::string g_teams_token; long long g_teams_token_exp = 0;
std::string TeamsToken(const json& cfg) {
    const auto app = Cred(cfg, "msteams", "appId", "MSTEAMS_APP_ID"), pw = Cred(cfg, "msteams", "appPassword", "MSTEAMS_APP_PASSWORD");
    if (app.empty() || pw.empty()) return {};
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_teams_token.empty() && NowMs() < g_teams_token_exp) return g_teams_token;
    const auto tenant = Cred(cfg, "msteams", "tenantId", "MSTEAMS_TENANT_ID", "botframework.com");
    auto url = Cred(cfg, "msteams", "tokenUrl", "MSTEAMS_TOKEN_URL");
    if (url.empty()) url = "https://login.microsoftonline.com/" + tenant + "/oauth2/v2.0/token";
    auto r = Http("POST", url, "grant_type=client_credentials&client_id=" + Esc(app) + "&client_secret=" + Esc(pw) + "&scope=" + Esc("https://api.botframework.com/.default"),
                  {"Content-Type: application/x-www-form-urlencoded"});
    if (r.value("http", 0) != 200 || !r.contains("json")) return {};
    g_teams_token = S(r["json"], "access_token");
    g_teams_token_exp = NowMs() + (r["json"].value("expires_in", 3600) - 60) * 1000LL;
    return g_teams_token;
}
}  // namespace

bool IsExtChannel(const std::string& id) { return id == "signal" || id == "matrix" || id == "msteams" || id == "googlechat" || id == "mattermost"; }

json ExtChannelSend(const std::string& id, const json& cfg, const std::string& to, const std::string& text, const json& ex_in) {
    const json ex = ex_in.is_object() ? ex_in : json::object();
    if (to.empty()) return nullptr;
    if (id == "signal") {
        const auto base = Trim(Cred(cfg, "signal", "apiBase", "SIGNAL_API_BASE")), number = Cred(cfg, "signal", "number", "SIGNAL_NUMBER");
        if (base.empty() || number.empty()) return nullptr;
        return PostJson(base + "/v2/send", {{"message", text}, {"number", number}, {"recipients", json::array({to})}}, {});
    }
    if (id == "matrix") {
        const auto hs = Trim(Cred(cfg, "matrix", "homeserver", "MATRIX_HOMESERVER")), tok = Cred(cfg, "matrix", "accessToken", "MATRIX_ACCESS_TOKEN");
        if (hs.empty() || tok.empty()) return nullptr;
        static std::atomic<long long> txn{0};
        json b{{"msgtype", "m.text"}, {"body", text}};
        if (!S(ex, "threadId").empty()) b["m.relates_to"] = {{"rel_type", "m.thread"}, {"event_id", S(ex, "threadId")}};
        else if (!S(ex, "replyTo").empty()) b["m.relates_to"] = {{"m.in_reply_to", {{"event_id", S(ex, "replyTo")}}}};
        const auto t = "oc" + std::to_string(NowMs()) + "-" + std::to_string(++txn);
        return Http("PUT", hs + "/_matrix/client/v3/rooms/" + Esc(to) + "/send/m.room.message/" + t, b.dump(),
                    {"Content-Type: application/json", "Authorization: Bearer " + tok});
    }
    if (id == "msteams") {
        std::string svc = S(ex, "serviceUrl");
        if (svc.empty()) { std::lock_guard<std::mutex> l(g_mu); auto it = g_teams_service.find(to); if (it != g_teams_service.end()) svc = it->second; }
        if (svc.empty()) svc = Cred(cfg, "msteams", "serviceUrl", "MSTEAMS_SERVICE_URL");
        const auto tok = TeamsToken(cfg);
        if (svc.empty() || tok.empty()) return nullptr;
        json b{{"type", "message"}, {"text", text}, {"textFormat", "markdown"}};
        if (!S(ex, "replyToId").empty()) b["replyToId"] = ex["replyToId"];
        return PostJson(Trim(svc) + "/v3/conversations/" + Esc(to) + "/activities" + (S(ex, "replyToId").empty() ? "" : "/" + Esc(S(ex, "replyToId"))), b,
                        {"Authorization: Bearer " + tok});
    }
    if (id == "googlechat") {
        const auto tok = Cred(cfg, "googlechat", "accessToken", "GOOGLE_CHAT_ACCESS_TOKEN");
        if (tok.empty()) return nullptr;
        const auto base = Trim(Cred(cfg, "googlechat", "apiBase", "GOOGLE_CHAT_API_BASE", "https://chat.googleapis.com"));
        json b{{"text", text}};
        std::string q;
        if (!S(ex, "threadName").empty()) { b["thread"] = {{"name", S(ex, "threadName")}}; q = "?messageReplyOption=REPLY_MESSAGE_FALLBACK_TO_NEW_THREAD"; }
        return PostJson(base + "/v1/" + to + "/messages" + q, b, {"Authorization: Bearer " + tok});
    }
    if (id == "mattermost") {
        const auto base = Trim(Cred(cfg, "mattermost", "baseUrl", "MATTERMOST_URL")), tok = Cred(cfg, "mattermost", "botToken", "MATTERMOST_BOT_TOKEN");
        if (base.empty() || tok.empty()) return nullptr;
        json b{{"channel_id", to}, {"message", text}};
        if (!S(ex, "rootId").empty()) b["root_id"] = ex["rootId"];
        return PostJson(base + "/api/v4/posts", b, {"Authorization: Bearer " + tok});
    }
    return nullptr;
}

json ExtChannelInbound(const std::string& id, const json& cfg, const json& body, const DeliverFn& deliver, const ExtSendFn& send) {
    InboundMessage m; m.channel = id;
    std::string to; json ex = json::object();
    auto ignored = [&](const std::string& why) { return json{{"ok", true}, {"ignored", true}, {"note", why}, {"channel", id}}; };
    if (id == "signal") {
        // signal-cli REST/JSON-RPC receive envelope: {envelope:{sourceNumber, dataMessage:{message, groupInfo:{groupId}}}}
        const json& env = body.contains("envelope") ? body["envelope"] : body.contains("params") && body["params"].contains("envelope") ? body["params"]["envelope"] : json();
        if (!env.is_object() || !env.contains("dataMessage")) return ignored("no dataMessage");
        const auto& dm = env["dataMessage"];
        m.text = S(dm, "message");
        m.sender = S(env, "sourceNumber").empty() ? S(env, "source") : S(env, "sourceNumber");
        if (m.sender.empty() && env.contains("sourceUuid")) m.sender = S(env, "sourceUuid");
        if (dm.contains("groupInfo") && !S(dm["groupInfo"], "groupId").empty()) {
            m.chat_type = "group"; m.conversation_id = S(dm["groupInfo"], "groupId");
            to = m.conversation_id.rfind("group.", 0) == 0 ? m.conversation_id : "group." + m.conversation_id;
        } else { m.chat_type = "direct"; to = m.sender; }
        if (m.sender == Cred(cfg, "signal", "number", "SIGNAL_NUMBER")) return ignored("own message");
    } else if (id == "matrix") {
        // Application-service transaction {events:[...]} or a single m.room.message event.
        json evs = body.contains("events") ? body["events"] : json::array({body});
        json ev;
        for (auto& e : evs) if (S(e, "type") == "m.room.message" && e.contains("content")) { ev = e; break; }
        if (!ev.is_object()) return ignored("no m.room.message");
        if (S(ev, "sender") == Cred(cfg, "matrix", "userId", "MATRIX_USER_ID")) return ignored("own message");
        m.text = S(ev["content"], "body"); m.sender = S(ev, "sender");
        to = S(ev, "room_id"); m.chat_type = "group"; m.conversation_id = to;
        auto dms = config::GetPath(cfg, "channels.matrix.dmRooms");  // rooms known to be 1:1
        if (dms && dms->is_array()) for (auto& r : *dms) if (r.is_string() && r.get<std::string>() == to) { m.chat_type = "direct"; m.conversation_id.clear(); m.peer_id = m.sender; }
        if (ev["content"].contains("m.relates_to") && S(ev["content"]["m.relates_to"], "rel_type") == "m.thread") {
            m.thread_id = S(ev["content"]["m.relates_to"], "event_id"); ex["threadId"] = m.thread_id;
        }
        if (m.chat_type == "direct") to = S(ev, "room_id");
    } else if (id == "msteams") {
        if (S(body, "type") != "message") return ignored("activity type " + S(body, "type"));
        m.text = std::regex_replace(S(body, "text"), std::regex("<at>[^<]*</at>\\s*"), "");
        m.sender = body.contains("from") ? S(body["from"], "id") : "";
        const json conv = body.value("conversation", json::object());
        to = S(conv, "id");
        const auto ctype = S(conv, "conversationType");
        m.chat_type = ctype == "personal" || ctype.empty() ? "direct" : (ctype == "channel" ? "channel" : "group");
        if (m.chat_type != "direct") m.conversation_id = to;
        if (!S(body, "serviceUrl").empty()) { std::lock_guard<std::mutex> l(g_mu); g_teams_service[to] = S(body, "serviceUrl"); ex["serviceUrl"] = body["serviceUrl"]; }
        if (body.contains("channelData") && body["channelData"].contains("team")) m.team_id = S(body["channelData"]["team"], "id");
        if (m.chat_type != "direct" && !S(body, "id").empty()) ex["replyToId"] = body["id"];
    } else if (id == "googlechat") {
        if (S(body, "type") != "MESSAGE" || !body.contains("message")) return ignored("event type " + S(body, "type"));
        const auto& msg = body["message"];
        m.text = S(msg, "argumentText").empty() ? S(msg, "text") : S(msg, "argumentText");
        while (!m.text.empty() && m.text[0] == ' ') m.text.erase(0, 1);
        m.sender = msg.contains("sender") ? S(msg["sender"], "name") : "";
        const json space = body.contains("space") ? body["space"] : msg.value("space", json::object());
        to = S(space, "name");
        const auto st = S(space, "type").empty() ? S(space, "spaceType") : S(space, "type");
        m.chat_type = st == "DM" || st == "DIRECT_MESSAGE" ? "direct" : "group";
        if (m.chat_type == "group") m.conversation_id = to;
        if (msg.contains("thread") && !S(msg["thread"], "name").empty()) { ex["threadName"] = msg["thread"]["name"]; if (m.chat_type == "group" && !S(space, "threaded").empty()) m.thread_id = S(msg["thread"], "name"); }
    } else if (id == "mattermost") {
        // Outgoing webhook (form or JSON): token, channel_id, user_id, user_name, text, post_id, trigger_word
        const auto want = Cred(cfg, "mattermost", "webhookToken", "MATTERMOST_WEBHOOK_TOKEN");
        if (!want.empty() && S(body, "token") != want) return {{"ok", false}, {"error", "mattermost webhook token mismatch"}, {"status", 401}};
        m.text = S(body, "text"); m.sender = S(body, "user_id").empty() ? S(body, "user_name") : S(body, "user_id");
        if (!S(body, "trigger_word").empty() && m.text.rfind(S(body, "trigger_word"), 0) == 0) m.text = m.text.substr(S(body, "trigger_word").size());
        while (!m.text.empty() && m.text[0] == ' ') m.text.erase(0, 1);
        to = S(body, "channel_id");
        m.chat_type = S(body, "channel_type") == "D" ? "direct" : "group";
        if (m.chat_type == "group") m.conversation_id = to;
        if (!S(body, "post_id").empty()) ex["rootId"] = S(body, "root_id").empty() ? body["post_id"] : body["root_id"];
        if (m.sender == Cred(cfg, "mattermost", "botUserId", "MATTERMOST_BOT_USER_ID")) return ignored("own message");
    }
    if (m.text.empty()) return ignored("no text");
    if (m.chat_type == "direct") m.peer_id = m.sender;
    m.session_key = id + "-" + to;
    auto reply = deliver ? deliver(m) : std::string();
    json out{{"ok", true}, {"channel", id}, {"reply", reply}, {"impl", "real"}, {"to", to}};
    if (reply.empty()) return out;
    json sent = send ? send(to, reply, ex) : json();
    out["sent"] = sent;
    // Google Chat HTTP apps may answer synchronously: without API credentials the reply rides in the response body.
    if (id == "googlechat" && !(sent.is_object() && sent.value("delivered", false))) {
        json sync{{"text", reply}};
        if (ex.contains("threadName")) sync["thread"] = {{"name", ex["threadName"]}};
        out["syncResponse"] = sync;
    }
    return out;
}
}  // namespace openclaw::channels
