#include <cctype>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdexcept>
#include <random>
#include "openclaw/channels/feishu_ws.hpp"
#include "openclaw/channels/feishu_frame.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <chrono>
#include <cstdlib>

namespace openclaw::channels {
namespace {

std::string Env(const char* k) {
    if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}

bool ForceMock() {
    const auto v = Env("FEISHU_WS_FORCE_MOCK");
    if (v == "1" || v == "true" || v == "yes") return true;
    const auto c = Env("FEISHU_LONGCONN_FORCE_MOCK");
    return c == "1" || c == "true" || c == "yes";
}

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string ExtractMessageText(const nlohmann::json& body) {
    if (body.contains("text") && body["text"].is_string()) return body.value("text", "");
    const nlohmann::json* ev = nullptr;
    if (body.contains("event") && body["event"].is_object()) ev = &body["event"];
    if (!ev && body.contains("data") && body["data"].is_object() &&
        body["data"].contains("event")) {
        ev = &body["data"]["event"];
    }
    if (!ev) return {};
    if (ev->contains("message") && (*ev)["message"].is_object()) {
        auto content = (*ev)["message"].value("content", "");
        if (!content.empty()) {
            try {
                auto c = nlohmann::json::parse(content);
                if (c.contains("text")) return c.value("text", "");
            } catch (...) {
                return content;
            }
        }
        if ((*ev)["message"].contains("text")) return (*ev)["message"].value("text", "");
    }
    if (ev->contains("text")) return (*ev).value("text", "");
    return {};
}

}  // namespace

FeishuLongConn::FeishuLongConn() = default;
FeishuLongConn::~FeishuLongConn() { Stop(); }

nlohmann::json FeishuLongConn::Status() const {
    std::lock_guard lock(mu_);
    return {{"id", "feishu-longconn"},
            {"mode", mode_},
            {"running", running_.load()},
            {"payloads", payloads_},
            {"delivered", delivered_},
            {"pongs", pongs_},
            {"pings", pings_},
            {"binary_events", binary_events_},
            {"replies_sent", replies_sent_},
            {"ping_interval_s", ping_interval_s_},
            {"reconnects", reconnects_},
            {"reconnect", {{"count", reconnect_count_}, {"interval_s", reconnect_interval_s_}, {"nonce_s", reconnect_nonce_s_}}},
            {"card_callbacks", card_callbacks_},
            {"gzip_frames", gzip_frames_},
            {"url_previews", url_previews_},
            {"app_id_set", !Env("FEISHU_APP_ID").empty()},
            {"app_secret_set", !Env("FEISHU_APP_SECRET").empty()},
            {"ws_url_override", !Env("FEISHU_WS_URL").empty()},
            {"tenant_token", tenant_token_hint_},
            {"force_mock", ForceMock()},
            {"last_error", last_error_},
            {"impl", (mode_ == "live") ? "real" : (mode_ == "mock" ? "real-sim" : "honest")},
            {"gaps", nlohmann::json::array(
                         {"url.preview answered with an inline title only (no card preview)",
                          "enterprise self-built apps only (platform limit)",
                          "cluster mode — one of N clients receives each event",
                          "must ack/process within ~3s on live"})}};
}

nlohmann::json FeishuLongConn::HandleEventBody(const nlohmann::json& body, const DeliverFn& deliver) {
    // Ignore url_verification on WS (HTTP only)
    if (body.value("type", "") == "url_verification") {
        return {{"ignored", true}, {"reason", "url_verification"}};
    }
    std::string event_type;
    if (body.contains("header") && body["header"].is_object()) {
        event_type = body["header"].value("event_type", body["header"].value("event_type", ""));
    }
    if (event_type.empty() && body.contains("data") && body["data"].is_object() &&
        body["data"].contains("header")) {
        event_type = body["data"]["header"].value("event_type", "");
    }

    const std::string text = ExtractMessageText(body);
    if (text.empty()) {
        return {{"ignored", true}, {"reason", "no text"}, {"event_type", event_type}};
    }

    InboundMessage msg;
    msg.channel = "feishu";
    msg.text = text;
    msg.sender = "feishu";
    const nlohmann::json* ev = body.contains("event") ? &body["event"] : nullptr;
    if (!ev && body.contains("data") && body["data"].contains("event")) ev = &body["data"]["event"];
    if (ev && ev->contains("sender") && (*ev)["sender"].contains("sender_id")) {
        msg.sender = (*ev)["sender"]["sender_id"].value("open_id", msg.sender);
    }
    msg.session_key = "feishu-default";
    if (ev && ev->contains("message")) {
        auto chat = (*ev)["message"].value("chat_id", "");
        if (!chat.empty()) msg.session_key = "feishu-" + chat;
        const auto& fm = (*ev)["message"];
        msg.conversation_id = chat;
        auto ct = fm.value("chat_type", "");
        msg.chat_type = ct == "p2p" ? "direct" : ct == "group" ? "group" : "";
        msg.thread_id = fm.value("thread_id", "");
    }

    std::string reply = deliver ? deliver(msg) : "";
    {
        std::lock_guard lock(mu_);
        ++delivered_;
    }
    nlohmann::json out{{"delivered", true},
                       {"session_key", msg.session_key},
                       {"reply", reply},
                       {"event_type", event_type.empty() ? "im.message" : event_type}};
    // Optional ack for long-conn message_id
    if (body.contains("message_id")) {
        out["send"] = nlohmann::json::array(
            {{{"type", "ack"}, {"message_id", body["message_id"]}, {"code", 0}}});
    }
    return out;
}

nlohmann::json FeishuLongConn::FeedPayload(const nlohmann::json& payload, const DeliverFn& deliver) {
    {
        std::lock_guard lock(mu_);
        ++payloads_;
        if (mode_ == "idle") mode_ = "mock";
    }

    const auto typ = payload.value("type", "");
    // Ping / heartbeat
    if (typ == "ping" || typ == "ping_frame" || payload.value("cmd", "") == "ping" ||
        payload.value("action", "") == "ping") {
        std::lock_guard lock(mu_);
        ++pongs_;
        return {{"type", "ping"},
                {"send", nlohmann::json::array({{{"type", "pong"}}})}};
    }
    if (typ == "pong") {
        return {{"type", "pong"}, {"ok", true}};
    }

    // Wrapped long-conn event: { type: "event", message_id?, data: <webhook body> }
    if (typ == "event" || typ == "callback") {
        nlohmann::json body = payload.contains("data") ? payload["data"] : payload;
        if (payload.contains("message_id") && !body.contains("message_id")) {
            body = body.is_object() ? body : nlohmann::json::object();
            // keep message_id on outer for ack — pass via merge
            auto route = HandleEventBody(body, deliver);
            if (payload.contains("message_id") && !route.contains("send")) {
                route["send"] = nlohmann::json::array(
                    {{{"type", "ack"}, {"message_id", payload["message_id"]}, {"code", 0}}});
            }
            route["type"] = typ;
            return route;
        }
        auto route = HandleEventBody(body, deliver);
        route["type"] = typ;
        return route;
    }

    // Bare webhook-shaped body (schema 2.0 / event)
    if (payload.contains("event") || payload.contains("header") ||
        payload.value("schema", "") == "2.0") {
        return HandleEventBody(payload, deliver);
    }

    return {{"ignored", true}, {"reason", "unknown frame"}, {"type", typ}};
}

nlohmann::json FeishuLongConn::RunMockFrames(const std::vector<nlohmann::json>& frames,
                                             const DeliverFn& deliver) {
    {
        std::lock_guard lock(mu_);
        mode_ = "mock";
    }
    nlohmann::json results = nlohmann::json::array();
    for (const auto& f : frames) results.push_back(FeedPayload(f, deliver));
    return {{"ok", true},
            {"impl", "real-sim"},
            {"mode", "mock"},
            {"frames", frames.size()},
            {"results", results},
            {"status", Status()}};
}

bool FeishuLongConn::FetchTenantToken(std::string& out_token, std::string& err) {
    const auto app_id = Env("FEISHU_APP_ID");
    const auto app_secret = Env("FEISHU_APP_SECRET");
    if (app_id.empty() || app_secret.empty()) {
        err = "FEISHU_APP_ID / FEISHU_APP_SECRET unset";
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        err = "curl_easy_init failed";
        return false;
    }
    std::string response;
    const auto body =
        nlohmann::json{{"app_id", app_id}, {"app_secret", app_secret}}.dump();
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    // Prefer open.feishu.cn; FEISHU_API_BASE can override (Lark intl)
    std::string base = Env("FEISHU_API_BASE");
    if (base.empty()) base = "https://open.feishu.cn";
    const std::string url = base + "/open-apis/auth/v3/tenant_access_token/internal";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    const CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        return false;
    }
    try {
        auto j = nlohmann::json::parse(response);
        if (j.value("code", -1) != 0) {
            err = j.value("msg", response);
            return false;
        }
        out_token = j.value("tenant_access_token", "");
        if (out_token.empty()) {
            err = "no tenant_access_token";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool FeishuLongConn::ResolveWsUrl(const std::string& /*tenant_token*/, std::string& out_url,
                                  std::string& err) {
    const auto override_url = Env("FEISHU_WS_URL");
    if (!override_url.empty()) {
        out_url = override_url;
        return true;
    }
    // larksuite SDK: POST {domain}/callback/ws/endpoint {"AppID","AppSecret"} → {code:0, data:{URL, ClientConfig}}.
    CURL* curl = curl_easy_init();
    if (!curl) { err = "curl_easy_init failed"; return false; }
    std::string response;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "locale: zh");
    std::string base = Env("FEISHU_API_BASE");
    if (base.empty()) base = "https://open.feishu.cn";
    const std::string url = base + "/callback/ws/endpoint";
    const std::string body = nlohmann::json{{"AppID", Env("FEISHU_APP_ID")}, {"AppSecret", Env("FEISHU_APP_SECRET")}}.dump();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || http < 200 || http >= 300) {
        err = "WS endpoint discovery failed (http=" + std::to_string(http) + ")";
        return false;
    }
    try {
        auto j = nlohmann::json::parse(response);
        if (j.value("code", 0) != 0) { err = "endpoint: code " + std::to_string(j.value("code", 0)) + " " + j.value("msg", std::string()); return false; }
        auto data = j.contains("data") ? j["data"] : j;
        out_url = data.value("URL", data.value("url", std::string()));
        if (data.contains("ClientConfig") && data["ClientConfig"].is_object()) {
            std::lock_guard lock(mu_);
            const auto& cc = data["ClientConfig"];
            ping_interval_s_ = cc.value("PingInterval", ping_interval_s_);
            reconnect_count_ = cc.value("ReconnectCount", reconnect_count_);
            reconnect_interval_s_ = cc.value("ReconnectInterval", reconnect_interval_s_);
            reconnect_nonce_s_ = cc.value("ReconnectNonce", reconnect_nonce_s_);
        }
        if (out_url.empty()) { err = "no URL in endpoint response"; return false; }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

nlohmann::json FeishuLongConn::SendReply(const std::string& tenant_token, const std::string& chat_id, const std::string& text) {
    std::string base = Env("FEISHU_API_BASE");
    if (base.empty()) base = "https://open.feishu.cn";
    CURL* curl = curl_easy_init();
    std::string response;
    struct curl_slist* hdrs = nullptr;
    const std::string auth = "Authorization: Bearer " + tenant_token;
    hdrs = curl_slist_append(hdrs, auth.c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json; charset=utf-8");
    const std::string url = base + "/open-apis/im/v1/messages?receive_id_type=chat_id";
    const std::string body = nlohmann::json{{"receive_id", chat_id}, {"msg_type", "text"}, {"content", nlohmann::json{{"text", text}}.dump()}}.dump();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return {{"http", http}, {"ok", rc == CURLE_OK && http >= 200 && http < 300}};
}

nlohmann::json FeishuLongConn::HandleCardAction(const nlohmann::json& body, const DeliverFn& deliver) {
    // v2 (card.action.trigger): {schema,header,event:{operator:{open_id},action:{value,tag,option?,form_value?},context:{open_chat_id,open_message_id}}}
    // v1 (legacy card callback): {open_id, open_chat_id, action:{value,tag}}
    const nlohmann::json& ev = body.contains("event") && body["event"].is_object() ? body["event"] : body;
    nlohmann::json action = ev.value("action", nlohmann::json::object());
    std::string open_id = ev.contains("operator") ? ev["operator"].value("open_id", std::string()) : ev.value("open_id", std::string());
    std::string chat = ev.contains("context") ? ev["context"].value("open_chat_id", std::string()) : ev.value("open_chat_id", std::string());
    std::string text;
    if (action.contains("value")) {
        const auto& v = action["value"];
        if (v.is_string()) text = v.get<std::string>();
        else if (v.is_object() && v.contains("text") && v["text"].is_string()) text = v["text"].get<std::string>();
        else text = v.dump();
    }
    if (action.contains("option") && action["option"].is_string()) text += (text.empty() ? "" : " ") + action["option"].get<std::string>();
    if (action.contains("form_value")) text += (text.empty() ? "" : " ") + action["form_value"].dump();
    { std::lock_guard lock(mu_); ++card_callbacks_; }
    std::string reply;
    if (!text.empty() && deliver) {
        InboundMessage msg;
        msg.channel = "feishu";
        msg.sender = open_id.empty() ? "feishu" : open_id;
        msg.text = "[card action] " + text;
        msg.session_key = chat.empty() ? "feishu-default" : "feishu-" + chat;
        msg.conversation_id = chat;
        reply = deliver(msg);
        std::lock_guard lock(mu_);
        ++delivered_;
    }
    nlohmann::json resp{{"toast", {{"type", "info"}, {"content", reply.empty() ? std::string("received") : reply.substr(0, 200)}}}};
    if (!reply.empty())
        resp["card"] = {{"type", "raw"},
                        {"data", {{"schema", "2.0"}, {"body", {{"elements", nlohmann::json::array({{{"tag", "markdown"}, {"content", reply}}})}}}}}};
    return resp;
}

namespace {
bool PrivateHost(const std::string& host) {
    if (host.empty() || host == "localhost") return true;
    addrinfo* ai = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, nullptr, &ai) != 0 || !ai) return true;
    bool priv = false;
    for (auto* a = ai; a; a = a->ai_next) {
        if (a->ai_family == AF_INET) {
            const auto ip = ntohl(reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr.s_addr);
            const unsigned b0 = ip >> 24, b1 = (ip >> 16) & 0xff;
            if (b0 == 10 || b0 == 127 || b0 == 0 || (b0 == 169 && b1 == 254) || (b0 == 172 && b1 >= 16 && b1 < 32) || (b0 == 192 && b1 == 168) || (b0 == 100 && b1 >= 64 && b1 < 128)) priv = true;
        } else priv = true;  // be conservative with v6
    }
    freeaddrinfo(ai);
    return priv;
}
std::string HtmlTitle(const std::string& url) {
    CURL* c = curl_easy_init();
    std::string body;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char* p, size_t s, size_t n, void* u) -> size_t {
        auto* b = static_cast<std::string*>(u); if (b->size() > (256u << 10)) return 0; b->append(p, s * n); return s * n; });
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    std::string low = body; for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    auto a = low.find("<title"); if (a == std::string::npos) return {};
    a = low.find('>', a); auto b = low.find("</title>", a);
    if (a == std::string::npos || b == std::string::npos) return {};
    std::string t = body.substr(a + 1, b - a - 1);
    std::string out; bool sp = false;
    for (char ch : t) { if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') { if (!out.empty()) sp = true; continue; } if (sp) out.push_back(' '); sp = false; out.push_back(ch); }
    return out.substr(0, 120);
}
}  // namespace

nlohmann::json FeishuLongConn::HandleUrlPreview(const nlohmann::json& body) {
    const nlohmann::json& ev = body.contains("event") && body["event"].is_object() ? body["event"] : body;
    const auto ctx = ev.value("context", nlohmann::json::object());
    const std::string url = ctx.value("url", ev.value("url", std::string()));
    { std::lock_guard lock(mu_); ++url_previews_; }
    std::string host, path;
    if (auto s = url.find("://"); s != std::string::npos) {
        auto rest = url.substr(s + 3);
        auto slash = rest.find('/');
        host = rest.substr(0, slash);
        path = slash == std::string::npos ? "" : rest.substr(slash);
        if (auto at = host.rfind('@'); at != std::string::npos) host = host.substr(at + 1);
        if (auto colon = host.find(':'); colon != std::string::npos) host = host.substr(0, colon);
    }
    if (host.empty()) return {{"inline", {{"title", "link"}}}};
    std::string title = host + (path.size() > 1 ? " · " + path.substr(0, 60) : "");
    const char* f = std::getenv("FEISHU_LINK_PREVIEW_FETCH");
    bool fetched = false;
    if (f && std::string(f) == "1" && (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) &&
        (!PrivateHost(host) || std::getenv("OPENCLAW_MEDIA_ALLOW_PRIVATE"))) {
        auto t = HtmlTitle(url);
        if (!t.empty()) { title = t; fetched = true; }
    }
    nlohmann::json out{{"inline", {{"title", title}, {"i18n_title", {{"zh_cn", title}, {"en_us", title}}}}}};
    return out;
}

void FeishuLongConn::LiveLoop(DeliverFn deliver) {
    std::string token, err;
    auto set_err = [&](const std::string& m) { std::lock_guard lock(mu_); mode_ = "live-error"; last_error_ = m; };
    if (!FetchTenantToken(token, err)) {
        set_err(err);
        std::lock_guard lock(mu_);
        tenant_token_hint_ = "unset";
        running_ = false;
        return;
    }
    { std::lock_guard lock(mu_); tenant_token_hint_ = "set"; }
    namespace ff = feishu_frame;
    // larksuite SDK reconnect policy: first retry after rand(0, ReconnectNonce)s, then every ReconnectInterval s,
    // up to ReconnectCount attempts (-1 = forever). Server may update all three via endpoint ClientConfig / pong.
    int attempt = 0;
    std::mt19937 rng(std::random_device{}());
    auto sleep_interruptible = [&](int ms) {
        for (int w = 0; w < ms && !stop_.load(); w += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };
    while (!stop_.load()) {
        if (attempt > 0) {
            int count, interval, nonce;
            { std::lock_guard lock(mu_); count = reconnect_count_; interval = reconnect_interval_s_; nonce = reconnect_nonce_s_; mode_ = "reconnecting"; }
            if (count >= 0 && attempt > count) { set_err("reconnect attempts exhausted (" + std::to_string(count) + "): " + last_error_); break; }
            int ms = attempt == 1 ? std::uniform_int_distribution<int>(0, std::max(0, nonce) * 1000)(rng) : std::max(0, interval) * 1000;
            sleep_interruptible(ms);
            if (stop_.load()) break;
            std::lock_guard lock(mu_);
            ++reconnects_;
        }
        ++attempt;
        std::string ws_url;
        if (!ResolveWsUrl(token, ws_url, err)) { set_err(err); continue; }
        CURL* curl = curl_easy_init();
        if (!curl) { set_err("curl_easy_init failed"); continue; }
        curl_easy_setopt(curl, CURLOPT_URL, ws_url.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) { set_err(std::string("ws connect: ") + curl_easy_strerror(rc)); curl_easy_cleanup(curl); continue; }
        { std::lock_guard lock(mu_); mode_ = "live"; last_error_.clear(); }
        attempt = 1;  // connected: next drop restarts the backoff schedule (nonce first)

        auto send_json = [&](const nlohmann::json& j) {
            auto s = j.dump();
            size_t sent = 0;
            (void)curl_ws_send(curl, s.data(), s.size(), &sent, 0, CURLWS_TEXT);
        };
        std::int32_t service_id = 0;
        if (auto q = ws_url.find("service_id="); q != std::string::npos) service_id = std::atoi(ws_url.c_str() + q + 11);
        auto send_bin = [&](const ff::Frame& f) {
            auto s = ff::Encode(f);
            size_t sent = 0;
            (void)curl_ws_send(curl, s.data(), s.size(), &sent, 0, CURLWS_BINARY);
        };
        ff::Reassembler reasm;
        std::string bin_acc;
        auto next_ping = std::chrono::steady_clock::now();
        auto last_ping = next_ping;
        char buf[65536];
        while (!stop_.load()) {
            if (std::chrono::steady_clock::now() >= next_ping) {
                send_bin(ff::Ping(service_id));
                last_ping = std::chrono::steady_clock::now();
                int iv;
                { std::lock_guard lock(mu_); iv = ping_interval_s_; ++pings_; }
                next_ping = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, iv));
            }
            size_t nread = 0;
            const struct curl_ws_frame* meta = nullptr;
            rc = curl_ws_recv(curl, buf, sizeof(buf), &nread, &meta);
            if (rc == CURLE_AGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            if (rc != CURLE_OK) { std::lock_guard lock(mu_); last_error_ = std::string("ws recv: ") + curl_easy_strerror(rc); break; }
            if (meta && (meta->flags & CURLWS_CLOSE)) { std::lock_guard lock(mu_); last_error_ = "server closed connection"; break; }
            if (meta && (meta->flags & CURLWS_PING)) {
                size_t sent = 0;
                (void)curl_ws_send(curl, buf, nread, &sent, 0, CURLWS_PONG);
                continue;
            }
            if (meta && (meta->flags & CURLWS_BINARY)) {
                bin_acc.append(buf, nread);
                if (meta->bytesleft > 0) continue;
                auto fr = ff::Decode(bin_acc);
                bin_acc.clear();
                if (!fr) { std::lock_guard lock(mu_); last_error_ = "undecodable binary frame"; continue; }
                const auto type = fr->Header("type");
                if (fr->method == ff::kControl) {
                    if (type == "pong") {
                        std::lock_guard lock(mu_);
                        ++pongs_;
                        try {
                            auto cc = nlohmann::json::parse(fr->payload);
                            ping_interval_s_ = std::max(1, cc.value("PingInterval", ping_interval_s_));
                            reconnect_count_ = cc.value("ReconnectCount", reconnect_count_);
                            reconnect_interval_s_ = cc.value("ReconnectInterval", reconnect_interval_s_);
                            reconnect_nonce_s_ = cc.value("ReconnectNonce", reconnect_nonce_s_);
                            next_ping = last_ping + std::chrono::seconds(ping_interval_s_);  // server-pushed ClientConfig
                        } catch (...) {}
                    }
                    continue;
                }
                auto full = reasm.Add(*fr);
                if (!full) continue;
                const auto t0 = std::chrono::steady_clock::now();
                int code = 200;
                std::string data;
                try {
                    std::string raw = *full;
                    if (fr->payload_encoding == "gzip" || fr->Header("payload_encoding") == "gzip" || fr->Header("compress") == "gzip") {
                        auto z = ff::Gunzip(raw);
                        if (!z) throw std::runtime_error("gzip inflate failed");
                        raw = *z;
                        std::lock_guard lock(mu_);
                        ++gzip_frames_;
                    }
                    auto body = nlohmann::json::parse(raw);
                    std::string et;
                    if (body.contains("header") && body["header"].is_object()) et = body["header"].value("event_type", std::string());
                    if (et == "url.preview.get") {
                        data = HandleUrlPreview(body).dump();
                    } else if (type == "card" || et == "card.action.trigger") {
                        data = HandleCardAction(body, deliver).dump();
                    } else {
                        auto route = HandleEventBody(body, deliver);
                        { std::lock_guard lock(mu_); ++binary_events_; }
                        const auto reply = route.value("reply", std::string());
                        std::string chat;
                        if (body.contains("event") && body["event"].contains("message")) chat = body["event"]["message"].value("chat_id", std::string());
                        if (!reply.empty() && !chat.empty()) {
                            auto r = SendReply(token, chat, reply);
                            std::lock_guard lock(mu_);
                            if (r.value("ok", false)) ++replies_sent_; else last_error_ = "reply send failed: " + r.dump();
                        }
                    }
                } catch (const std::exception& e) { code = 500; std::lock_guard lock(mu_); last_error_ = std::string("frame: ") + e.what(); }
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                send_bin(ff::Response(*fr, code, ms, data));
                continue;
            }
            if (!meta || !(meta->flags & CURLWS_TEXT) || nread == 0) continue;
            try {
                auto payload = nlohmann::json::parse(std::string(buf, nread));
                auto handled = FeedPayload(payload, deliver);
                if (handled.contains("send") && handled["send"].is_array())
                    for (const auto& frame : handled["send"]) send_json(frame);
            } catch (...) {
                std::lock_guard lock(mu_);
                last_error_ = "non-JSON WS text frame";
            }
        }
        curl_easy_cleanup(curl);
    }
    running_ = false;
    std::lock_guard lock(mu_);
    if (mode_ == "live" || mode_ == "reconnecting") mode_ = "idle";
}

bool FeishuLongConn::Start(DeliverFn deliver) {
    if (running_.load()) return true;
    if (ForceMock()) {
        std::lock_guard lock(mu_);
        mode_ = "mock";
        last_error_ = "FORCE_MOCK — not connecting";
        return false;
    }
    if (Env("FEISHU_APP_ID").empty() || Env("FEISHU_APP_SECRET").empty()) {
        if (Env("FEISHU_WS_URL").empty()) {
            std::lock_guard lock(mu_);
            mode_ = "idle";
            last_error_ = "FEISHU_APP_ID/SECRET (and optional FEISHU_WS_URL) unset — use feed/mock";
            return false;
        }
    }
    stop_ = false;
    running_ = true;
    thr_ = std::thread([this, deliver = std::move(deliver)]() mutable { LiveLoop(std::move(deliver)); });
    return true;
}

void FeishuLongConn::Stop() {
    stop_ = true;
    if (thr_.joinable()) thr_.join();
    running_ = false;
}

}  // namespace openclaw::channels
