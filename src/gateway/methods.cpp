#include "openclaw/gateway/methods.hpp"

namespace openclaw::gateway {

std::vector<std::string> ImplementedMethods() {
    return {
        "health", "status", "gateway.status", "system.info", "connect",
        "methods.list", "events.list",
        "sessions.list", "sessions.get", "sessions.delete", "sessions.create",
        "sessions.reset", "sessions.preview", "sessions.patch", "sessions.send",
        "sessions.subscribe", "sessions.messages.subscribe", "sessions.compact",
        "chat.send", "chat.history", "chat.abort", "chat.inject",
        "tools.list", "tools.invoke", "tools.catalog", "tools.effective",
        "config.get", "config.set", "config.apply", "config.patch", "config.schema",
        "config.schema.lookup",
        "channels.status", "channels.start", "channels.stop",
        "channels.pairing.list", "channels.pairing.approve", "channels.pairing.dismiss",
        "agents.list", "agents.create", "agents.update", "agents.delete",
        "agents.files.list", "agents.files.get", "agents.files.set",
        "skills.status", "skills.library.list", "skills.library.read", "skills.detail",
        "skills.search", "plugins.list", "plugins.setEnabled",
        "cron.add", "cron.update", "cron.remove", "cron.runs", "cron.list", "cron.run",
        "memory.get", "memory.status", "memory.search",
        "approval.list", "approval.get", "approval.resolve",
        "exec.approval.list", "exec.approval.request", "exec.approval.resolve",
        "plugin.approval.list", "plugin.approval.request", "plugin.approval.resolve",
        "secrets.store.list", "secrets.store.set", "secrets.store.delete", "secrets.reload",
        "node.pair.list", "device.pair.list", "device.pair.approve", "device.pair.reject",
        "device.pair.remove", "node.list", "node.describe", "node.rename",
        "mcp.app.listTools",
        "message.action", "conversations.send",
        "artifacts.list", "artifacts.get", "assistant.media.get",
        "logs.tail",
    };
}

}  // namespace openclaw::gateway
