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
        const std::string url = EnvOr("OPENCLAW_STT_URL", "");
        if (url.empty()) return {{"ok", false}, {"error", "no STT provider configured (OPENCLAW_STT_URL)"}};
        if (audio.empty()) return {{"ok", false}, {"error", "empty audio"}};
        CURL* c = curl_easy_init();
        curl_mime* form = curl_mime_init(c);
        auto* f = curl_mime_addpart(form);
        curl_mime_name(f, "file");
        curl_mime_data(f, audio.data(), audio.size());
        curl_mime_filename(f, filename.empty() ? "audio.ogg" : filename.c_str());
        if (!mime.empty()) curl_mime_type(f, mime.c_str());
        const std::string model = EnvOr("OPENCLAW_STT_MODEL", "whisper-1");
        auto* m = curl_mime_addpart(form);
        curl_mime_name(m, "model");
        curl_mime_data(m, model.c_str(), CURL_ZERO_TERMINATED);
        if (!language.empty()) {
            auto* l = curl_mime_addpart(form);
            curl_mime_name(l, "language");
            curl_mime_data(l, language.c_str(), CURL_ZERO_TERMINATED);
        }
        curl_slist* h = nullptr;
        const std::string key = EnvOr("OPENCLAW_STT_API_KEY", "");
        if (!key.empty()) h = curl_slist_append(h, ("Authorization: Bearer " + key).c_str());
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
        if (j.is_object() && j.contains("text")) return {{"ok", true}, {"text", j["text"]}, {"model", model}};
        return {{"ok", true}, {"text", resp}, {"model", model}};  // response_format=text servers
    }
};
}  // namespace

SttEngine& DefaultStt() { static HttpStt s; return s; }

}  // namespace openclaw::voice
