#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::events {

struct SubFilter {
    std::optional<std::string> session_key;  // if set, only events with matching payload.sessionKey
    std::optional<std::string> event_name;   // if set, only this event type
};

using Listener = std::function<void(const std::string& event, const nlohmann::json& payload)>;

class Bus {
public:
    using Id = std::uint64_t;
    Id Subscribe(Listener listener, SubFilter filter = {});
    void Unsubscribe(Id id);
    void Emit(const std::string& event, const nlohmann::json& payload);
    std::size_t ListenerCount() const;
    std::vector<nlohmann::json> Recent(std::size_t limit = 32,
                                       const SubFilter& filter = {}) const;

private:
    struct Entry {
        Id id;
        Listener listener;
        SubFilter filter;
    };
    static bool Match(const SubFilter& f, const std::string& event, const nlohmann::json& payload);

    mutable std::mutex mu_;
    Id next_ = 1;
    std::vector<Entry> listeners_;
    std::vector<nlohmann::json> recent_;
};

}  // namespace openclaw::events
