#include "openclaw/gateway/methods.hpp"

namespace openclaw::gateway {

std::vector<std::string> ImplementedMethods() {
    return {
        "health",
        "status",
        "gateway.status",
        "methods.list",
        "events.list",
        "sessions.list",
        "sessions.get",
        "sessions.delete",
        "sessions.create",
        "sessions.reset",
        "sessions.preview",
        "chat.send",
        "chat.history",
        "chat.abort",
        "tools.list",
        "tools.invoke",
        "tools.catalog",
        "config.get",
        "config.schema",
        "channels.status",
        "channels.pairing.list",
        "agents.list",
        "skills.status",
        "skills.library.list",
        "plugins.list",
        "cron.add",
        "cron.remove",
        "cron.runs",
        "cron.list",
        "memory.get",
        "memory.status",
        "memory.search",
        "system.info",
        "connect",
        "node.pair.list",
        "device.pair.list",
    };
}

}  // namespace openclaw::gateway
