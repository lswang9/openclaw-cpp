#pragma once
#include <cstdlib>
#include "openclaw/config/config.hpp"

#include "openclaw/channels/adapter.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace openclaw::channels {

// Env wins; else channels.<id>.<key> string from adapter config snapshot.
inline std::string ChannelCred(const nlohmann::json& cfg, const std::string& channel_id,
                               const std::string& key, const char* env) {
    if (env && *env) {
        if (const char* v = std::getenv(env); v && *v) return v;
    }
    auto v = config::GetPath(cfg, "channels." + channel_id + "." + key);
    if (v && v->is_string() && !v->get<std::string>().empty()) return v->get<std::string>();
    // Common aliases
    if (key == "token") {
        auto a = config::GetPath(cfg, "channels." + channel_id + ".botToken");
        if (a && a->is_string()) return a->get<std::string>();
    }
    if (key == "botToken") {
        auto a = config::GetPath(cfg, "channels." + channel_id + ".token");
        if (a && a->is_string()) return a->get<std::string>();
    }
    return {};
}


class OutboundRecordAdapter : public Adapter {
public:
    OutboundRecordAdapter(std::string id, std::string todo_note, std::string webhook_path);
    std::string Id() const override { return id_; }
    bool Enabled() const override { return enabled_; }
    void SetEnabled(bool on) override { enabled_ = on; }
    nlohmann::json Status() const override;

    // Send text. If env token present, POST via libcurl; always records payload.
    // Telegram: TELEGRAM_BOT_TOKEN → https://api.telegram.org/bot<token>/sendMessage
    // Discord:  DISCORD_BOT_TOKEN + channel id → Discord API
    // Feishu:   FEISHU_APP_ID/SECRET deferred — record only unless FEISHU_WEBHOOK_URL set
    nlohmann::json SendText(const std::string& chat_id, const std::string& text,
                            const nlohmann::json& extra = {});
    // Typing indicator: Telegram sendChatAction, Discord POST /channels/{id}/typing; others record only.
    nlohmann::json SendTyping(const std::string& chat_id);
    nlohmann::json OutboundLog() const;
    void ClearLog();

protected:
    nlohmann::json HttpPostJson(const std::string& url, const nlohmann::json& body,
                                const std::vector<std::string>& headers) const;
    nlohmann::json HttpJson(const std::string& method, const std::string& url, const nlohmann::json& body,
                            const std::vector<std::string>& headers) const;
    void Record(nlohmann::json e) { sent_.push_back(std::move(e)); }

private:
    std::string id_;
    std::string todo_;
    std::string webhook_path_;
    bool enabled_ = false;
    std::vector<nlohmann::json> sent_;
    std::mutex typing_mu_;
};

}  // namespace openclaw::channels
