#include "openclaw/channels/slack_socket.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace openclaw::channels {
namespace {

std::string Env(const char* k) {
    if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}

bool ForceMock() {
    const auto v = Env("SLACK_SOCKET_FORCE_MOCK");
    if (v == "1" || v == "true" || v == "yes") return true;
    const auto c = Env("SLACK_SOCKET_FORCE_SIM");
    return c == "1" || c == "true" || c == "yes";
}

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

SlackSocketMode::SlackSocketMode() = default;

SlackSocketMode::~SlackSocketMode() { Stop(); }

nlohmann::json SlackSocketMode::Status() const {
    std::lock_guard lock(mu_);
    const auto app = Env("SLACK_APP_TOKEN");
    const auto bot = Env("SLACK_BOT_TOKEN");
    return {{"id", "slack-socket"},
            {"mode", mode_},
            {"running", running_.load()},
            {"envelopes", envelopes_},
            {"delivered", delivered_},
            {"acked", acked_},
            {"app_token_set", !app.empty()},
            {"bot_token_set", !bot.empty()},
            {"force_mock", ForceMock()},
            {"last_error", last_error_},
            {"impl", (mode_ == "live") ? "real" : (mode_ == "mock" ? "real-sim" : "honest")},
            {"gaps", nlohmann::json::array({"no interactive block actions UI",
                                            "no disconnect auto-reconnect backoff UI",
                                            "no app-level rate limit handling",
                                            "slash response_url optional"})}};
}

nlohmann::json SlackSocketMode::HandleEventsApi(const nlohmann::json& payload,
                                                const DeliverFn& deliver) {
    // Same shape as Events API HTTP body.
    const auto typ = payload.value("type", "");
    std::string text;
    std::string sender = "slack";
    std::string session = "slack-default";
    std::string conv, ctype, thread;
    if (typ == "event_callback" && payload.contains("event")) {
        const auto& ev = payload["event"];
        const auto et = ev.value("type", "");
        if (et == "message" || et == "app_mention") {
            // Ignore bot message loops when subtype=bot_message
            if (ev.value("subtype", "") == "bot_message") {
                return {{"ignored", true}, {"reason", "bot_message"}};
            }
            text = ev.value("text", "");
            sender = ev.value("user", sender);
            auto ch = ev.value("channel", "");
            if (!ch.empty()) session = "slack-" + ch;
            conv = ch; ctype = ev.value("channel_type", "");
            auto tts = ev.value("thread_ts", ""); if (!tts.empty() && tts != ev.value("ts", "")) thread = tts;
        }
    }
    if (text.empty()) text = payload.value("text", "");
    if (text.empty()) return {{"ignored", true}, {"reason", "no text"}};

    InboundMessage msg;
    msg.channel = "slack";
    msg.text = text;
    msg.sender = sender;
    msg.session_key = session;
    msg.chat_type = ctype == "im" ? "direct" : (ctype == "mpim" || ctype == "group") ? "group" : ctype == "channel" ? "channel" : "";
    msg.conversation_id = conv; msg.thread_id = thread;
    std::string reply = deliver ? deliver(msg) : "";
    {
        std::lock_guard lock(mu_);
        ++delivered_;
    }
    return {{"delivered", true}, {"session_key", session}, {"reply", reply}};
}

nlohmann::json SlackSocketMode::HandleSlash(const nlohmann::json& payload, const DeliverFn& deliver) {
    std::string text = payload.value("text", "");
    const auto cmd = payload.value("command", "");
    if (!cmd.empty() && !text.empty()) text = cmd + " " + text;
    else if (!cmd.empty()) text = cmd;
    if (text.empty()) return {{"ignored", true}, {"reason", "empty slash"}};

    InboundMessage msg;
    msg.channel = "slack";
    msg.text = text;
    msg.sender = payload.value("user_id", payload.value("user_name", "slack"));
    auto ch = payload.value("channel_id", "");
    msg.session_key = ch.empty() ? "slack-slash" : ("slack-" + ch);
    msg.conversation_id = ch;
    msg.chat_type = payload.value("channel_name", "") == "directmessage" ? "direct" : "channel";
    std::string reply = deliver ? deliver(msg) : "";
    {
        std::lock_guard lock(mu_);
        ++delivered_;
    }
    nlohmann::json out{{"delivered", true}, {"session_key", msg.session_key}, {"reply", reply}};
    // Optional ephemeral ack payload for accepts_response_payload
    if (!reply.empty()) {
        out["response_payload"] = {{"text", reply}};
    }
    return out;
}

nlohmann::json SlackSocketMode::FeedEnvelope(const nlohmann::json& envelope, const DeliverFn& deliver) {
    {
        std::lock_guard lock(mu_);
        ++envelopes_;
        if (mode_ == "idle") mode_ = "mock";
    }

    const auto typ = envelope.value("type", "");
    // Bare hello (sometimes without envelope_id)
    if (typ == "hello") {
        return nlohmann::json::object();  // no ack
    }

    const auto eid = envelope.value("envelope_id", "");
    nlohmann::json route;
    if (typ == "events_api") {
        route = HandleEventsApi(envelope.value("payload", nlohmann::json::object()), deliver);
    } else if (typ == "slash_commands") {
        route = HandleSlash(envelope.value("payload", nlohmann::json::object()), deliver);
    } else if (typ == "disconnect") {
        std::lock_guard lock(mu_);
        last_error_ = "server disconnect: " + envelope.value("reason", envelope.dump());
        return nlohmann::json::object();
    } else if (typ == "interactive") {
        route = {{"ignored", true}, {"reason", "interactive not fully handled"}};
    } else {
        route = {{"ignored", true}, {"reason", "unknown type"}, {"type", typ}};
    }

    nlohmann::json ack;
    if (!eid.empty()) {
        ack["envelope_id"] = eid;
        if (envelope.value("accepts_response_payload", false) && route.contains("response_payload")) {
            ack["payload"] = route["response_payload"];
        }
        std::lock_guard lock(mu_);
        ++acked_;
    }
    ack["_route"] = route;  // test/debug; stripped before live send
    return ack;
}

nlohmann::json SlackSocketMode::RunMockFrames(const std::vector<nlohmann::json>& frames,
                                              const DeliverFn& deliver) {
    {
        std::lock_guard lock(mu_);
        mode_ = "mock";
    }
    nlohmann::json acks = nlohmann::json::array();
    for (const auto& f : frames) {
        auto ack = FeedEnvelope(f, deliver);
        if (!ack.empty()) acks.push_back(ack);
    }
    return {{"ok", true},
            {"impl", "real-sim"},
            {"mode", "mock"},
            {"frames", frames.size()},
            {"acks", acks},
            {"status", Status()}};
}

bool SlackSocketMode::OpenConnectionUrl(std::string& out_url, std::string& err) {
    const auto app = Env("SLACK_APP_TOKEN");
    if (app.empty()) {
        err = "SLACK_APP_TOKEN unset";
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        err = "curl_easy_init failed";
        return false;
    }
    std::string response;
    struct curl_slist* hdrs = nullptr;
    const std::string auth = "Authorization: Bearer " + app;
    hdrs = curl_slist_append(hdrs, auth.c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_URL, "https://slack.com/api/apps.connections.open");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
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
        if (!j.value("ok", false)) {
            err = j.value("error", response);
            return false;
        }
        out_url = j.value("url", "");
        if (out_url.empty()) {
            err = "no url in connections.open";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

void SlackSocketMode::LiveLoop(DeliverFn deliver) {
    std::string url, err;
    if (!OpenConnectionUrl(url, err)) {
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
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);  // WebSocket
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

    char buf[65536];
    while (!stop_.load()) {
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
        if (!meta || !(meta->flags & CURLWS_TEXT) || nread == 0) continue;
        try {
            auto env = nlohmann::json::parse(std::string(buf, nread));
            auto ack = FeedEnvelope(env, deliver);
            if (ack.contains("envelope_id")) {
                nlohmann::json send = {{"envelope_id", ack["envelope_id"]}};
                if (ack.contains("payload")) send["payload"] = ack["payload"];
                auto s = send.dump();
                size_t sent = 0;
                (void)curl_ws_send(curl, s.data(), s.size(), &sent, 0, CURLWS_TEXT);
            }
        } catch (...) {
            // ignore bad frame
        }
    }
    curl_easy_cleanup(curl);
    running_ = false;
    std::lock_guard lock(mu_);
    if (mode_ == "live") mode_ = "idle";
}

bool SlackSocketMode::Start(DeliverFn deliver) {
    if (running_.load()) return true;
    if (ForceMock()) {
        std::lock_guard lock(mu_);
        mode_ = "mock";
        last_error_ = "FORCE_MOCK — not connecting";
        return false;
    }
    if (Env("SLACK_APP_TOKEN").empty()) {
        std::lock_guard lock(mu_);
        mode_ = "idle";
        last_error_ = "SLACK_APP_TOKEN unset — use FeedEnvelope/mock";
        return false;
    }
    stop_ = false;
    running_ = true;
    thr_ = std::thread([this, deliver = std::move(deliver)]() mutable { LiveLoop(std::move(deliver)); });
    return true;
}

void SlackSocketMode::Stop() {
    stop_ = true;
    if (thr_.joinable()) thr_.join();
    running_ = false;
}

}  // namespace openclaw::channels
