#include "openclaw/gateway/methods.hpp"

namespace openclaw::gateway {

std::vector<std::string> ImplementedMethods() {
    return {
        "health", "status", "gateway.status", "system.info", "connect",
        "methods.list", "events.list",
        "sessions.list", "sessions.get", "sessions.delete", "sessions.create",
        "sessions.reset", "sessions.preview", "sessions.patch", "sessions.send",
        "sessions.subscribe", "sessions.compact",
        "chat.send", "chat.history", "chat.abort", "chat.inject",
        "tools.list", "tools.invoke", "tools.catalog", "tools.effective",
        "config.get", "config.set", "config.apply", "config.patch", "config.schema",
        "channels.status", "channels.start", "channels.stop",
        "channels.pairing.list", "channels.pairing.approve", "channels.pairing.dismiss",
        "agents.list", "agents.create", "agents.update", "agents.delete",
        "skills.status", "skills.library.list", "skills.library.read", "skills.detail",
        "skills.search", "plugins.list", "plugins.setEnabled",
        "cron.add", "cron.update", "cron.remove", "cron.runs", "cron.list", "cron.run",
        "memory.get", "memory.status", "memory.search",
        "approval.list", "approval.get", "approval.resolve",
        "exec.approval.list", "exec.approval.request", "exec.approval.resolve",
        "plugin.approval.list", "plugin.approval.request", "plugin.approval.resolve",
        "secrets.store.list", "secrets.store.set", "secrets.store.delete",
        "secrets.reload",
        "node.pair.list", "device.pair.list",
        "mcp.app.listTools",
    };
}

}  // namespace openclaw::gateway
