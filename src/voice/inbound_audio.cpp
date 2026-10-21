#include "openclaw/voice/inbound_audio.hpp"

#include "openclaw/channels/outbound.hpp"
#include "openclaw/net/safe_fetch.hpp"
#include "openclaw/voice/stt.hpp"

#include <curl/curl.h>

#include <cstdlib>

namespace openclaw::voice {
namespace {
std::string Env(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }
size_t W(char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }
bool Get(const std::string& url, const std::string& auth, std::string& body, std::string* ctype = nullptr, std::size_t cap = 25u << 20) {
    CURL* c = curl_easy_init();
    body.clear();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, W);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(cap));
    curl_slist* h = nullptr;
    if (!auth.empty()) h = curl_slist_append(h, ("Authorization: " + auth).c_str());
    if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    auto rc = curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    char* ct = nullptr; curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
    if (ctype && ct) *ctype = ct;
    if (h) curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return rc == CURLE_OK && code / 100 == 2 && body.size() <= cap;
}
std::string Trim(std::string s) { while (!s.empty() && s.back() == '/') s.pop_back(); return s; }
}  // namespace

nlohmann::json ExtractAudioRefs(const nlohmann::json& b) {
    nlohmann::json out = nlohmann::json::array();
    try {
        if (b.contains("media") && b["media"].is_array())
            for (auto& m : b["media"]) if (m.is_object() && m.value("mime", m.value("kind", std::string())).find("audio") != std::string::npos) out.push_back(m);
        if (b.contains("message") && b["message"].is_object()) {
            const auto& tm = b["message"];
            for (const char* k : {"voice", "audio"})
                if (tm.contains(k) && tm[k].contains("file_id"))
                    out.push_back({{"kind", "audio"}, {"mime", tm[k].value("mime_type", std::string("audio/ogg"))}, {"telegram_file_id", tm[k]["file_id"]},
                                   {"filename", std::string(k) == "voice" ? "voice.ogg" : tm[k].value("file_name", std::string("audio"))}});
        }
        if (b.contains("entry")) {
            const auto& v = b["entry"][0]["changes"][0]["value"];
            if (v.contains("messages") && v["messages"][0].contains("audio"))
                out.push_back({{"kind", "audio"}, {"mime", v["messages"][0]["audio"].value("mime_type", std::string("audio/ogg"))},
                               {"whatsapp_media_id", v["messages"][0]["audio"]["id"]}, {"filename", "voice.ogg"}});
        }
        if (b.contains("attachments") && b["attachments"].is_array())
            for (auto& a : b["attachments"])
                if (a.value("content_type", std::string()).rfind("audio/", 0) == 0)
                    out.push_back({{"kind", "audio"}, {"mime", a["content_type"]}, {"url", a.value("url", std::string())}, {"filename", a.value("filename", std::string("voice.ogg"))}});
    } catch (...) {}
    return out;
}

nlohmann::json TranscribeInboundAudio(const nlohmann::json& cfg, channels::InboundMessage& m) {
    nlohmann::json info = nlohmann::json::array();
    if (!m.media.is_array() || m.media.empty()) return info;
    if (Env("OPENCLAW_AUDIO_TRANSCRIBE") == "0") return info;
    try { if (cfg.contains("tools") && cfg["tools"]["media"]["audio"].value("enabled", true) == false) return info; } catch (...) {}
    std::string transcript;
    for (auto& ref : m.media) {
        if (!ref.is_object() || ref.value("kind", std::string("audio")) != "audio") continue;
        std::string bytes, err, ctype;
        const std::string mime = ref.value("mime", std::string("audio/ogg"));
        if (ref.contains("telegram_file_id")) {
            auto base = channels::ChannelCred(cfg, "telegram", "apiBase", "TELEGRAM_API_BASE");
            base = base.empty() ? "https://api.telegram.org" : Trim(base);
            const auto tok = channels::ChannelCred(cfg, "telegram", "botToken", "TELEGRAM_BOT_TOKEN");
            std::string meta;
            if (tok.empty()) err = "no telegram token";
            else if (!Get(base + "/bot" + tok + "/getFile?file_id=" + ref["telegram_file_id"].get<std::string>(), "", meta)) err = "getFile failed";
            else {
                try {
                    auto fp = nlohmann::json::parse(meta)["result"].value("file_path", std::string());
                    if (fp.empty() || fp.find("..") != std::string::npos) err = "bad file_path";
                    else if (!Get(base + "/file/bot" + tok + "/" + fp, "", bytes)) err = "telegram download failed";
                } catch (...) { err = "bad getFile json"; }
            }
        } else if (ref.contains("whatsapp_media_id")) {
            auto base = channels::ChannelCred(cfg, "whatsapp", "apiBase", "WHATSAPP_API_BASE");
            base = base.empty() ? "https://graph.facebook.com" : Trim(base);
            auto tok = channels::ChannelCred(cfg, "whatsapp", "token", "WHATSAPP_TOKEN");
            if (tok.empty()) tok = channels::ChannelCred(cfg, "whatsapp", "accessToken", "WHATSAPP_ACCESS_TOKEN");
            std::string meta;
            if (tok.empty()) err = "no whatsapp token";
            else if (!Get(base + "/v21.0/" + ref["whatsapp_media_id"].get<std::string>(), "Bearer " + tok, meta)) err = "media lookup failed";
            else {
                try {
                    auto u = nlohmann::json::parse(meta).value("url", std::string());
                    if (u.empty() || !Get(u, "Bearer " + tok, bytes)) err = "whatsapp download failed";
                } catch (...) { err = "bad media json"; }
            }
        } else if (ref.contains("url")) {
            auto r = net::SafeFetch(ref["url"].get<std::string>(), 25u << 20, Env("OPENCLAW_MEDIA_ALLOW_PRIVATE") == "1");
            if (!r.ok) err = "fetch: " + r.error; else bytes = r.body;
        } else err = "unresolvable media ref";
        nlohmann::json one{{"mime", mime}};
        if (!err.empty()) { one["error"] = err; info.push_back(one); continue; }
        auto tr = DefaultStt().Transcribe(bytes, mime, ref.value("filename", std::string("voice.ogg")), "");
        if (!tr.value("ok", false) && tr.value("text", std::string()).empty()) { one["error"] = tr.value("error", std::string("stt failed")); info.push_back(one); continue; }
        one["bytes"] = bytes.size();
        one["transcript"] = tr.value("text", std::string());
        info.push_back(one);
        if (!transcript.empty()) transcript += "\n";
        transcript += tr.value("text", std::string());
    }
    if (!transcript.empty()) m.text = "[Audio] Transcript: " + transcript + (m.text.empty() ? "" : "\n\n" + m.text);
    return info;
}

}  // namespace openclaw::voice
