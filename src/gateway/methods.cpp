#include "openclaw/gateway/methods.hpp"

namespace openclaw::gateway {

std::vector<std::string> AdvertisedMethods() {
    return {
        // health / meta
        "health",
        "status",
        "gateway.status",
        "methods.list",
        "events.list",
        // sessions (partial)
        "sessions.list",
        "sessions.get",
        "sessions.delete",
        // chat (partial)
        "chat.send",
        // tools
        "tools.list",
        "tools.invoke",
        // config
        "config.get",
        // channels
        "channels.status",
    };
}

nlohmann::json AdvertisedMethodsJson() {
    return AdvertisedMethods();
}

std::vector<std::string> AdvertisedEvents() {
    return {
        "connect.challenge",
        "health",
        "tick",
        "session.message",
        "sessions.changed",
        "shutdown",
    };
}

}  // namespace openclaw::gateway
