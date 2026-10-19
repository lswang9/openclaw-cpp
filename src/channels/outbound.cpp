#include "openclaw/channels/outbound.hpp"

#include <chrono>

namespace openclaw::channels {

OutboundRecordAdapter::OutboundRecordAdapter(std::string id, std::string todo_note,
                                             std::string webhook_path)
    : id_(std::move(id)), todo_(std::move(todo_note)), webhook_path_(std::move(webhook_path)) {}

nlohmann::json OutboundRecordAdapter::Status() const {
    return {{"id", id_},
            {"enabled", enabled_},
            {"type", "outbound_record"},
            {"todo", todo_},
            {"webhook_path", webhook_path_},
            {"sent_count", sent_.size()}};
}

nlohmann::json OutboundRecordAdapter::SendText(const std::string& chat_id, const std::string& text,
                                               const nlohmann::json& extra) {
    if (!enabled_) {
        // Allow recording even when disabled for dry-run; mark dry_run
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    nlohmann::json entry{
        {"ts_ms", ms},
        {"channel", id_},
        {"chat_id", chat_id},
        {"text", text},
        {"webhook_path", webhook_path_},
        {"enabled", enabled_},
        {"extra", extra},
    };
    sent_.push_back(entry);
    return {{"ok", true}, {"recorded", true}, {"entry", entry}, {"sent_count", sent_.size()}};
}

nlohmann::json OutboundRecordAdapter::OutboundLog() const {
    return sent_;
}

void OutboundRecordAdapter::ClearLog() { sent_.clear(); }

}  // namespace openclaw::channels
