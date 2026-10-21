#pragma once

#include "openclaw/channels/adapter.hpp"

#include <string>
#include <vector>

namespace openclaw::channels {

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
    nlohmann::json OutboundLog() const;
    void ClearLog();

protected:
    nlohmann::json HttpPostJson(const std::string& url, const nlohmann::json& body,
                                const std::vector<std::string>& headers) const;
    void Record(nlohmann::json e) { sent_.push_back(std::move(e)); }

private:
    std::string id_;
    std::string todo_;
    std::string webhook_path_;
    bool enabled_ = false;
    std::vector<nlohmann::json> sent_;
};

}  // namespace openclaw::channels
