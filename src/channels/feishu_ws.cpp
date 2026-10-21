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
            {"app_id_set", !Env("FEISHU_APP_ID").empty()},
            {"app_secret_set", !Env("FEISHU_APP_SECRET").empty()},
            {"ws_url_override", !Env("FEISHU_WS_URL").empty()},
            {"tenant_token", tenant_token_hint_},
            {"force_mock", ForceMock()},
            {"last_error", last_error_},
            {"impl", (mode_ == "live") ? "real" : (mode_ == "mock" ? "real-sim" : "honest")},
            {"gaps", nlohmann::json::array(
                         {"card.action / url.preview callbacks are routed as events, not answered with card JSON",
                          "no automatic reconnect with ReconnectCount/ReconnectNonce backoff",
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
            ping_interval_s_ = data["ClientConfig"].value("PingInterval", ping_interval_s_);
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

void FeishuLongConn::LiveLoop(DeliverFn deliver) {
    std::string token, err;
    if (!FetchTenantToken(token, err)) {
        std::lock_guard lock(mu_);
        mode_ = "live-error";
        last_error_ = err;
        tenant_token_hint_ = "unset";
        running_ = false;
        return;
    }
    {
        std::lock_guard lock(mu_);
        tenant_token_hint_ = "set";
    }
    std::string ws_url;
    if (!ResolveWsUrl(token, ws_url, err)) {
        std::lock_guard lock(mu_);
        mode_ = "live-error";
        last_error_ = err;
        running_ = false;
        return;
    }
    {
        std::lock_guard lock(mu_);
        mode_ = "live";
        last_error_.clear();
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard lock(mu_);
        mode_ = "live-error";
        last_error_ = "curl_easy_init failed";
        running_ = false;
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, ws_url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        std::lock_guard lock(mu_);
        mode_ = "live-error";
        last_error_ = std::string("ws connect: ") + curl_easy_strerror(rc);
        curl_easy_cleanup(curl);
        running_ = false;
        return;
    }

    auto send_json = [&](const nlohmann::json& j) {
        auto s = j.dump();
        size_t sent = 0;
        (void)curl_ws_send(curl, s.data(), s.size(), &sent, 0, CURLWS_TEXT);
    };

    namespace ff = feishu_frame;
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
        if (rc == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (rc != CURLE_OK) {
            std::lock_guard lock(mu_);
            last_error_ = std::string("ws recv: ") + curl_easy_strerror(rc);
            break;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) break;
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
                        next_ping = last_ping + std::chrono::seconds(ping_interval_s_);  // server-pushed ClientConfig
                    } catch (...) {}
                }
                continue;
            }
            auto full = reasm.Add(*fr);
            if (!full) continue;
            const auto t0 = std::chrono::steady_clock::now();
            int code = 200;
            try {
                auto body = nlohmann::json::parse(*full);
                auto route = HandleEventBody(body, deliver);
                {
                    std::lock_guard lock(mu_);
                    ++binary_events_;
                }
                const auto reply = route.value("reply", std::string());
                std::string chat;
                if (body.contains("event") && body["event"].contains("message")) chat = body["event"]["message"].value("chat_id", std::string());
                if (!reply.empty() && !chat.empty()) {
                    auto r = SendReply(token, chat, reply);
                    std::lock_guard lock(mu_);
                    if (r.value("ok", false)) ++replies_sent_; else last_error_ = "reply send failed: " + r.dump();
                }
            } catch (...) { code = 500; }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            send_bin(ff::Response(*fr, code, ms));
            continue;
        }
        if (!meta || !(meta->flags & CURLWS_TEXT) || nread == 0) continue;
        try {
            auto payload = nlohmann::json::parse(std::string(buf, nread));
            auto handled = FeedPayload(payload, deliver);
            if (handled.contains("send") && handled["send"].is_array()) {
                for (const auto& frame : handled["send"]) send_json(frame);
            }
        } catch (...) {
            // Non-JSON / proprietary binary — count as gap
            std::lock_guard lock(mu_);
            last_error_ = "non-JSON WS frame (proprietary SDK framing?)";
        }
    }
    curl_easy_cleanup(curl);
    running_ = false;
    std::lock_guard lock(mu_);
    if (mode_ == "live") mode_ = "idle";
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
