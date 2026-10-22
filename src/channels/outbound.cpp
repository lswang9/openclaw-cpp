#include <map>
#include "openclaw/voice/tts.hpp"
#include "openclaw/config/config.hpp"
#include <curl/curl.h>
#include "openclaw/voice/inbound_audio.hpp"
#include "openclaw/channels/outbound.hpp"
#include "openclaw/persist/state_db.hpp"
#include <mutex>
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

// 1.157: business connections (chat id → business_connection_id) so replies go out through the connection.
static std::mutex g_biz_mu;
static std::map<std::string, std::string>& BizConn() { static std::map<std::string, std::string> m; return m; }
static std::string BizFor(const std::string& chat) { std::lock_guard<std::mutex> l(g_biz_mu); auto it = BizConn().find(chat); return it == BizConn().end() ? "" : it->second; }
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
                if (extra.is_object() && extra.contains("business_connection_id")) b["business_connection_id"] = extra["business_connection_id"];
                else if (auto bz = BizFor(chat_id); !bz.empty()) b["business_connection_id"] = bz;
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


// 1.146: public-poll routes so inbound poll_answer (which lacks chat/thread) can rejoin the right session.
namespace {
std::mutex g_poll_mu;
nlohmann::json& PollRegistry() {
    static nlohmann::json reg = nlohmann::json::object();
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        const char* d = std::getenv("OPENCLAW_DATA_DIR");
        std::filesystem::path path = (d && *d) ? std::filesystem::path(d) / "telegram" / "poll-registry.json"
                                              : std::filesystem::path("/tmp/openclaw-cpp-data/telegram/poll-registry.json");
        if (auto t = persist::StateRead(path)) try { auto j = nlohmann::json::parse(*t); if (j.is_object()) reg = j; } catch (...) {}
    }
    return reg;
}
void PersistPollRegistry() {
    const char* d = std::getenv("OPENCLAW_DATA_DIR");
    std::filesystem::path path = (d && *d) ? std::filesystem::path(d) / "telegram" / "poll-registry.json"
                                          : std::filesystem::path("/tmp/openclaw-cpp-data/telegram/poll-registry.json");
    persist::StateWrite(path, PollRegistry().dump(2));
}
}  // namespace

// 1.166: forum topic icon cache (chatId:threadId → {name, icon_color, icon_custom_emoji_id}).
static std::mutex g_topic_mu;
nlohmann::json& TopicIconCache() {
    static nlohmann::json cache = nlohmann::json::object();
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        const char* d = std::getenv("OPENCLAW_DATA_DIR");
        std::filesystem::path path = (d && *d) ? std::filesystem::path(d) / "telegram" / "topic-icons.json"
                                              : std::filesystem::path("/tmp/openclaw-cpp-data/telegram/topic-icons.json");
        if (auto t = persist::StateRead(path)) try { auto j = nlohmann::json::parse(*t); if (j.is_object()) cache = j; } catch (...) {}
    }
    return cache;
}
void PersistTopicIcons() {
    const char* d = std::getenv("OPENCLAW_DATA_DIR");
    std::filesystem::path path = (d && *d) ? std::filesystem::path(d) / "telegram" / "topic-icons.json"
                                          : std::filesystem::path("/tmp/openclaw-cpp-data/telegram/topic-icons.json");
    persist::StateWrite(path, TopicIconCache().dump(2));
}
void RememberTopicIcon(const std::string& chat, const std::string& thread, const nlohmann::json& info) {
    std::lock_guard<std::mutex> l(g_topic_mu);
    TopicIconCache()[chat + ":" + thread] = info;
    PersistTopicIcons();
}
void OutboundRecordAdapter::TelegramRememberPoll(const nlohmann::json& entry) {
    std::lock_guard<std::mutex> l(g_poll_mu);
    PollRegistry()[entry.value("pollId", "")] = entry;
    PersistPollRegistry();
}
static void TelegramRetirePoll(const std::string& poll_id) {
    std::lock_guard<std::mutex> l(g_poll_mu);
    if (PollRegistry().erase(poll_id)) PersistPollRegistry();
}
nlohmann::json OutboundRecordAdapter::TelegramLookupPoll(const std::string& poll_id) {
    std::lock_guard<std::mutex> l(g_poll_mu);
    auto& reg = PollRegistry();
    return reg.contains(poll_id) ? reg[poll_id] : nlohmann::json();
}

nlohmann::json OutboundRecordAdapter::TelegramAction(const std::string& action, const nlohmann::json& p) {
    const auto token = ChannelCred(Config(), "telegram", "botToken", "TELEGRAM_BOT_TOKEN");
    if (token.empty()) return {{"ok", false}, {"error", "telegram botToken not configured"}};
    const std::string api = TelegramBase(Config()) + "/bot" + token + "/";
    auto idv = [](const nlohmann::json& v) -> nlohmann::json {
        if (v.is_string()) { const auto s = v.get<std::string>(); if (!s.empty() && (std::isdigit((unsigned char)s[0]) || s[0] == '-') && s.find_first_not_of("-0123456789") == std::string::npos) return std::stoll(s); }
        return v; };
    nlohmann::json chat = p.contains("chat_id") ? p["chat_id"] : p.contains("to") ? p["to"] : p.value("target", nlohmann::json());
    nlohmann::json mid = p.contains("message_id") ? p["message_id"] : p.value("messageId", nlohmann::json());
    nlohmann::json thread = p.contains("message_thread_id") ? p["message_thread_id"] : p.value("threadId", nlohmann::json());
    if (chat.is_null() || chat == "") return {{"ok", false}, {"error", "to (chat id) is required"}};
    nlohmann::json body{{"chat_id", chat.is_string() ? chat : nlohmann::json(chat.dump())}};
    if (!thread.is_null() && thread != "") body["message_thread_id"] = idv(thread);
    // 1.166: business callback edits/deletes carry business_connection_id (upstream createTelegramCallbackMessageActions).
    std::string biz;
    if (p.contains("business_connection_id")) biz = p["business_connection_id"].get<std::string>();
    else {
        std::string cid = body["chat_id"].is_string() ? body["chat_id"].get<std::string>() : body["chat_id"].dump();
        biz = BizFor(cid);
    }
    if (!biz.empty()) body["business_connection_id"] = biz;
    std::string method;
    // 1.146: sendPoll (public polls register a route for later poll_answer updates) and forum topic CRUD.
    if (action == "poll") {
        method = "sendPoll";
        const std::string q = p.value("question", p.value("message", p.value("text", std::string())));
        nlohmann::json opts = p.contains("options") ? p["options"] : p.value("choices", nlohmann::json::array());
        if (q.empty() || !opts.is_array() || opts.size() < 2) return {{"ok", false}, {"error", "poll requires question and ≥2 options"}};
        nlohmann::json opt_strs = nlohmann::json::array();
        for (auto& o : opts) opt_strs.push_back(o.is_string() ? o.get<std::string>() : o.value("text", o.dump()));
        body["question"] = q; body["options"] = opt_strs;
        if (p.contains("is_anonymous") || p.contains("anonymous") || p.contains("pollAnonymous") || p.contains("pollPublic")) {
            bool anon = true;
            if (p.contains("is_anonymous")) anon = p["is_anonymous"].get<bool>();
            else if (p.contains("anonymous")) anon = p["anonymous"].get<bool>();
            else if (p.value("pollAnonymous", false)) anon = true;
            else if (p.value("pollPublic", false)) anon = false;
            body["is_anonymous"] = anon;
        } else body["is_anonymous"] = false;  // public by default so poll_answer updates arrive
        if (p.value("allows_multiple_answers", p.value("multiple", false))) body["allows_multiple_answers"] = true;
        if (p.contains("correct_option_id")) body["correct_option_id"] = p["correct_option_id"];
        if (p.contains("explanation")) body["explanation"] = p["explanation"];
        auto r = HttpPostJson(api + method, body, {});
        nlohmann::json rec{{"channel", "telegram"}, {"action", action}, {"method", method}, {"body", body}, {"http", r.value("http", 0)}};
        sent_.push_back(rec);
        const bool ok = r.value("http", 0) == 200;
        nlohmann::json resp = r.contains("json") ? r["json"] : nlohmann::json();
        nlohmann::json out{{"ok", ok}, {"method", method}, {"http", r.value("http", 0)}, {"response", resp}};
        if (ok) {
            auto result = resp.value("result", nlohmann::json::object());
            std::string poll_id;
            if (result.is_object() && result.contains("poll") && result["poll"].is_object()) poll_id = result["poll"].value("id", std::string());
            if (!poll_id.empty()) {  // 1.157: anonymous polls too (results arrive as `poll` updates, not poll_answer)
                nlohmann::json entry{{"pollId", poll_id}, {"chat", {{"id", body["chat_id"]}, {"type", p.value("chatType", "private")}}},
                                    {"messageId", result.value("message_id", 0)}, {"question", q}, {"options", opt_strs},
                                    {"anonymous", body.value("is_anonymous", true)}};
                if (body.contains("message_thread_id")) entry["threadId"] = body["message_thread_id"];
                TelegramRememberPoll(entry);
                out["pollId"] = poll_id;
            }
        }
        return out;
    }
    if (action == "topic_icon" || action == "get_topic_icon") {  // 1.166
        std::string cid = body["chat_id"].is_string() ? body["chat_id"].get<std::string>() : body["chat_id"].dump();
        std::string tid;
        if (!thread.is_null() && thread != "") tid = thread.is_string() ? thread.get<std::string>() : thread.dump();
        else if (!mid.is_null() && mid != "") tid = mid.is_string() ? mid.get<std::string>() : mid.dump();
        if (tid.empty()) return {{"ok", false}, {"error", "threadId (topic id) is required"}};
        std::lock_guard<std::mutex> l(g_topic_mu);
        auto& cache = TopicIconCache();
        auto key = cid + ":" + tid;
        if (!cache.contains(key)) return {{"ok", false}, {"error", "no cached icon for topic"}, {"key", key}};
        return {{"ok", true}, {"topicIcon", cache[key]}, {"chatId", cid}, {"threadId", tid}};
    }
    if (action == "invoice" || action == "sendInvoice") {  // 1.157
        method = "sendInvoice";
        for (const char* k : {"title", "description", "payload", "currency"})
            if (!p.contains(k)) return {{"ok", false}, {"error", std::string(k) + " is required"}};
        body["title"] = p["title"]; body["description"] = p["description"]; body["payload"] = p["payload"]; body["currency"] = p["currency"];
        body["prices"] = p.contains("prices") ? p["prices"] : nlohmann::json::array({{{"label", p.value("label", std::string("Total"))}, {"amount", p.value("amount", 0)}}});
        if (p.contains("provider_token")) body["provider_token"] = p["provider_token"];
        else if (p["currency"] != "XTR") { auto pt = config::GetPath(Config(), "channels.telegram.payments.providerToken"); if (pt && pt->is_string()) body["provider_token"] = *pt; }
        body.erase("message_id");
    } else if (action == "create_topic" || action == "createForumTopic") {
        method = "createForumTopic";
        const std::string name = p.value("name", p.value("topic", p.value("title", std::string())));
        if (name.empty()) return {{"ok", false}, {"error", "name is required"}};
        body["name"] = name.substr(0, 128);
        if (p.contains("icon_color")) body["icon_color"] = p["icon_color"];
        if (p.contains("icon_custom_emoji_id") || p.contains("iconCustomEmojiId"))
            body["icon_custom_emoji_id"] = p.contains("icon_custom_emoji_id") ? p["icon_custom_emoji_id"] : p["iconCustomEmojiId"];
        body.erase("message_id");
    } else if (action == "edit_topic" || action == "editForumTopic" || action == "rename_topic") {
        method = "editForumTopic";
        nlohmann::json tid = thread.is_null() || thread == "" ? mid : thread;
        if (tid.is_null() || tid == "") return {{"ok", false}, {"error", "message_thread_id (topic id) is required"}};
        body["message_thread_id"] = idv(tid);
        if (p.contains("name") || p.contains("topic") || p.contains("title"))
            body["name"] = p.value("name", p.value("topic", p.value("title", std::string()))).substr(0, 128);
        if (p.contains("icon_custom_emoji_id") || p.contains("iconCustomEmojiId"))
            body["icon_custom_emoji_id"] = p.contains("icon_custom_emoji_id") ? p["icon_custom_emoji_id"] : p["iconCustomEmojiId"];
        if (!body.contains("name") && !body.contains("icon_custom_emoji_id")) return {{"ok", false}, {"error", "name or iconCustomEmojiId required"}};
    } else if (action == "close_topic" || action == "closeForumTopic") {
        method = "closeForumTopic";
        nlohmann::json tid = thread.is_null() || thread == "" ? mid : thread;
        if (tid.is_null() || tid == "") return {{"ok", false}, {"error", "message_thread_id (topic id) is required"}};
        body["message_thread_id"] = idv(tid);
    } else if (action == "reopen_topic" || action == "reopenForumTopic") {
        method = "reopenForumTopic";
        nlohmann::json tid = thread.is_null() || thread == "" ? mid : thread;
        if (tid.is_null() || tid == "") return {{"ok", false}, {"error", "message_thread_id (topic id) is required"}};
        body["message_thread_id"] = idv(tid);
    } else {
        const bool need_mid = action != "typing";
        if (need_mid) { if (mid.is_null() || mid == "") return {{"ok", false}, {"error", "messageId is required"}}; body["message_id"] = idv(mid); }
        if (action == "react") {
            method = "setMessageReaction";
            const std::string emoji = p.value("emoji", p.value("reaction", std::string()));
            const bool remove = p.value("remove", false) || emoji.empty();
            body["reaction"] = remove ? nlohmann::json::array() : nlohmann::json::array({{{"type", "emoji"}, {"emoji", emoji}}});
            if (p.value("big", false)) body["is_big"] = true;
        } else if (action == "edit") {
            const std::string text = p.value("message", p.value("text", std::string()));
            if (p.contains("buttons")) body["reply_markup"] = TelegramInlineKeyboard(p["buttons"]);
            if (text.empty()) { if (!p.contains("buttons")) return {{"ok", false}, {"error", "message (new text) or buttons required"}}; method = "editMessageReplyMarkup"; }
            else { method = "editMessageText"; body["text"] = text; }
        } else if (action == "delete" || action == "unsend") {
        if (!biz.empty()) {
            // Bot API deleteBusinessMessages(business_connection_id, message_ids)
            method = "deleteBusinessMessages";
            body.erase("chat_id"); body.erase("message_thread_id");
            body["business_connection_id"] = biz;
            nlohmann::json ids = nlohmann::json::array();
            if (!mid.is_null() && mid != "") ids.push_back(idv(mid));
            else if (p.contains("message_ids")) ids = p["message_ids"];
            if (ids.empty()) return {{"ok", false}, {"error", "messageId is required"}};
            body["message_ids"] = ids;
        } else method = "deleteMessage";
        }
        else if (action == "pin") { method = "pinChatMessage"; if (p.value("silent", false)) body["disable_notification"] = true; }
        else if (action == "unpin") method = "unpinChatMessage";
        else if (action == "typing") { method = "sendChatAction"; body["action"] = "typing"; }
        else return {{"ok", false}, {"error", "unsupported telegram action: " + action}};
    }
    auto r = HttpPostJson(api + method, body, {});
    nlohmann::json rec{{"channel", "telegram"}, {"action", action}, {"method", method}, {"body", body}, {"http", r.value("http", 0)}};
    sent_.push_back(rec);
    const bool ok = r.value("http", 0) == 200;
    nlohmann::json out{{"ok", ok}, {"method", method}, {"http", r.value("http", 0)}, {"response", r.contains("json") ? r["json"] : r.value("body", nlohmann::json())}};
    if (ok && (method == "createForumTopic" || method == "editForumTopic")) {
        auto result = r.contains("json") ? r["json"].value("result", nlohmann::json::object()) : nlohmann::json::object();
        std::string cid = body["chat_id"].is_string() ? body["chat_id"].get<std::string>() : body["chat_id"].dump();
        std::string tid = result.contains("message_thread_id") ? result["message_thread_id"].dump()
                        : (body.contains("message_thread_id") ? body["message_thread_id"].dump() : "");
        if (!tid.empty()) {
            nlohmann::json info{{"name", result.value("name", body.value("name", ""))},
                                {"icon_color", result.contains("icon_color") ? result["icon_color"] : body.value("icon_color", nlohmann::json())},
                                {"icon_custom_emoji_id", result.value("icon_custom_emoji_id", body.value("icon_custom_emoji_id", ""))}};
            RememberTopicIcon(cid, tid, info);
            out["topicIcon"] = info; out["threadId"] = result.value("message_thread_id", nlohmann::json());
        }
    }
    return out;
}

nlohmann::json OutboundRecordAdapter::TelegramSyncCommands() {
    const auto token = ChannelCred(Config(), "telegram", "botToken", "TELEGRAM_BOT_TOKEN");
    if (token.empty()) return {{"ok", false}, {"error", "no token"}};
    nlohmann::json cmds = nlohmann::json::array();
    auto cv = config::GetPath(Config(), "channels.telegram.commands");
    if (cv && cv->is_boolean() && !cv->get<bool>()) return {{"ok", true}, {"skipped", true}};
    if (cv && cv->is_array()) {
        for (auto& c : *cv) {
            if (c.is_string()) cmds.push_back({{"command", c.get<std::string>()}, {"description", c.get<std::string>()}});
            else if (c.is_object() && c.contains("command")) cmds.push_back({{"command", c["command"]}, {"description", c.value("description", c["command"].get<std::string>())}});
        }
    } else {
        cmds = nlohmann::json::array({{{"command", "new"}, {"description", "Start a new session"}}, {{"command", "reset"}, {"description", "Reset the current session"}},
                                      {{"command", "status"}, {"description", "Show session status"}}, {{"command", "model"}, {"description", "Show or switch the model"}},
                                      {{"command", "stop"}, {"description", "Stop the current run"}}, {{"command", "help"}, {"description", "Show available commands"}}});
    }
    for (auto& c : cmds) { auto s = c["command"].get<std::string>(); if (!s.empty() && s[0] == '/') s.erase(0, 1); for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); c["command"] = s.substr(0, 32); }
    auto r = HttpPostJson(TelegramBase(Config()) + "/bot" + token + "/setMyCommands", {{"commands", cmds}}, {});
    return {{"ok", r.value("http", 0) == 200}, {"commands", cmds}};
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
        if (synth.contains("business_connection_id") && synth.contains("chat")) {
            const auto& bc = synth["chat"]["id"];
            std::lock_guard<std::mutex> l(g_biz_mu);
            BizConn()[bc.is_string() ? bc.get<std::string>() : bc.dump()] = synth["business_connection_id"].get<std::string>();
        }
    }
    // 1.157: `poll` state updates. A closed poll we sent retires its registry entry and its final tallies are noted in
    // the chat's session (the only result signal anonymous polls ever produce); open-poll tallies are ignored.
    if (body.contains("poll") && body["poll"].is_object() && !body.contains("message")) {
        const auto& pl = body["poll"];
        const std::string poll_id = pl.value("id", std::string());
        auto entry = TelegramLookupPoll(poll_id);
        if (!entry.is_object() || entry.empty()) return {{"ok", true}, {"ignored", true}, {"note", "unknown poll_id"}};
        if (!pl.value("is_closed", false)) return {{"ok", true}, {"ignored", true}, {"note", "poll still open"}};
        std::string tallies; long total = pl.value("total_voter_count", 0L);
        for (auto& o : pl.value("options", nlohmann::json::array()))
            tallies += (tallies.empty() ? "" : ", ") + o.value("text", std::string("?")) + ": " + std::to_string(o.value("voter_count", 0));
        TelegramRetirePoll(poll_id);
        InboundMessage m; m.channel = "telegram";
        const auto& chat = entry["chat"];
        const std::string cid = chat["id"].is_string() ? chat["id"].get<std::string>() : chat["id"].dump();
        m.sender = cid; m.peer_id = cid;
        m.chat_type = chat.value("type", std::string("private")) == "private" ? "direct" : "group";
        if (m.chat_type == "group") m.conversation_id = cid;
        m.session_key = "telegram-" + cid;
        if (entry.contains("threadId")) { m.thread_id = entry["threadId"].dump(); m.session_key += ":topic:" + m.thread_id; if (m.chat_type == "group") m.conversation_id = cid + ":topic:" + m.thread_id; }
        m.text = std::string("[poll closed") + (entry.value("anonymous", false) ? ", anonymous" : "") + "] " + entry.value("question", std::string("?")) +
                 " → " + tallies + " (" + std::to_string(total) + " voters)";
        m.inject_only = true;
        if (deliver) deliver(m);
        return {{"ok", true}, {"channel", "telegram"}, {"pollId", poll_id}, {"closed", true}, {"results", tallies}};
    }
    // 1.157: payments. pre_checkout_query must be answered within 10 s; approved only when channels.telegram.payments.enabled.
    if (body.contains("pre_checkout_query") && body["pre_checkout_query"].is_object()) {
        const auto& q = body["pre_checkout_query"];
        auto en = config::GetPath(Config(), "channels.telegram.payments.enabled");
        const bool enabled = en && en->is_boolean() && en->get<bool>();
        const auto token = ChannelCred(Config(), "telegram", "botToken", "TELEGRAM_BOT_TOKEN");
        nlohmann::json ans{{"pre_checkout_query_id", q.value("id", std::string())}, {"ok", enabled}};
        if (!enabled) ans["error_message"] = "Payments are not enabled for this bot.";
        if (!token.empty()) HttpPostJson(TelegramBase(Config()) + "/bot" + token + "/answerPreCheckoutQuery", ans, {});
        return {{"ok", true}, {"channel", "telegram"}, {"preCheckout", enabled ? "approved" : "declined"}};
    }
    if (body.contains("message") && body["message"].is_object() && body["message"].contains("successful_payment")) {
        const auto& msg = body["message"]; const auto& sp = msg["successful_payment"];
        InboundMessage m; m.channel = "telegram";
        const auto& chat = msg["chat"]; const std::string cid = chat["id"].is_string() ? chat["id"].get<std::string>() : chat["id"].dump();
        m.sender = msg.contains("from") ? msg["from"]["id"].dump() : cid; m.peer_id = m.sender;
        m.chat_type = chat.value("type", std::string("private")) == "private" ? "direct" : "group";
        if (m.chat_type == "group") m.conversation_id = cid;
        m.session_key = "telegram-" + cid;
        char amt[64]; std::snprintf(amt, sizeof amt, "%.2f", sp.value("total_amount", 0L) / 100.0);
        m.text = "[payment received] " + std::string(amt) + " " + sp.value("currency", std::string()) + " payload=" + sp.value("invoice_payload", std::string()) +
                 " charge=" + sp.value("telegram_payment_charge_id", std::string());
        m.inject_only = true;
        if (deliver) deliver(m);
        return {{"ok", true}, {"channel", "telegram"}, {"payment", sp}};
    }
    // 1.146: poll_answer updates (public polls only) rejoin via the sendPoll registry.
    if (body.contains("poll_answer") && body["poll_answer"].is_object()) {
        const auto& pa = body["poll_answer"];
        const std::string poll_id = pa.value("poll_id", std::string());
        auto entry = TelegramLookupPoll(poll_id);
        if (!entry.is_object() || entry.empty() || entry.value("anonymous", false)) return {{"ok", true}, {"ignored", true}, {"note", "unknown poll_id"}};
        if (pa.contains("user") && pa["user"].value("is_bot", false)) return {{"ok", true}, {"ignored", true}};
        std::string chosen;
        auto opts = entry.value("options", nlohmann::json::array());
        for (auto& oid : pa.value("option_ids", nlohmann::json::array())) {
            int i = oid.is_number() ? oid.get<int>() : 0;
            if (i >= 0 && i < (int)opts.size()) chosen += (chosen.empty() ? "" : ", ") + opts[i].get<std::string>();
        }
        InboundMessage m; m.channel = "telegram";
        const auto& chat = entry["chat"];
        const std::string cid = chat["id"].is_string() ? chat["id"].get<std::string>() : chat["id"].dump();
        m.sender = pa.contains("user") ? pa["user"]["id"].dump() : cid; m.peer_id = m.sender;
        const auto type = chat.value("type", std::string("private"));
        m.chat_type = type == "private" ? "direct" : "group";
        if (m.chat_type == "group") m.conversation_id = cid;
        m.session_key = "telegram-" + cid;
        if (entry.contains("threadId")) { m.thread_id = entry["threadId"].dump(); m.session_key += ":topic:" + m.thread_id; if (m.chat_type == "group") m.conversation_id = cid + ":topic:" + m.thread_id; }
        m.text = "[poll answer] " + entry.value("question", std::string("?")) + " → " + (chosen.empty() ? "(none)" : chosen);
        if (deliver) deliver(m);
        return {{"ok", true}, {"channel", "telegram"}, {"pollId", poll_id}, {"options", chosen}};
    }
    // 1.140: message_reaction updates become a short system-style user note in the chat's session.
    if (body.contains("message_reaction") && body["message_reaction"].is_object()) {
        const auto& mr = body["message_reaction"];
        if (!mr.contains("chat")) return {{"ok", true}, {"ignored", true}};
        std::string added;
        for (auto& r : mr.value("new_reaction", nlohmann::json::array())) if (r.value("type", "") == "emoji") added += (added.empty() ? "" : " ") + r.value("emoji", std::string());
        if (added.empty()) return {{"ok", true}, {"ignored", true}, {"note", "reaction removed"}};
        InboundMessage m; m.channel = "telegram";
        const auto& chat = mr["chat"]; const std::string cid = chat["id"].is_string() ? chat["id"].get<std::string>() : chat["id"].dump();
        m.sender = mr.contains("user") ? mr["user"]["id"].dump() : cid; m.peer_id = m.sender;
        m.chat_type = chat.value("type", std::string("private")) == "private" ? "direct" : "group";
        if (m.chat_type == "group") m.conversation_id = cid;
        m.session_key = "telegram-" + cid;
        m.text = "[reaction " + added + " on message " + mr.value("message_id", nlohmann::json(0)).dump() + "]";
        m.inject_only = true;
        if (deliver) deliver(m);
        return {{"ok", true}, {"channel", "telegram"}, {"reaction", added}};
    }
    if (body.contains("business_connection") || body.contains("deleted_business_messages")) return {{"ok", true}, {"ignored", true}, {"note", "business bookkeeping"}};
    const bool edited = !synth.is_object() && !body.contains("message") && (body.contains("edited_message") || body.contains("edited_business_message"));
    const auto& tm = synth.is_object() ? synth : body.contains("message") ? body["message"] : body.contains("business_message") ? body["business_message"]
                   : body.contains("edited_message") ? body["edited_message"] : body.contains("edited_business_message") ? body["edited_business_message"] : nlohmann::json();
    if (tm.is_object() && tm.contains("business_connection_id") && tm.contains("chat")) {
        const auto& bc = tm["chat"]["id"];
        std::lock_guard<std::mutex> l(g_biz_mu);
        BizConn()[bc.is_string() ? bc.get<std::string>() : bc.dump()] = tm["business_connection_id"].get<std::string>();
    }
    if (!tm.is_object() || !tm.contains("chat")) return {{"ok", true}, {"ignored", true}, {"note", "no message"}};
    InboundMessage m;
    m.channel = "telegram";
    m.text = tm.value("text", tm.value("caption", std::string()));
    if (edited && !m.text.empty()) m.text = "[edited message " + tm.value("message_id", nlohmann::json(0)).dump() + "] " + m.text;
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
    // 1.166: cache forum topic icons from service messages / topic headers.
    if (tm.contains("forum_topic_created") && tm["forum_topic_created"].is_object()) {
        const auto& ft = tm["forum_topic_created"];
        std::string cid = tm["chat"]["id"].is_string() ? tm["chat"]["id"].get<std::string>() : tm["chat"]["id"].dump();
        std::string tid = tm.contains("message_thread_id") ? tm["message_thread_id"].dump() : "";
        if (!tid.empty()) RememberTopicIcon(cid, tid, {{"name", ft.value("name", "")}, {"icon_color", ft.value("icon_color", nlohmann::json())},
                                                       {"icon_custom_emoji_id", ft.value("icon_custom_emoji_id", "")}});
    }
    if (tm.contains("forum_topic_edited") && tm["forum_topic_edited"].is_object()) {
        const auto& ft = tm["forum_topic_edited"];
        std::string cid = tm["chat"]["id"].is_string() ? tm["chat"]["id"].get<std::string>() : tm["chat"]["id"].dump();
        std::string tid = tm.contains("message_thread_id") ? tm["message_thread_id"].dump() : "";
        if (!tid.empty()) {
            nlohmann::json info = nlohmann::json::object();
            { std::lock_guard<std::mutex> l(g_topic_mu); auto& c = TopicIconCache(); auto k = cid + ":" + tid; if (c.contains(k)) info = c[k]; }
            if (ft.contains("name")) info["name"] = ft["name"];
            if (ft.contains("icon_color")) info["icon_color"] = ft["icon_color"];
            if (ft.contains("icon_custom_emoji_id")) info["icon_custom_emoji_id"] = ft["icon_custom_emoji_id"];
            RememberTopicIcon(cid, tid, info);
        }
    }
    if (m.text.empty() && m.media.empty()) return {{"ok", true}, {"ignored", true}, {"note", "no text or media"}};
    const auto& chat = tm["chat"];
    const std::string cid = chat["id"].is_string() ? chat["id"].get<std::string>() : chat["id"].dump();
    m.sender = tm.contains("from") ? (tm["from"]["id"].is_string() ? tm["from"]["id"].get<std::string>() : tm["from"]["id"].dump()) : cid;
    const auto type = chat.value("type", std::string("private"));
    m.chat_type = type == "private" ? "direct" : "group";
    if (m.chat_type == "group") m.conversation_id = cid;
    m.peer_id = m.sender;
    m.session_key = "telegram-" + cid;
    // 1.146: forum topics get their own session (`telegram-<chat>:topic:<threadId>`) so threads do not share history.
    if ((tm.value("is_topic_message", false) || (chat.value("is_forum", false) && tm.contains("message_thread_id"))) && tm.contains("message_thread_id")) {
        m.thread_id = tm["message_thread_id"].dump();
        m.session_key += ":topic:" + m.thread_id;
        if (m.chat_type == "group") m.conversation_id = cid + ":topic:" + m.thread_id;
    }
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
