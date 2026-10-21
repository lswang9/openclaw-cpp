#include "openclaw/voice/stt.hpp"

#include "openclaw/util.hpp"

#include <curl/curl.h>

namespace openclaw::voice {
namespace {
size_t Sink(char* p, size_t s, size_t n, void* ud) { static_cast<std::string*>(ud)->append(p, s * n); return s * n; }

class HttpStt : public SttEngine {
public:
    nlohmann::json Transcribe(const std::string& audio, const std::string& mime, const std::string& filename,
                              const std::string& language) override {
        const std::string prov = EnvOr("OPENCLAW_STT_PROVIDER", "openai");
        std::string url = EnvOr("OPENCLAW_STT_URL", "");
        std::string key = EnvOr("OPENCLAW_STT_API_KEY", "");
        auto vendor = [&](const char* kenv, const char* host) {
            if (key.empty()) key = EnvOr(kenv, "");
            if (url.empty() && !key.empty()) url = host;
        };
        if (prov == "deepgram") vendor("DEEPGRAM_API_KEY", "https://api.deepgram.com");
        else if (prov == "elevenlabs") vendor("ELEVENLABS_API_KEY", "https://api.elevenlabs.io");
        else if (prov == "openai" && url.empty()) { vendor("OPENAI_API_KEY", "https://api.openai.com"); if (!url.empty()) url += "/v1/audio/transcriptions"; }
        if (url.empty()) return {{"ok", false}, {"error", "no STT provider configured (OPENCLAW_STT_URL or vendor key)"}};
        if (audio.empty()) return {{"ok", false}, {"error", "empty audio"}};
        while (url.back() == '/') url.pop_back();
        if (prov == "deepgram") return Deepgram(url, key, audio, mime, language);
        if (prov == "elevenlabs" && url.find("/speech-to-text") == std::string::npos) url += "/v1/speech-to-text";
        CURL* c = curl_easy_init();
        curl_mime* form = curl_mime_init(c);
        auto* f = curl_mime_addpart(form);
        curl_mime_name(f, "file");
        curl_mime_data(f, audio.data(), audio.size());
        curl_mime_filename(f, filename.empty() ? "audio.ogg" : filename.c_str());
        if (!mime.empty()) curl_mime_type(f, mime.c_str());
        const std::string model = EnvOr("OPENCLAW_STT_MODEL", prov == "elevenlabs" ? "scribe_v1" : "whisper-1");
        auto* m = curl_mime_addpart(form);
        curl_mime_name(m, prov == "elevenlabs" ? "model_id" : "model");
        curl_mime_data(m, model.c_str(), CURL_ZERO_TERMINATED);
        if (!language.empty()) {
            auto* l = curl_mime_addpart(form);
            curl_mime_name(l, prov == "elevenlabs" ? "language_code" : "language");
            curl_mime_data(l, language.c_str(), CURL_ZERO_TERMINATED);
        }
        curl_slist* h = nullptr;
        if (!key.empty()) h = curl_slist_append(h, (prov == "elevenlabs" ? "xi-api-key: " + key : "Authorization: Bearer " + key).c_str());
        std::string resp;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_MIMEPOST, form);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
        auto rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_mime_free(form);
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK) return {{"ok", false}, {"error", curl_easy_strerror(rc)}};
        if (code != 200) return {{"ok", false}, {"error", "STT HTTP " + std::to_string(code)}, {"body", resp.substr(0, 300)}};
        auto j = nlohmann::json::parse(resp, nullptr, false);
        if (j.is_object() && j.contains("text")) return {{"ok", true}, {"text", j["text"]}, {"model", model}, {"provider", prov}};
        return {{"ok", true}, {"text", resp}, {"model", model}, {"provider", prov}};  // response_format=text servers
    }

    // Deepgram pre-recorded: raw audio body, Authorization: Token, results.channels[0].alternatives[0].transcript
    static nlohmann::json Deepgram(std::string url, const std::string& key, const std::string& audio, const std::string& mime,
                                   const std::string& language) {
        const std::string model = EnvOr("OPENCLAW_STT_MODEL", "nova-3");
        if (url.find("/listen") == std::string::npos) url += "/v1/listen";
        url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("model=") + model + "&smart_format=true";
        if (!language.empty()) url += "&language=" + language;
        CURL* c = curl_easy_init();
        curl_slist* h = curl_slist_append(nullptr, ("Content-Type: " + (mime.empty() ? std::string("audio/ogg") : mime)).c_str());
        if (!key.empty()) h = curl_slist_append(h, ("Authorization: Token " + key).c_str());
        std::string resp;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, audio.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(audio.size()));
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
        auto rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK) return {{"ok", false}, {"error", curl_easy_strerror(rc)}};
        if (code != 200) return {{"ok", false}, {"error", "STT HTTP " + std::to_string(code)}, {"body", resp.substr(0, 300)}};
        auto j = nlohmann::json::parse(resp, nullptr, false);
        try {
            return {{"ok", true}, {"text", j.at("results").at("channels").at(0).at("alternatives").at(0).at("transcript")},
                    {"model", model}, {"provider", "deepgram"}};
        } catch (...) { return {{"ok", false}, {"error", "deepgram: unexpected response"}, {"body", resp.substr(0, 300)}}; }
    }
};
}  // namespace

SttEngine& DefaultStt() { static HttpStt s; return s; }

}  // namespace openclaw::voice
