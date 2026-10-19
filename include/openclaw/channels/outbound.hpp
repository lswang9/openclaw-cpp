#pragma once

#include "openclaw/channels/adapter.hpp"

#include <string>
#include <vector>

namespace openclaw::channels {

// Records outbound webhook payloads (no real network). For telegram/feishu stubs.
class OutboundRecordAdapter : public Adapter {
public:
    OutboundRecordAdapter(std::string id, std::string todo_note, std::string webhook_path);
    std::string Id() const override { return id_; }
    bool Enabled() const override { return enabled_; }
    void SetEnabled(bool on) override { enabled_ = on; }
    nlohmann::json Status() const override;

    // "Send" — records payload; optionally would HTTP POST when OPENCLAW_CHANNEL_HTTP_SEND=1 later.
    nlohmann::json SendText(const std::string& chat_id, const std::string& text,
                            const nlohmann::json& extra = {});
    nlohmann::json OutboundLog() const;
    void ClearLog();

private:
    std::string id_;
    std::string todo_;
    std::string webhook_path_;
    bool enabled_ = false;
    std::vector<nlohmann::json> sent_;
};

}  // namespace openclaw::channels
