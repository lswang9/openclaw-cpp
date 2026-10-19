#include "openclaw/events/bus.hpp"
#include <algorithm>

namespace openclaw::events {

Bus::Id Bus::Subscribe(Listener listener) {
    std::lock_guard lock(mu_);
    Id id = next_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void Bus::Unsubscribe(Id id) {
    std::lock_guard lock(mu_);
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [&](const auto& p) { return p.first == id; }),
                     listeners_.end());
}

void Bus::Emit(const std::string& event, const nlohmann::json& payload) {
    std::vector<Listener> copy;
    {
        std::lock_guard lock(mu_);
        recent_.push_back({{"event", event}, {"payload", payload}});
        if (recent_.size() > 128) {
            recent_.erase(recent_.begin(), recent_.begin() + static_cast<long>(recent_.size() - 128));
        }
        for (auto& [_, l] : listeners_) {
            copy.push_back(l);
        }
    }
    for (auto& l : copy) {
        try {
            l(event, payload);
        } catch (...) {
        }
    }
}

std::size_t Bus::ListenerCount() const {
    std::lock_guard lock(mu_);
    return listeners_.size();
}

std::vector<nlohmann::json> Bus::Recent(std::size_t limit) const {
    std::lock_guard lock(mu_);
    if (recent_.size() <= limit) return recent_;
    return {recent_.end() - static_cast<long>(limit), recent_.end()};
}

}  // namespace openclaw::events
