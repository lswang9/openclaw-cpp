#include "openclaw/events/bus.hpp"

#include <algorithm>

namespace openclaw::events {

bool Bus::Match(const SubFilter& f, const std::string& event, const nlohmann::json& payload) {
    if (f.event_name && *f.event_name != event) return false;
    if (f.session_key) {
        const std::string sk = payload.value("sessionKey", payload.value("session_key", ""));
        if (sk != *f.session_key) return false;
    }
    return true;
}

Bus::Id Bus::Subscribe(Listener listener, SubFilter filter) {
    std::lock_guard lock(mu_);
    Id id = next_++;
    listeners_.push_back({id, std::move(listener), std::move(filter)});
    return id;
}

void Bus::Unsubscribe(Id id) {
    std::lock_guard lock(mu_);
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [&](const Entry& e) { return e.id == id; }),
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
        for (auto& e : listeners_) {
            if (Match(e.filter, event, payload)) copy.push_back(e.listener);
        }
    }
    for (auto& l : copy) {
        try { l(event, payload); } catch (...) {}
    }
}

std::size_t Bus::ListenerCount() const {
    std::lock_guard lock(mu_);
    return listeners_.size();
}

std::vector<nlohmann::json> Bus::Recent(std::size_t limit, const SubFilter& filter) const {
    std::lock_guard lock(mu_);
    std::vector<nlohmann::json> out;
    for (auto it = recent_.rbegin(); it != recent_.rend() && out.size() < limit; ++it) {
        const std::string ev = it->value("event", "");
        const auto& payload = (*it)["payload"];
        if (Match(filter, ev, payload.is_null() ? nlohmann::json::object() : payload)) {
            out.push_back(*it);
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace openclaw::events
