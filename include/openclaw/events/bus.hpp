#pragma once
#include <cstdint>

#include <nlohmann/json.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace openclaw::events {

using Listener = std::function<void(const std::string& event, const nlohmann::json& payload)>;

// Simple in-process pub/sub for gateway WS fan-out.
class Bus {
public:
    using Id = std::uint64_t;
    Id Subscribe(Listener listener);
    void Unsubscribe(Id id);
    void Emit(const std::string& event, const nlohmann::json& payload);
    std::size_t ListenerCount() const;
    // Recent events ring buffer for self-test / sessions.subscribe catch-up.
    std::vector<nlohmann::json> Recent(std::size_t limit = 32) const;

private:
    mutable std::mutex mu_;
    Id next_ = 1;
    std::vector<std::pair<Id, Listener>> listeners_;
    std::vector<nlohmann::json> recent_;
};

}  // namespace openclaw::events
