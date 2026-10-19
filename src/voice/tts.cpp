#include "openclaw/voice/tts.hpp"

#include <curl/curl.h>
#include <chrono>
#include <cstdlib>

namespace openclaw::voice {
namespace {
size_t WriteCb(char* p, size_t s, size_t n, void* u) {
    auto* o = static_cast<std::string*>(u);
    o->append(p, s * n);
    return s * n;
}
std::string Env(const char* k) {
    if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}
}  // namespace

TtsEngine::TtsEngine() {
    url_ = Env("OPENCLAW_TTS_URL");
    api_key_ = Env("OPENCLAW_TTS_API_KEY");
    enabled_ = !url_.empty();
    if (const char* p = std::getenv("OPENCLAW_TTS_PROVIDER"); p && *p) provider_ = p;
}

nlohmann::json TtsEngine::Status() const {
    return {{"enabled", enabled_},
            {"provider", provider_},
            {"persona", persona_},
            {"url_configured", !url_.empty()},
            {"api_key_set", !api_key_.empty()},
            {"mode", url_.empty() ? "record_only" : "http"},
            {"impl", "real"},
            {"speak_count", log_.size()}};
}

nlohmann::json TtsEngine::Providers() const {
    return nlohmann::json::array({
        {{"id", "http"}, {"configured", !url_.empty()}, {"env", "OPENCLAW_TTS_URL"}},
        {{"id", "none"}, {"configured", true}, {"note", "record-only"}},
    });
}

void TtsEngine::SetProvider(const std::string& id) { provider_ = id.empty() ? "http" : id; }
void TtsEngine::SetPersona(const std::string& persona) { persona_ = persona.empty() ? "default" : persona; }
void TtsEngine::SetEnabled(bool on) { enabled_ = on; }

nlohmann::json TtsEngine::HttpPost(const std::string& text, const std::string& voice) const {
    CURL* curl = curl_easy_init();
    if (!curl) return {{"ok", false}, {"error", "curl_easy_init failed"}};
    std::string response;
    nlohmann::json body{{"text", text}, {"voice", voice.empty() ? persona_ : voice}};
    std::string payload = body.dump();
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    if (!api_key_.empty()) {
        auto h = "Authorization: Bearer " + api_key_;
        hdrs = curl_slist_append(hdrs, h.c_str());
        auto h2 = "X-API-Key: " + api_key_;
        hdrs = curl_slist_append(hdrs, h2.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return {{"curl_ok", rc == CURLE_OK},
            {"http", http},
            {"bytes", response.size()},
            {"body_prefix", response.substr(0, std::min<size_t>(64, response.size()))},
            {"error", rc == CURLE_OK ? "" : curl_easy_strerror(rc)}};
}

nlohmann::json TtsEngine::Speak(const std::string& text, const std::string& voice) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    nlohmann::json entry{{"ts_ms", ms},
                         {"text", text},
                         {"voice", voice.empty() ? persona_ : voice},
                         {"provider", provider_},
                         {"enabled", enabled_},
                         {"mode", "record_only"},
                         {"delivered", false}};
    if (enabled_ && !url_.empty() && provider_ != "none") {
        auto http = HttpPost(text, voice);
        entry["mode"] = "live_http";
        entry["http"] = http;
        entry["delivered"] = http.value("curl_ok", false) && http.value("http", 0) >= 200 &&
                             http.value("http", 0) < 300;
    }
    log_.push_back(entry);
    if (log_.size() > 100) log_.erase(log_.begin());
    return {{"ok", true},
            {"impl", "real"},
            {"mode", entry["mode"]},
            {"delivered", entry["delivered"]},
            {"entry", entry},
            {"note", url_.empty() ? "OPENCLAW_TTS_URL unset — record_only" : ""}};
}

nlohmann::json TtsEngine::Convert(const std::string& text) { return Speak(text); }

}  // namespace openclaw::voice
