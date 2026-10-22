#include "openclaw/voice/tts.hpp"
#include "openclaw/config/config.hpp"
#include <curl/curl.h>
#include "openclaw/voice/inbound_audio.hpp"
#include "openclaw/channels/outbound.hpp"
#include "openclaw/channels/ext_channels.hpp"
#include "openclaw/channels/misc_channels.hpp"

#include <curl/curl.h>

#include <chrono>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <sstream>
#include <fstream>
#include <cstdlib>

namespace openclaw::channels {
namespace {
std::string TrimSlash(std::string s) { while (!s.empty() && s.back() == '/') s.pop_back(); return s; }
std::string TelegramBase(const nlohmann::json& cfg) {
    auto b = ChannelCred(cfg, "telegram", "apiBase", "TELEGRAM_API_BASE");
    return b.empty() ? "https://api.telegram.org" : TrimSlash(b);
}
std::string DiscordBase(const nlohmann::json& cfg) {
    auto b = ChannelCred(cfg, "discord", "apiBase", "DISCORD_API_BASE");
    return b.empty() ? "https://discord.com/api/v10" : TrimSlash(b);
}
// 1.135 Telegram media + inline keyboards (defined below, after the multipart helper).
nlohmann::json TelegramSendMedia(const std::string& api, const std::string& chat_id, const nlohmann::json& item, const nlohmann::json& extra);
nlohmann::json TelegramInlineKeyboard(const nlohmann::json& buttons);
// Upstream MEDIA directives (src/media/parse-output.ts): a line "MEDIA: <path|url>" (optionally backticked) is an
// attachment, removed from the visible text.
std::string ExtractMediaDirectives(const std::string& text, nlohmann::json* media) {
    std::istringstream in(text); std::string line, out; bool first = true;
    static const std::regex re(R"(^\s*MEDIA:\s*`?([^`\n]+?)`?\s*$)", std::regex::icase);
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, re)) { media->push_back(m[1].str()); continue; }
        if (!first) out += "\n"; out += line; first = false;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}
}  // namespace
namespace {

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string Env(const char* k) {
    if (const char* v = std::getenv(k); v && *v) return v;
    return {};
}

}  // namespace

OutboundRecordAdapter::OutboundRecordAdapter(std::string id, std::string todo_note,
                                             std::string webhook_path)
    : id_(std::move(id)), todo_(std::move(todo_note)), webhook_path_(std::move(webhook_path)) {}

nlohmann::json OutboundRecordAdapter::Status() const {
    bool token = false;
    if (id_ == "telegram") token = !ChannelCred(Config(), "telegram", "botToken", "TELEGRAM_BOT_TOKEN").empty();
    if (id_ == "discord") token = !ChannelCred(Config(), "discord", "token", "DISCORD_BOT_TOKEN").empty();
    if (id_ == "feishu") token = !ChannelCred(Config(), "feishu", "webhookUrl", "FEISHU_WEBHOOK_URL").empty();
    if (id_ == "slack") token = !ChannelCred(Config(), "slack", "botToken", "SLACK_BOT_TOKEN").empty();
    if (id_ == "whatsapp")
        token = !ChannelCred(Config(), "whatsapp", "token", "WHATSAPP_TOKEN").empty()
            || !ChannelCred(Config(), "whatsapp", "accessToken", "WHATSAPP_ACCESS_TOKEN").empty();
    return {{"id", id_},
            {"enabled", enabled_},
            {"type", "outbound"},
            {"todo", todo_},
            {"webhook_path", webhook_path_},
            {"token_present", token},
            {"sent_count", sent_.size()}};
}

nlohmann::json OutboundRecordAdapter::HttpPostJson(const std::string& url, const nlohmann::json& body,
                                                   const std::vector<std::string>& headers) const {
    return HttpJson("POST", url, body, headers);
}

nlohmann::json OutboundRecordAdapter::HttpJson(const std::string& method, const std::string& url,
                                               const nlohmann::json& body,
                                               const std::vector<std::string>& headers) const {
    CURL* curl = curl_easy_init();
    if (!curl) return {{"ok", false}, {"error", "curl_easy_init failed"}};
    std::string response;
    std::string payload = body.dump();
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    if (method != "POST") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    nlohmann::json out{{"curl_ok", rc == CURLE_OK}, {"http", http}, {"body", response}};
    if (rc != CURLE_OK) out["error"] = curl_easy_strerror(rc);
    try {
        out["json"] = nlohmann::json::parse(response);
    } catch (...) {
    }
    return out;
}

nlohmann::json OutboundRecordAdapter::SendText(const std::string& chat_id, const std::string& text,
                                               const nlohmann::json& extra) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    nlohmann::json entry{
        {"ts_ms", ms},
        {"channel", id_},
        {"chat_id", chat_id},
        {"text", text},
        {"enabled", enabled_},
        {"extra", extra},
        {"delivered", false},
        {"mode", "record_only"},
    };

    nlohmann::json http_result;
    if (id_ == "telegram") {
        const auto token = ChannelCred(Config(), "telegram", "botToken", "TELEGRAM_BOT_TOKEN");
        if (!token.empty() && !chat_id.empty()) {
            const std::string api = TelegramBase(Config()) + "/bot" + token + "/";
            const std::string url = api + "sendMessage";
            nlohmann::json media = nlohmann::json::array();
            if (extra.is_object() && extra.contains("media")) {
                if (extra["media"].is_array()) media = extra["media"]; else media.push_back(extra["media"]);
            }
            const std::string text_out = ExtractMediaDirectives(text, &media);
            const nlohmann::json keyboard = extra.is_object() && extra.contains("buttons") ? TelegramInlineKeyboard(extra["buttons"]) : nlohmann::json();
            // Bot API caps text at 4096 chars: split on paragraph/line/space boundaries (upstream chunker semantics).
            std::vector<std::string> chunks; std::string rest = text_out; const std::size_t lim = 4096;
            while (rest.size() > lim) {
                std::size_t cut = rest.rfind("\n\n", lim); if (cut == std::string::npos || cut < lim / 2) cut = rest.rfind('\n', lim);
                if (cut == std::string::npos || cut < lim / 2) cut = rest.rfind(' ', lim);
                if (cut == std::string::npos || cut == 0) { cut = lim; while (cut > 0 && (static_cast<unsigned char>(rest[cut]) & 0xC0) == 0x80) --cut; }
                chunks.push_back(rest.substr(0, cut)); rest = rest.substr(cut); while (!rest.empty() && (rest[0] == '\n' || rest[0] == ' ')) rest.erase(0, 1);
            }
            if (!rest.empty() || (chunks.empty() && media.empty())) chunks.push_back(rest);
            bool all_ok = true; nlohmann::json parts = nlohmann::json::array();
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                nlohmann::json b = {{"chat_id", chat_id}, {"text", chunks[i]}};
                if (i + 1 == chunks.size() && keyboard.is_object()) b["reply_markup"] = keyboard;
                if (extra.is_object() && extra.contains("message_thread_id")) b["message_thread_id"] = extra["message_thread_id"];
                if (i == 0 && extra.is_object() && extra.contains("reply_to_message_id")) b["reply_parameters"] = {{"message_id", extra["reply_to_message_id"]}, {"allow_sending_without_reply", true}};
                auto r = HttpPostJson(url, b, {});
                all_ok = all_ok && r.value("http", 0) >= 200 && r.value("http", 0) < 300; parts.push_back(r);
            }
            nlohmann::json media_sent = nlohmann::json::array();
            for (std::size_t i = 0; i < media.size(); ++i) {
                nlohmann::json mx = extra.is_object() ? extra : nlohmann::json::object();
                if (chunks.empty() && i + 1 == media.size() && keyboard.is_object()) mx["__keyboard"] = keyboard;  // media-only reply keeps its buttons
                auto r = TelegramSendMedia(api, chat_id, media[i], mx);
                all_ok = all_ok && r.value("http", 0) >= 200 && r.value("http", 0) < 300; media_sent.push_back(r);
            }
            http_result = parts.size() == 1 && media_sent.empty() ? parts[0] : nlohmann::json{{"http", all_ok ? 200 : 502}, {"chunks", parts}};
            if (!media_sent.empty()) { http_result["media"] = media_sent; entry["media"] = media.size(); }
            if (keyboard.is_object()) entry["buttons"] = keyboard["inline_keyboard"];
            entry["text"] = text_out;
            entry["mode"] = "live_http";
            entry["chunks"] = chunks.size();
            entry["delivered"] = all_ok;
            entry["http"] = http_result;
        }
    } else if (id_ == "discord") {
        const auto token = ChannelCred(Config(), "discord", "token", "DISCORD_BOT_TOKEN");
        if (!token.empty() && !chat_id.empty()) {
            const std::string url = DiscordBase(Config()) + "/channels/" + chat_id + "/messages";
            http_result = HttpPostJson(url, {{"content", text}},
                                       {"Authorization: Bot " + token});
            entry["mode"] = "live_http";
            entry["delivered"] = http_result.value("http", 0) >= 200 && http_result.value("http", 0) < 300;
            entry["http"] = http_result;
        }
    } else if (id_ == "feishu") {
        const auto hook = ChannelCred(Config(), "feishu", "webhookUrl", "FEISHU_WEBHOOK_URL");
        if (!hook.empty()) {
            http_result = HttpPostJson(hook, {{"msg_type", "text"}, {"content", {{"text", text}}}}, {});
            entry["mode"] = "live_http";
            entry["delivered"] = http_result.value("http", 0) >= 200 && http_result.value("http", 0) < 300;
            entry["http"] = http_result;
        }
    } else if (id_ == "slack") {
        const auto token = ChannelCred(Config(), "slack", "botToken", "SLACK_BOT_TOKEN");
        if (!token.empty() && !chat_id.empty()) {
            http_result = HttpPostJson("https://slack.com/api/chat.postMessage",
                                       [&] {
                                           nlohmann::json b{{"channel", chat_id}, {"text", text}};
                                           if (extra.is_object() && extra.contains("blocks")) b["blocks"] = extra["blocks"];
                                           if (extra.is_object() && extra.contains("thread_ts")) b["thread_ts"] = extra["thread_ts"];
                                           return b;
                                       }(),
                                       {"Authorization: Bearer " + token});
            entry["mode"] = "live_http";
            bool ok_http = http_result.value("http", 0) >= 200 && http_result.value("http", 0) < 300;
            bool ok_slack = true;
            if (http_result.contains("json") && http_result["json"].is_object()) {
                ok_slack = http_result["json"].value("ok", false);
            }
            entry["delivered"] = ok_http && ok_slack;
            entry["http"] = http_result;
        }
    } else if (id_ == "whatsapp") {
        auto token = ChannelCred(Config(), "whatsapp", "token", "WHATSAPP_TOKEN");
        if (token.empty()) token = ChannelCred(Config(), "whatsapp", "accessToken", "WHATSAPP_ACCESS_TOKEN");
        const auto phone_id = ChannelCred(Config(), "whatsapp", "phoneNumberId", "WHATSAPP_PHONE_NUMBER_ID");
        if (!token.empty() && !phone_id.empty() && !chat_id.empty()) {
            std::string base = ChannelCred(Config(), "whatsapp", "apiBase", "WHATSAPP_API_BASE");
            if (base.empty()) base = "https://graph.facebook.com";
            const std::string url = base + "/v21.0/" + phone_id + "/messages";
            nlohmann::json body{{"messaging_product", "whatsapp"},
                                {"to", chat_id},
                                {"type", "text"},
                                {"text", {{"body", text}}}};
            http_result = HttpPostJson(url, body, {"Authorization: Bearer " + token});
            entry["mode"] = "live_http";
            entry["delivered"] =
                http_result.value("http", 0) >= 200 && http_result.value("http", 0) < 300;
            entry["http"] = http_result;
        }
    } else if (IsExtChannel(id_)) {
        http_result = ExtChannelSend(id_, Config(), chat_id, text, extra);
        if (!http_result.is_null()) {
            entry["mode"] = "live_http";
            entry["delivered"] = http_result.value("http", 0) >= 200 && http_result.value("http", 0) < 300;
            entry["http"] = http_result;
        }
    } else if (IsMiscChannel(id_)) {
        http_result = MiscChannelSend(id_, Config(), chat_id, text, extra);
        if (!http_result.is_null()) {
            entry["mode"] = "live_http";
            entry["delivered"] = http_result.value("http", 0) >= 200 && http_result.value("http", 0) < 300;
            entry["http"] = http_result;
        }
    }

    sent_.push_back(entry);
    return {{"ok", true},
            {"recorded", true},
            {"delivered", entry.value("delivered", false)},
            {"mode", entry["mode"]},
            {"entry", entry},
            {"sent_count", sent_.size()}};
}

nlohmann::json OutboundRecordAdapter::SendTyping(const std::string& chat_id) {
    nlohmann::json entry{{"ts_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch()).count()},
                         {"channel", id_}, {"chat_id", chat_id}, {"type", "typing"}, {"mode", "record_only"}, {"delivered", false}};
    nlohmann::json http;
    if (id_ == "telegram") {
        const auto token = ChannelCred(Config(), "telegram", "botToken", "TELEGRAM_BOT_TOKEN");
        if (!token.empty() && !chat_id.empty())
            http = HttpPostJson(TelegramBase(Config()) + "/bot" + token + "/sendChatAction", {{"chat_id", chat_id}, {"action", "typing"}}, {});
    } else if (id_ == "discord") {
        const auto token = ChannelCred(Config(), "discord", "token", "DISCORD_BOT_TOKEN");
        if (!token.empty() && !chat_id.empty())
            http = HttpPostJson(DiscordBase(Config()) + "/channels/" + chat_id + "/typing", nlohmann::json::object(), {"Authorization: Bot " + token});
    }
    if (!http.is_null()) {
        entry["mode"] = "live_http";
        entry["http"] = http;
        entry["delivered"] = http.value("http", 0) >= 200 && http.value("http", 0) < 300;
    }
    {
        std::lock_guard l(typing_mu_);
        sent_.push_back(entry);
    }
    return entry;
}

namespace {
struct Part { std::string name, data, filename, mime; };
nlohmann::json Multipart(const std::string& url, const std::vector<std::string>& headers, const std::vector<Part>& parts,
                         const std::string& method = "POST") {
    CURL* c = curl_easy_init();
    curl_mime* form = curl_mime_init(c);
    for (const auto& p : parts) {
        auto* f = curl_mime_addpart(form);
        curl_mime_name(f, p.name.c_str());
        curl_mime_data(f, p.data.data(), p.data.size());
        if (!p.filename.empty()) curl_mime_filename(f, p.filename.c_str());
        if (!p.mime.empty()) curl_mime_type(f, p.mime.c_str());
    }
    curl_slist* h = nullptr;
    for (auto& x : headers) h = curl_slist_append(h, x.c_str());
    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_MIMEPOST, form);
    if (method != "POST") curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; });
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    long code = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_mime_free(form); curl_slist_free_all(h); curl_easy_cleanup(c);
    nlohmann::json out{{"http", code}};
    try { out["json"] = nlohmann::json::parse(resp); } catch (...) { out["body"] = resp.substr(0, 512); }
    return out;
}
nlohmann::json RawPost(const std::string& url, const std::string& data, const std::vector<std::string>& headers) {
    CURL* c = curl_easy_init();
    curl_slist* h = nullptr;
    for (auto& x : headers) h = curl_slist_append(h, x.c_str());
    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, data.data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(data.size()));
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char* p, size_t s, size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; });
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    long code = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h); curl_easy_cleanup(c);
    nlohmann::json out{{"http", code}};
    try { out["json"] = nlohmann::json::parse(resp); } catch (...) {}
    return out;
}
bool Ok2xx(const nlohmann::json& r) { auto c = r.value("http", 0L); return c >= 200 && c < 300; }
}  // namespace
namespace {
nlohmann::json TelegramInlineKeyboard(const nlohmann::json& buttons) {
    // buttons: [[btn, ...], ...] rows, or a flat [btn, ...] (one per row). btn: "label" (callback_data = label) or
    // {text, callback_data | data | value | url}. callback_data is capped at Telegram's 64 bytes.
    if (!buttons.is_array() || buttons.empty()) return {};
    nlohmann::json rows = nlohmann::json::array();
    auto btn = [](const nlohmann::json& b) -> nlohmann::json {
        if (b.is_string()) return {{"text", b}, {"callback_data", b.get<std::string>().substr(0, 64)}};
        if (!b.is_object()) return {};
        const auto text = b.value("text", b.value("label", std::string()));
        if (text.empty()) return {};
        if (b.contains("url") && b["url"].is_string()) return {{"text", text}, {"url", b["url"]}};
        std::string data = b.value("callback_data", b.value("data", b.value("value", text)));
        return {{"text", text}, {"callback_data", data.substr(0, 64)}};
    };
    for (auto& r : buttons) {
        nlohmann::json row = nlohmann::json::array();
        if (r.is_array()) { for (auto& b : r) { auto x = btn(b); if (!x.is_null()) row.push_back(x); } }
        else { auto x = btn(r); if (!x.is_null()) row.push_back(x); }
        if (!row.empty()) rows.push_back(row);
    }
    if (rows.empty()) return {};
    return {{"inline_keyboard", rows}};
}
nlohmann::json TelegramSendMedia(const std::string& api, const std::string& chat_id, const nlohmann::json& item, const nlohmann::json& extra) {
    std::string src = item.is_string() ? item.get<std::string>() : item.is_object() ? item.value("path", item.value("url", std::string())) : std::string();
    const std::string caption = item.is_object() ? item.value("caption", std::string()) : std::string();
    if (src.rfind("file://", 0) == 0) src = src.substr(7);
    if (!src.empty() && src[0] == '~') if (const char* h = std::getenv("HOME")) src = std::string(h) + src.substr(1);
    std::string ext = src.substr(src.find_last_of('.') == std::string::npos ? src.size() : src.find_last_of('.'));
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (const auto q = ext.find('?'); q != std::string::npos) ext.resize(q);
    std::string method = "sendDocument", field = "document", mime = "application/octet-stream";
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp") { method = "sendPhoto"; field = "photo"; mime = ext == ".png" ? "image/png" : ext == ".webp" ? "image/webp" : "image/jpeg"; }
    else if (ext == ".gif") { method = "sendAnimation"; field = "animation"; mime = "image/gif"; }
    else if (ext == ".mp4" || ext == ".mov" || ext == ".webm") { method = "sendVideo"; field = "video"; mime = "video/mp4"; }
    else if (ext == ".ogg" || ext == ".oga" || ext == ".opus") { method = "sendVoice"; field = "voice"; mime = "audio/ogg"; }
    else if (ext == ".mp3" || ext == ".m4a" || ext == ".wav" || ext == ".flac") { method = "sendAudio"; field = "audio"; mime = "audio/mpeg"; }
    if (item.is_object() && item.value("asDocument", false)) { method = "sendDocument"; field = "document"; }
    nlohmann::json out{{"method", method}, {"source", src}};
    if (src.empty()) { out["http"] = 0; out["error"] = "empty media source"; return out; }
    const bool remote = src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0;
    if (remote) {  // Telegram fetches URLs itself
        nlohmann::json b{{"chat_id", chat_id}, {field, src}};
        if (!caption.empty()) b["caption"] = caption.substr(0, 1024);
        if (extra.contains("__keyboard")) b["reply_markup"] = extra["__keyboard"];
        if (extra.contains("message_thread_id")) b["message_thread_id"] = extra["message_thread_id"];
        auto r = RawPost(api + method, b.dump(), {"Content-Type: application/json"});
        out["http"] = r.value("http", 0L); return out;
    }
    std::ifstream f(src, std::ios::binary);
    if (!f) { out["http"] = 0; out["error"] = "media file not found: " + src; return out; }
    std::string data((std::istreambuf_iterator<char>(f)), {});
    if (data.size() > 50u * 1024 * 1024) { out["http"] = 0; out["error"] = "media larger than Telegram's 50 MB bot upload limit"; return out; }
    std::vector<Part> parts{{"chat_id", chat_id, "", ""}, {field, data, std::filesystem::path(src).filename().string(), mime}};
    if (!caption.empty()) parts.push_back({"caption", caption.substr(0, 1024), "", ""});
    if (extra.contains("__keyboard")) parts.push_back({"reply_markup", extra["__keyboard"].dump(), "", ""});
    if (extra.contains("message_thread_id")) parts.push_back({"message_thread_id", extra["message_thread_id"].dump(), "", ""});
    auto r = Multipart(api + method, {}, parts);
    out["http"] = r.value("http", 0L); out["bytes"] = data.size();
    return out;
}
// Transcode to Ogg/Opus via ffmpeg (if installed). Returns empty on failure.
std::string ToOggOpus(const std::string& audio) {
    if (audio.rfind("OggS", 0) == 0) return audio;
    char in_t[] = "/tmp/oc-tts-XXXXXX";
    int fd = mkstemp(in_t);
    if (fd < 0) return {};
    { FILE* f = fdopen(fd, "wb"); fwrite(audio.data(), 1, audio.size(), f); fclose(f); }
    const std::string out_p = std::string(in_t) + ".ogg";
    const std::string cmd = "ffmpeg -nostdin -loglevel error -y -i '" + std::string(in_t) + "' -c:a libopus -b:a 32k -ar 48000 -ac 1 -f ogg '" + out_p + "' 2>/dev/null";
    std::string out;
    if (std::system(cmd.c_str()) == 0) {
        std::ifstream f(out_p, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(f), {});
    }
    std::remove(in_t); std::remove(out_p.c_str());
    return out.rfind("OggS", 0) == 0 ? out : std::string();
}
// Duration from the last Ogg page's granule position (Opus: 48 kHz).
double OggDurationSecs(const std::string& ogg) {
    auto pos = ogg.rfind("OggS");
    if (pos == std::string::npos || pos + 14 > ogg.size()) return 0;
    std::uint64_t g = 0;
    for (int i = 7; i >= 0; --i) g = (g << 8) | static_cast<unsigned char>(ogg[pos + 6 + i]);
    if (g == ~0ull) return 0;
    return static_cast<double>(g) / 48000.0;
}
std::string B64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; int val = 0, bits = -6;
    for (unsigned char c : in) { val = (val << 8) + c; bits += 8; while (bits >= 0) { o.push_back(k[(val >> bits) & 0x3F]); bits -= 6; } }
    if (bits > -6) o.push_back(k[((val << 8) >> (bits + 8)) & 0x3F]);
    while (o.size() % 4) o.push_back('=');
    return o;
}
// Coarse amplitude envelope (≤256 bytes) from the compressed stream — Discord only uses it for display.
std::string Waveform(const std::string& audio) {
    const size_t n = std::min<size_t>(64, std::max<size_t>(1, audio.size() / 64));
    std::string w;
    const size_t step = std::max<size_t>(1, audio.size() / n);
    for (size_t i = 0; i < n; ++i) {
        unsigned acc = 0, cnt = 0;
        for (size_t j = i * step; j < std::min(audio.size(), (i + 1) * step); j += 7) { acc += static_cast<unsigned char>(audio[j]); ++cnt; }
        w.push_back(static_cast<char>(cnt ? acc / cnt : 0));
    }
    return B64(w);
}
}  // namespace

nlohmann::json OutboundRecordAdapter::SendVoiceReply(const std::string& chat_id, const std::string& text) {
    voice::TtsEngine tts;
    std::string audio;
    std::size_t chunks = 0;
    auto r = tts.SpeakStream(text, {}, [&](const std::string& c) { audio += c; ++chunks; });
    nlohmann::json entry{{"channel", id_}, {"chat_id", chat_id}, {"type", "voice"}, {"tts", r}, {"chunks", chunks}, {"bytes", audio.size()}, {"delivered", false}, {"mode", "record_only"}};
    auto record = [&]() { std::lock_guard l(typing_mu_); sent_.push_back(entry); return entry; };
    if (audio.empty() || chat_id.empty()) return record();
    const auto& cfg = Config();
    if (id_ == "telegram") {
        const auto token = ChannelCred(cfg, "telegram", "botToken", "TELEGRAM_BOT_TOKEN");
        if (token.empty()) return record();
        const bool ogg = audio.rfind("OggS", 0) == 0;
        const std::string method = ogg ? "sendVoice" : "sendAudio";
        auto h = Multipart(TelegramBase(cfg) + "/bot" + token + "/" + method, {},
                           {{"chat_id", chat_id, "", ""}, {ogg ? "voice" : "audio", audio, ogg ? "reply.ogg" : "reply.mp3", ogg ? "audio/ogg" : "audio/mpeg"}});
        entry["mode"] = "live_http"; entry["method"] = method; entry["http"] = h.value("http", 0L); entry["delivered"] = Ok2xx(h);
        return record();
    }
    const std::string ogg = ToOggOpus(audio);
    entry["format"] = ogg.empty() ? "source" : "ogg_opus";
    if (!ogg.empty() && audio.rfind("OggS", 0) != 0) entry["transcoded"] = "ffmpeg";
    const bool is_ogg = !ogg.empty();
    const std::string& data = is_ogg ? ogg : audio;
    const double dur = is_ogg ? OggDurationSecs(ogg) : 0;
    if (id_ == "discord") {
        const auto token = ChannelCred(cfg, "discord", "token", "DISCORD_BOT_TOKEN");
        if (token.empty()) return record();
        nlohmann::json att{{"id", "0"}, {"filename", is_ogg ? "voice-message.ogg" : "reply.mp3"}};
        nlohmann::json payload{{"attachments", nlohmann::json::array()}};
        if (is_ogg) { att["duration_secs"] = std::max(0.1, dur); att["waveform"] = Waveform(ogg); payload["flags"] = 8192; }  // IS_VOICE_MESSAGE
        payload["attachments"].push_back(att);
        auto h = Multipart(DiscordBase(cfg) + "/channels/" + chat_id + "/messages", {"Authorization: Bot " + token},
                           {{"payload_json", payload.dump(), "", "application/json"},
                            {"files[0]", data, att["filename"].get<std::string>(), is_ogg ? "audio/ogg" : "audio/mpeg"}});
        entry["mode"] = "live_http"; entry["method"] = is_ogg ? "voice_message" : "attachment"; entry["http"] = h.value("http", 0L); entry["delivered"] = Ok2xx(h);
        if (!Ok2xx(h)) entry["error"] = h;
        return record();
    }
    if (id_ == "slack") {
        const auto token = ChannelCred(cfg, "slack", "botToken", "SLACK_BOT_TOKEN");
        if (token.empty()) return record();
        auto base = ChannelCred(cfg, "slack", "apiBase", "SLACK_API_BASE");
        if (base.empty()) base = "https://slack.com/api";
        const std::string fname = is_ogg ? "reply.ogg" : "reply.mp3";
        const std::vector<std::string> auth{"Authorization: Bearer " + token};
        auto u = RawPost(base + "/files.getUploadURLExternal", "filename=" + fname + "&length=" + std::to_string(data.size()),
                         {auth[0], "Content-Type: application/x-www-form-urlencoded"});
        const auto uj = u.value("json", nlohmann::json::object());
        if (!uj.value("ok", false)) { entry["mode"] = "live_http"; entry["error"] = u; return record(); }
        auto up = RawPost(uj.value("upload_url", std::string()), data, {"Content-Type: application/octet-stream"});
        auto done = RawPost(base + "/files.completeUploadExternal",
                            nlohmann::json{{"files", nlohmann::json::array({{{"id", uj.value("file_id", std::string())}, {"title", "Voice reply"}}})}, {"channel_id", chat_id}}.dump(),
                            {auth[0], "Content-Type: application/json; charset=utf-8"});
        entry["mode"] = "live_http"; entry["method"] = "files.uploadV2"; entry["file_id"] = uj.value("file_id", std::string());
        entry["delivered"] = Ok2xx(up) && done.value("json", nlohmann::json::object()).value("ok", false);
        return record();
    }
    if (id_ == "feishu") {
        const auto app = ChannelCred(cfg, "feishu", "appId", "FEISHU_APP_ID"), sec = ChannelCred(cfg, "feishu", "appSecret", "FEISHU_APP_SECRET");
        if (app.empty() || sec.empty()) return record();
        if (!is_ogg) { entry["error"] = "feishu audio messages need opus (ffmpeg unavailable)"; return record(); }
        auto base = ChannelCred(cfg, "feishu", "apiBase", "FEISHU_API_BASE");
        if (base.empty()) base = "https://open.feishu.cn";
        auto tk = RawPost(base + "/open-apis/auth/v3/tenant_access_token/internal", nlohmann::json{{"app_id", app}, {"app_secret", sec}}.dump(), {"Content-Type: application/json"});
        const auto token = tk.value("json", nlohmann::json::object()).value("tenant_access_token", std::string());
        entry["mode"] = "live_http";
        if (token.empty()) { entry["error"] = "tenant token"; return record(); }
        auto f = Multipart(base + "/open-apis/im/v1/files", {"Authorization: Bearer " + token},
                           {{"file_type", "opus", "", ""}, {"file_name", "reply.opus", "", ""},
                            {"duration", std::to_string(static_cast<long long>(dur * 1000)), "", ""}, {"file", ogg, "reply.opus", "audio/ogg"}});
        std::string key;
        try { key = f["json"]["data"].value("file_key", std::string()); } catch (...) {}
        if (key.empty()) { entry["error"] = f; return record(); }
        auto m = RawPost(base + "/open-apis/im/v1/messages?receive_id_type=chat_id",
                         nlohmann::json{{"receive_id", chat_id}, {"msg_type", "audio"}, {"content", nlohmann::json{{"file_key", key}}.dump()}}.dump(),
                         {"Authorization: Bearer " + token, "Content-Type: application/json; charset=utf-8"});
        entry["method"] = "im.audio"; entry["file_key"] = key; entry["delivered"] = Ok2xx(m) && m.value("json", nlohmann::json::object()).value("code", -1) == 0;
        return record();
    }
    if (id_ == "whatsapp") {
        auto token = ChannelCred(cfg, "whatsapp", "token", "WHATSAPP_TOKEN");
        if (token.empty()) token = ChannelCred(cfg, "whatsapp", "accessToken", "WHATSAPP_ACCESS_TOKEN");
        const auto phone = ChannelCred(cfg, "whatsapp", "phoneNumberId", "WHATSAPP_PHONE_NUMBER_ID");
        if (token.empty() || phone.empty()) return record();
        std::string base = ChannelCred(cfg, "whatsapp", "apiBase", "WHATSAPP_API_BASE");
        if (base.empty()) base = "https://graph.facebook.com";
        const std::string mime = is_ogg ? "audio/ogg" : "audio/mpeg";
        auto up = Multipart(base + "/v21.0/" + phone + "/media", {"Authorization: Bearer " + token},
                            {{"messaging_product", "whatsapp", "", ""}, {"type", mime, "", ""}, {"file", data, is_ogg ? "reply.ogg" : "reply.mp3", mime}});
        const auto mid = up.value("json", nlohmann::json::object()).value("id", std::string());
        entry["mode"] = "live_http";
        if (mid.empty()) { entry["error"] = up; return record(); }
        nlohmann::json audio_obj{{"id", mid}};
        if (is_ogg) audio_obj["voice"] = true;
        auto m = RawPost(base + "/v21.0/" + phone + "/messages",
                         nlohmann::json{{"messaging_product", "whatsapp"}, {"to", chat_id}, {"type", "audio"}, {"audio", audio_obj}}.dump(),
                         {"Authorization: Bearer " + token, "Content-Type: application/json"});
        entry["method"] = "media+audio"; entry["media_id"] = mid; entry["delivered"] = Ok2xx(m);
        return record();
    }
    entry["error"] = "voice replies not supported on channel " + id_;
    return record();
}

nlohmann::json OutboundRecordAdapter::MaybeAutoVoice(const std::string& chat_id, const std::string& reply, bool inbound_had_audio) {
    if (reply.empty() || chat_id.empty()) return nullptr;
    auto mode = config::GetPath(Config(), "messages.tts.auto");
    std::string am = mode && mode->is_string() ? mode->get<std::string>() : std::string("off");
    if (const char* e = std::getenv("OPENCLAW_TTS_AUTO"); e && *e) am = e;
    if (am == "always" || (am == "inbound" && inbound_had_audio)) return SendVoiceReply(chat_id, reply);
    return nullptr;
}

nlohmann::json OutboundRecordAdapter::OutboundLog() const { return sent_; }
void OutboundRecordAdapter::ClearLog() { sent_.clear(); }

nlohmann::json OutboundRecordAdapter::HandleHttpInbound(const nlohmann::json& body, const DeliverFn& deliver) {
    if (IsExtChannel(id_))
        return ExtChannelInbound(id_, Config(), body, deliver, [this](const std::string& to, const std::string& t, const nlohmann::json& ex) { return SendText(to, t, ex); });
    if (IsMiscChannel(id_))
        return MiscChannelInbound(id_, Config(), body, deliver, [this](const std::string& to, const std::string& t, const nlohmann::json& ex) { return SendText(to, t, ex); });
    if (id_ != "telegram") return {{"ok", false}, {"error", "unsupported"}};
    // Secret is verified by the gateway HTTP layer from X-Telegram-Bot-Api-Secret-Token (1.79).
    // 1.135: inline-keyboard presses (callback_query) arrive as a user message carrying the button's callback_data;
    // the query is acknowledged with answerCallbackQuery so the client stops its spinner.
    nlohmann::json cbq = body.contains("callback_query") && body["callback_query"].is_object() ? body["callback_query"] : nlohmann::json();
    nlohmann::json synth;
    if (cbq.is_object()) {
        const auto token = ChannelCred(Config(), "telegram", "botToken", "TELEGRAM_BOT_TOKEN");
        if (!token.empty() && cbq.contains("id"))
            HttpPostJson(TelegramBase(Config()) + "/bot" + token + "/answerCallbackQuery", {{"callback_query_id", cbq["id"]}}, {});
        if (!cbq.contains("message") || !cbq["message"].is_object() || !cbq.contains("data")) return {{"ok", true}, {"ignored", true}, {"note", "callback without message/data"}};
        synth = cbq["message"];
        synth["text"] = cbq["data"];
        synth.erase("caption"); synth.erase("photo"); synth.erase("document");
        if (cbq.contains("from")) synth["from"] = cbq["from"];
        synth["__callback"] = true;
    }
    const auto& tm = synth.is_object() ? synth : body.contains("message") ? body["message"] : (body.contains("edited_message") ? body["edited_message"] : nlohmann::json());
    if (!tm.is_object() || !tm.contains("chat")) return {{"ok", true}, {"ignored", true}, {"note", "no message"}};
    InboundMessage m;
    m.channel = "telegram";
    m.text = tm.value("text", tm.value("caption", std::string()));
    m.media = synth.is_object() ? nlohmann::json::array() : voice::ExtractAudioRefs(body);
    // 1.135: photos (largest size), image/video/other documents, videos, animations and stickers become media refs
    // ({kind, telegram_file_id, mime, filename}); the gateway downloads them (getFile) before the turn.
    auto add_ref = [&](const char* kind, const nlohmann::json& o, const std::string& mime, const std::string& fname) {
        if (!o.is_object() || !o.contains("file_id")) return;
        m.media.push_back({{"kind", kind}, {"telegram_file_id", o["file_id"]}, {"mime", o.value("mime_type", mime)}, {"filename", o.value("file_name", fname)},
                           {"size", o.value("file_size", 0)}});
    };
    if (tm.contains("photo") && tm["photo"].is_array() && !tm["photo"].empty()) add_ref("image", tm["photo"].back(), "image/jpeg", "photo.jpg");
    if (tm.contains("document") && tm["document"].is_object()) {
        const auto mt = tm["document"].value("mime_type", std::string());
        add_ref(mt.rfind("image/", 0) == 0 ? "image" : mt.rfind("video/", 0) == 0 ? "video" : "document", tm["document"], "application/octet-stream", "file");
    }
    if (tm.contains("video")) add_ref("video", tm["video"], "video/mp4", "video.mp4");
    if (tm.contains("animation")) add_ref("video", tm["animation"], "video/mp4", "animation.mp4");
    if (tm.contains("sticker") && !tm["sticker"].value("is_animated", false) && !tm["sticker"].value("is_video", false)) add_ref("image", tm["sticker"], "image/webp", "sticker.webp");
    if (m.text.empty() && m.media.empty()) return {{"ok", true}, {"ignored", true}, {"note", "no text or media"}};
    const auto& chat = tm["chat"];
    const std::string cid = chat["id"].is_string() ? chat["id"].get<std::string>() : chat["id"].dump();
    m.sender = tm.contains("from") ? (tm["from"]["id"].is_string() ? tm["from"]["id"].get<std::string>() : tm["from"]["id"].dump()) : cid;
    const auto type = chat.value("type", std::string("private"));
    m.chat_type = type == "private" ? "direct" : "group";
    if (m.chat_type == "group") m.conversation_id = cid;
    m.peer_id = m.sender;
    m.session_key = "telegram-" + cid;
    if (tm.value("is_topic_message", false) && tm.contains("message_thread_id")) m.thread_id = tm["message_thread_id"].dump();
    auto reply = deliver ? deliver(m) : std::string();
    nlohmann::json sent;
    if (!reply.empty()) {
        nlohmann::json ex = nlohmann::json::object();
        if (!m.thread_id.empty()) ex["message_thread_id"] = tm["message_thread_id"];
        if (m.chat_type == "group" && tm.contains("message_id")) ex["reply_to_message_id"] = tm["message_id"];
        sent = SendText(cid, reply, ex);
    }
    nlohmann::json voice_sent = MaybeAutoVoice(cid, reply, !m.media.empty());
    return {{"ok", true}, {"channel", "telegram"}, {"reply", reply}, {"sent", sent}, {"voice", voice_sent}, {"impl", "real"}};
}

}  // namespace openclaw::channels
