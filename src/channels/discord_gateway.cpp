#include "openclaw/voice/inbound_audio.hpp"
#include "openclaw/channels/discord_gateway.hpp"

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
    const auto v = Env("DISCORD_GATEWAY_FORCE_MOCK");
    if (v == "1" || v == "true" || v == "yes") return true;
    const auto c = Env("DISCORD_GATEWAY_FORCE_SIM");
    return c == "1" || c == "true" || c == "yes";
}

// GUILDS | GUILD_MESSAGES | MESSAGE_CONTENT | DIRECT_MESSAGES
constexpr int kDefaultIntents = 1 | 128 | 512 | 32768 | 4096;  // + GUILD_VOICE_STATES

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

DiscordGateway::DiscordGateway() = default;
DiscordGateway::~DiscordGateway() { Stop(); }

nlohmann::json DiscordGateway::Status() const {
    std::lock_guard lock(mu_);
    return {{"id", "discord-gateway"},
            {"mode", mode_},
            {"running", running_.load()},
            {"payloads", payloads_},
            {"delivered", delivered_},
            {"heartbeats", heartbeats_},
            {"heartbeat_interval_ms", heartbeat_interval_ms_},
            {"last_seq", last_seq_ ? nlohmann::json(*last_seq_) : nlohmann::json(nullptr)},
            {"session_id", session_id_},
            {"bot_token_set", !Env("DISCORD_BOT_TOKEN").empty()},
            {"force_mock", ForceMock()},
            {"last_error", last_error_},
            {"impl", (mode_ == "live") ? "real" : (mode_ == "mock" ? "real-sim" : "honest")},
            {"gaps", nlohmann::json::array({"no slash command / interaction full handling",
                                            "no voice / stage",
                                            "no guild member / presence intents by default",
                                            "no resume/reconnect backoff UI",
                                            "MESSAGE_CONTENT privileged intent required for content"})}};
}

nlohmann::json DiscordGateway::MakeIdentify() const {
    const auto token = Env("DISCORD_BOT_TOKEN");
    int intents = kDefaultIntents;
    if (const char* e = std::getenv("DISCORD_GATEWAY_INTENTS"); e && *e) intents = std::atoi(e);
    return {{"op", 2},
            {"d",
             {{"token", token.empty() ? "mock-token" : token},
              {"intents", intents},
              {"properties",
               {{"os", "linux"}, {"browser", "openclaw-cpp"}, {"device", "openclaw-cpp"}}}}}};
}

nlohmann::json DiscordGateway::MakeHeartbeat() const {
    std::lock_guard lock(mu_);
    return {{"op", 1}, {"d", last_seq_ ? nlohmann::json(*last_seq_) : nlohmann::json(nullptr)}};
}

nlohmann::json DiscordGateway::HandleDispatch(const std::string& event, const nlohmann::json& data,
                                              const DeliverFn& deliver) {
    if (event == "READY") {
        std::lock_guard lock(mu_);
        session_id_ = data.value("session_id", "");
        if (data.contains("user") && data["user"].is_object()) bot_user_id_ = data["user"].value("id", "");
        return {{"ready", true}, {"session_id", session_id_}};
    }
    if (event == "VOICE_STATE_UPDATE" || event == "VOICE_SERVER_UPDATE") {
        std::lock_guard lock(mu_);
        if (event == "VOICE_SERVER_UPDATE") voice_server_ = data;
        else if (data.value("user_id", "") == bot_user_id_) voice_state_ = data;
        voice_cv_.notify_all();
        return {{"voice", event}};
    }
    if (event == "INTERACTION_CREATE") {
        if (!interaction_) return {{"ignored", true}, {"reason", "no interaction handler"}};
        auto r = interaction_(data, deliver);
        std::lock_guard lock(mu_);
        ++delivered_;
        return r;
    }
    if (event == "MESSAGE_CREATE") {
        // Ignore bots (including self)
        if (data.contains("author") && data["author"].value("bot", false)) {
            return {{"ignored", true}, {"reason", "bot_author"}};
        }
        const auto content = data.value("content", "");
        auto audio = voice::ExtractAudioRefs(data);
        if (content.empty() && audio.empty()) return {{"ignored", true}, {"reason", "empty content"}};
        InboundMessage msg;
        msg.channel = "discord";
        msg.text = content;
        msg.media = audio;
        msg.sender = data.contains("author") ? data["author"].value("id", "discord") : "discord";
        auto chan = data.value("channel_id", "");
        msg.session_key = chan.empty() ? "discord-default" : ("discord-" + chan);
        msg.conversation_id = chan;
        msg.chat_type = data.contains("guild_id") && !data["guild_id"].is_null() ? "channel" : "direct";
        if (data.contains("guild_id") && data["guild_id"].is_string()) msg.guild_id = data["guild_id"];
        std::string reply = deliver ? deliver(msg) : "";
        {
            std::lock_guard lock(mu_);
            ++delivered_;
        }
        return {{"delivered", true},
                {"session_key", msg.session_key},
                {"reply", reply},
                {"channel_id", chan}};
    }
    return {{"ignored", true}, {"event", event}};
}

nlohmann::json DiscordGateway::FeedPayload(const nlohmann::json& payload, const DeliverFn& deliver) {
    {
        std::lock_guard lock(mu_);
        ++payloads_;
        if (mode_ == "idle") mode_ = "mock";
        if (payload.contains("s") && !payload["s"].is_null()) {
            last_seq_ = payload["s"].get<int>();
        }
    }

    const int op = payload.value("op", -1);
    nlohmann::json out{{"op", op}};

    if (op == 10) {  // Hello
        int interval = 45000;
        if (payload.contains("d") && payload["d"].is_object()) {
            interval = payload["d"].value("heartbeat_interval", interval);
        }
        {
            std::lock_guard lock(mu_);
            heartbeat_interval_ms_ = interval;
        }
        // After Hello, client should Identify
        out["send"] = nlohmann::json::array({MakeIdentify()});
        out["hello"] = true;
        return out;
    }
    if (op == 11) {  // Heartbeat ACK
        std::lock_guard lock(mu_);
        ++heartbeats_;
        out["heartbeat_ack"] = true;
        return out;
    }
    if (op == 1) {  // Heartbeat request from server (rare) — respond
        out["send"] = nlohmann::json::array({MakeHeartbeat()});
        return out;
    }
    if (op == 7) {  // Reconnect
        std::lock_guard lock(mu_);
        last_error_ = "gateway requested reconnect";
        out["reconnect"] = true;
        return out;
    }
    if (op == 9) {  // Invalid session
        std::lock_guard lock(mu_);
        last_error_ = "invalid session";
        out["invalid_session"] = true;
        return out;
    }
    if (op == 0) {  // Dispatch
        const auto t = payload.value("t", "");
        auto route = HandleDispatch(t, payload.value("d", nlohmann::json::object()), deliver);
        out["t"] = t;
        out["route"] = route;
        return out;
    }
    out["ignored"] = true;
    return out;
}

nlohmann::json DiscordGateway::RunMockFrames(const std::vector<nlohmann::json>& frames,
                                             const DeliverFn& deliver) {
    {
        std::lock_guard lock(mu_);
        mode_ = "mock";
    }
    nlohmann::json results = nlohmann::json::array();
    for (const auto& f : frames) {
        results.push_back(FeedPayload(f, deliver));
    }
    return {{"ok", true},
            {"impl", "real-sim"},
            {"mode", "mock"},
            {"frames", frames.size()},
            {"results", results},
            {"status", Status()}};
}

bool DiscordGateway::ResolveGatewayUrl(std::string& out_url, std::string& err) {
    const auto token = Env("DISCORD_BOT_TOKEN");
    if (token.empty()) {
        err = "DISCORD_BOT_TOKEN unset";
        return false;
    }
    if (const char* u = std::getenv("DISCORD_GATEWAY_URL"); u && *u) {  // tests / proxies
        out_url = u;
        return true;
    }
    // Prefer bot gateway URL
    CURL* curl = curl_easy_init();
    if (!curl) {
        err = "curl_easy_init failed";
        return false;
    }
    std::string response;
    struct curl_slist* hdrs = nullptr;
    const std::string auth = "Authorization: Bot " + token;
    hdrs = curl_slist_append(hdrs, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, "https://discord.com/api/v10/gateway/bot");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    const CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        // Fallback default
        out_url = "wss://gateway.discord.gg/?v=10&encoding=json";
        err = std::string("gateway/bot failed: ") + curl_easy_strerror(rc) + " — using default";
        return true;
    }
    try {
        auto j = nlohmann::json::parse(response);
        auto url = j.value("url", "");
        if (url.empty()) {
            out_url = "wss://gateway.discord.gg/?v=10&encoding=json";
            return true;
        }
        if (url.find('?') == std::string::npos) url += "/?v=10&encoding=json";
        else if (url.find("v=") == std::string::npos) url += "&v=10&encoding=json";
        out_url = url;
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        out_url = "wss://gateway.discord.gg/?v=10&encoding=json";
        return true;
    }
}

void DiscordGateway::LiveLoop(DeliverFn deliver) {
    std::string url, err;
    if (!ResolveGatewayUrl(url, err)) {
        std::lock_guard lock(mu_);
        mode_ = "live-error";
        last_error_ = err;
        running_ = false;
        return;
    }
    {
        std::lock_guard lock(mu_);
        mode_ = "live";
        if (!err.empty()) last_error_ = err;
        else last_error_.clear();
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

    char buf[65536];
    auto next_hb = std::chrono::steady_clock::now() + std::chrono::hours(24);
    bool identified = false;

    while (!stop_.load()) {
        {
            std::vector<nlohmann::json> q;
            { std::lock_guard l(mu_); q.swap(outq_); }
            for (auto& f : q) send_json(f);
        }
        // Heartbeat timer
        if (identified && heartbeat_interval_ms_ > 0 &&
            std::chrono::steady_clock::now() >= next_hb) {
            send_json(MakeHeartbeat());
            {
                std::lock_guard lock(mu_);
                ++heartbeats_;
            }
            next_hb = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(std::max(1000, heartbeat_interval_ms_));
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
        if (!meta || !(meta->flags & CURLWS_TEXT) || nread == 0) continue;
        try {
            auto payload = nlohmann::json::parse(std::string(buf, nread));
            auto handled = FeedPayload(payload, deliver);
            if (handled.contains("send") && handled["send"].is_array()) {
                for (const auto& frame : handled["send"]) {
                    send_json(frame);
                    if (frame.value("op", -1) == 2) identified = true;
                }
            }
            if (handled.value("hello", false) && heartbeat_interval_ms_ > 0) {
                // Jitter: wait interval before first heartbeat (Discord recommends)
                next_hb = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(heartbeat_interval_ms_);
            }
            if (handled.value("op", -1) == 0 && handled.value("t", "") == "READY") {
                identified = true;
            }
        } catch (...) {
        }
    }
    curl_easy_cleanup(curl);
    running_ = false;
    std::lock_guard lock(mu_);
    if (mode_ == "live") mode_ = "idle";
}

nlohmann::json DiscordGateway::JoinVoice(const std::string& guild_id, const std::string& channel_id, int timeout_ms) {
    std::unique_lock l(mu_);
    if (!running_.load() || mode_ != "live") return {{"ok", false}, {"error", "discord gateway not connected (live)"}, {"mode", mode_}};
    voice_state_ = nullptr; voice_server_ = nullptr;
    outq_.push_back({{"op", 4}, {"d", {{"guild_id", guild_id}, {"channel_id", channel_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(channel_id)},
                                       {"self_mute", false}, {"self_deaf", false}}}});
    if (channel_id.empty()) return {{"ok", true}, {"left", true}, {"guild_id", guild_id}};
    const bool got = voice_cv_.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] {
        return voice_state_.is_object() && voice_server_.is_object() && !voice_server_.value("endpoint", std::string()).empty();
    });
    if (!got) return {{"ok", false}, {"error", "timed out waiting for VOICE_STATE_UPDATE + VOICE_SERVER_UPDATE"}};
    std::string ep = voice_server_.value("endpoint", std::string());
    if (ep.find("://") == std::string::npos) {
        const char* ins = std::getenv("DISCORD_VOICE_INSECURE_WS");  // tests only: fake voice server speaks ws://
        ep = std::string(ins && *ins == '1' ? "ws://" : "wss://") + ep;
    }
    ep += ep.find('?') == std::string::npos ? "/?v=8" : "";
    return {{"ok", true}, {"endpoint", ep}, {"token", voice_server_.value("token", "")}, {"session_id", voice_state_.value("session_id", "")},
            {"user_id", bot_user_id_}, {"guild_id", guild_id}, {"channel_id", channel_id}};
}

bool DiscordGateway::Start(DeliverFn deliver) {
    if (running_.load()) return true;
    if (ForceMock()) {
        std::lock_guard lock(mu_);
        mode_ = "mock";
        last_error_ = "FORCE_MOCK — not connecting";
        return false;
    }
    if (Env("DISCORD_BOT_TOKEN").empty()) {
        std::lock_guard lock(mu_);
        mode_ = "idle";
        last_error_ = "DISCORD_BOT_TOKEN unset — use FeedPayload/mock";
        return false;
    }
    stop_ = false;
    running_ = true;
    thr_ = std::thread([this, deliver = std::move(deliver)]() mutable { LiveLoop(std::move(deliver)); });
    return true;
}

void DiscordGateway::Stop() {
    stop_ = true;
    if (thr_.joinable()) thr_.join();
    running_ = false;
}

}  // namespace openclaw::channels
