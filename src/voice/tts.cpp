#include "openclaw/voice/tts.hpp"

#include <curl/curl.h>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include "openclaw/util.hpp"

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
    if (const char* p = std::getenv("OPENCLAW_TTS_PROVIDER"); p && *p) provider_ = p;
    // Provider-shaped clients: base URL defaults to the vendor API; key from vendor env when unset.
    if (provider_ == "openai") {
        if (api_key_.empty()) api_key_ = Env("OPENAI_API_KEY");
        if (url_.empty() && !api_key_.empty()) url_ = "https://api.openai.com";
    } else if (provider_ == "elevenlabs") {
        if (api_key_.empty()) api_key_ = Env("ELEVENLABS_API_KEY");
        if (url_.empty() && !api_key_.empty()) url_ = "https://api.elevenlabs.io";
    } else if (provider_ == "deepgram") {
        if (api_key_.empty()) api_key_ = Env("DEEPGRAM_API_KEY");
        if (url_.empty() && !api_key_.empty()) url_ = "https://api.deepgram.com";
    }
    enabled_ = !url_.empty();
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
        {{"id", "http"}, {"configured", provider_ == "http" && !url_.empty()}, {"env", "OPENCLAW_TTS_URL"}},
        {{"id", "openai"}, {"configured", provider_ == "openai" && !url_.empty()}, {"env", "OPENAI_API_KEY"}, {"endpoint", "/v1/audio/speech"}},
        {{"id", "elevenlabs"}, {"configured", provider_ == "elevenlabs" && !url_.empty()}, {"env", "ELEVENLABS_API_KEY"}, {"endpoint", "/v1/text-to-speech/{voice}"}},
        {{"id", "deepgram"}, {"configured", provider_ == "deepgram" && !url_.empty()}, {"env", "DEEPGRAM_API_KEY"}, {"endpoint", "/v1/speak"}},
        {{"id", "none"}, {"configured", true}, {"note", "record-only"}},
    });
}

void TtsEngine::SetProvider(const std::string& id) { provider_ = id.empty() ? "http" : id; }
void TtsEngine::SetPersona(const std::string& persona) { persona_ = persona.empty() ? "default" : persona; }
void TtsEngine::SetEnabled(bool on) { enabled_ = on; }

nlohmann::json TtsEngine::HttpPost(const std::string& text, const std::string& voice_in) const {
    CURL* curl = curl_easy_init();
    if (!curl) return {{"ok", false}, {"error", "curl_easy_init failed"}};
    const std::string voice = voice_in.empty() || voice_in == "default" ? std::string() : voice_in;
    std::string base = url_;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string url = base, payload, ctype;
    struct curl_slist* hdrs = curl_slist_append(nullptr, "Content-Type: application/json");
    auto add = [&](const std::string& h) { hdrs = curl_slist_append(hdrs, h.c_str()); };
    if (provider_ == "openai") {
        url = base + "/v1/audio/speech";
        payload = nlohmann::json{{"model", EnvOr("OPENCLAW_TTS_MODEL", "gpt-4o-mini-tts")}, {"input", text},
                                 {"voice", voice.empty() ? "alloy" : voice}, {"response_format", EnvOr("OPENCLAW_TTS_FORMAT", "mp3")}}.dump();
        if (!api_key_.empty()) add("Authorization: Bearer " + api_key_);
    } else if (provider_ == "elevenlabs") {
        url = base + "/v1/text-to-speech/" + (voice.empty() ? std::string("21m00Tcm4TlvDq8ikWAM") : voice);
        payload = nlohmann::json{{"text", text}, {"model_id", EnvOr("OPENCLAW_TTS_MODEL", "eleven_multilingual_v2")}}.dump();
        add("Accept: audio/mpeg");
        if (!api_key_.empty()) add("xi-api-key: " + api_key_);
    } else if (provider_ == "deepgram") {
        url = base + "/v1/speak?model=" + (voice.empty() ? EnvOr("OPENCLAW_TTS_MODEL", "aura-2-thalia-en") : voice);
        payload = nlohmann::json{{"text", text}}.dump();
        if (!api_key_.empty()) add("Authorization: Token " + api_key_);
    } else {
        payload = nlohmann::json{{"text", text}, {"voice", voice.empty() ? persona_ : voice}}.dump();
        if (!api_key_.empty()) { add("Authorization: Bearer " + api_key_); add("X-API-Key: " + api_key_); }
    }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    char* ct = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    if (ct) ctype = ct;
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    nlohmann::json out{{"curl_ok", rc == CURLE_OK}, {"http", http}, {"bytes", response.size()}, {"url", url},
                       {"content_type", ctype},
                       {"body_prefix", response.substr(0, std::min<size_t>(64, response.size()))},
                       {"error", rc == CURLE_OK ? "" : curl_easy_strerror(rc)}};
    if (http >= 400) out["error"] = "TTS HTTP " + std::to_string(http) + ": " + response.substr(0, 200);
    if (const char* d = std::getenv("OPENCLAW_TTS_OUT_DIR"); d && *d && http >= 200 && http < 300 && !response.empty()) {
        std::string ext = ctype.find("wav") != std::string::npos ? "wav" : ctype.find("ogg") != std::string::npos ? "ogg" : "mp3";
        std::string path = std::string(d) + "/tts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." + ext;
        if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(response.data(), 1, response.size(), f); std::fclose(f); out["path"] = path; }
    }
    return out;
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
