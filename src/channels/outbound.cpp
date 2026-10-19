#include "openclaw/channels/outbound.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdlib>

namespace openclaw::channels {
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
    if (id_ == "telegram") token = !Env("TELEGRAM_BOT_TOKEN").empty();
    if (id_ == "discord") token = !Env("DISCORD_BOT_TOKEN").empty();
    if (id_ == "feishu") token = !Env("FEISHU_WEBHOOK_URL").empty();
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
        const auto token = Env("TELEGRAM_BOT_TOKEN");
        if (!token.empty() && !chat_id.empty()) {
            const std::string url = "https://api.telegram.org/bot" + token + "/sendMessage";
            http_result = HttpPostJson(url, {{"chat_id", chat_id}, {"text", text}}, {});
            entry["mode"] = "live_http";
            entry["delivered"] = http_result.value("http", 0) >= 200 && http_result.value("http", 0) < 300;
            entry["http"] = http_result;
        }
    } else if (id_ == "discord") {
        const auto token = Env("DISCORD_BOT_TOKEN");
        if (!token.empty() && !chat_id.empty()) {
            const std::string url = "https://discord.com/api/v10/channels/" + chat_id + "/messages";
            http_result = HttpPostJson(url, {{"content", text}},
                                       {"Authorization: Bot " + token});
            entry["mode"] = "live_http";
            entry["delivered"] = http_result.value("http", 0) >= 200 && http_result.value("http", 0) < 300;
            entry["http"] = http_result;
        }
    } else if (id_ == "feishu") {
        const auto hook = Env("FEISHU_WEBHOOK_URL");
        if (!hook.empty()) {
            http_result = HttpPostJson(hook, {{"msg_type", "text"}, {"content", {{"text", text}}}}, {});
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

nlohmann::json OutboundRecordAdapter::OutboundLog() const { return sent_; }
void OutboundRecordAdapter::ClearLog() { sent_.clear(); }

}  // namespace openclaw::channels
