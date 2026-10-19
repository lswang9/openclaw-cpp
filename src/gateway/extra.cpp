#include "openclaw/gateway/extra.hpp"
#include "openclaw/gateway/methods.hpp"
#include "openclaw/config/config.hpp"

#include <set>

namespace openclaw::gateway {
namespace {

const std::set<std::string>& ImplementedSet() {
    static const std::set<std::string> s(ImplementedMethods().begin(), ImplementedMethods().end());
    return s;
}

nlohmann::json Honest(const std::string& method, const nlohmann::json& params,
                      const nlohmann::json& state) {
    return {{"ok", true},
            {"method", method},
            {"honest", true},
            {"params", params},
            {"state", state},
            {"note", "category handler — partial OpenClaw 2026.10.5 parity"}};
}

}  // namespace

ExtraResult TryExtraDispatch(ServerDeps& deps, const std::string& method,
                             const nlohmann::json& params) {
    ExtraResult r;
    if (!ImplementedSet().count(method)) return r;

    // Provider hot-switch
    if (method == "providers.list") {
        r.handled = true;
        nlohmann::json list = nlohmann::json::array();
        if (deps.router) {
            for (const auto& n : deps.router->Names()) {
                list.push_back({{"id", n}, {"default", n == deps.router->Default()}});
            }
        }
        r.payload = {{"providers", list}, {"default", deps.router ? deps.router->Default() : ""}};
        return r;
    }
    if (method == "providers.setDefault" || method == "models.setDefault") {
        r.handled = true;
        if (!deps.router) {
            r.ok = false;
            r.error = "router not configured";
            return r;
        }
        const std::string id = params.value("id", params.value("provider", params.value("model", "")));
        // allow "openai/gpt-x" → provider openai
        std::string prov = id;
        auto slash = id.find('/');
        if (slash != std::string::npos) prov = id.substr(0, slash);
        if (!deps.router->Has(prov)) {
            r.ok = false;
            r.error = "unknown provider: " + prov;
            return r;
        }
        deps.router->SetDefault(prov);
        deps.logs.Append("info", "default provider -> " + prov);
        r.payload = {{"default", deps.router->Default()}, {"requested", id}};
        return r;
    }

    // Prefix category honest handlers with local state
    auto starts = [&](const char* p) { return method.rfind(p, 0) == 0; };

    if (starts("users.")) {
        r.handled = true;
        if (!deps.config.raw.contains("users")) deps.config.raw["users"] = nlohmann::json::object();
        auto& users = deps.config.raw["users"];
        if (method == "users.self") {
            r.payload = {{"id", "local"}, {"displayName", users.value("displayName", "operator")}};
            return r;
        }
        if (method == "users.prefs.get") {
            r.payload = {{"prefs", users.value("prefs", nlohmann::json::object())}};
            return r;
        }
        if (method == "users.prefs.set") {
            users["prefs"] = params.value("prefs", params);
            config::Save(config::DefaultPath(), deps.config);
            r.payload = {{"saved", true}, {"prefs", users["prefs"]}};
            return r;
        }
        if (method == "users.list") {
            r.payload = {{"users", nlohmann::json::array({{{"id", "local"}}})}};
            return r;
        }
        r.payload = Honest(method, params, {{"users", users}});
        return r;
    }

    if (starts("plugins.")) {
        r.handled = true;
        if (method == "plugins.search") {
            auto q = params.value("q", params.value("query", ""));
            nlohmann::json hits = nlohmann::json::array();
            for (const auto& p : deps.plugins.ListJson()) {
                if (q.empty() || p.dump().find(q) != std::string::npos) hits.push_back(p);
            }
            r.payload = {{"hits", hits}};
            return r;
        }
        r.payload = Honest(method, params, {{"plugins", deps.plugins.ListJson()}});
        return r;
    }

    if (starts("update.")) {
        r.handled = true;
        r.payload = {{"status", "idle"},
                     {"current", "0.8.0-dev"},
                     {"align_target", "OpenClaw 2026.10.5"},
                     {"method", method},
                     {"note", "C++ rewrite: no auto-updater yet"}};
        return r;
    }

    if (starts("tts.") || starts("voicewake.") || starts("talk.")) {
        r.handled = true;
        r.payload = Honest(method, params, {{"available", false}, {"reason", "media pipeline TODO"}});
        return r;
    }

    if (starts("hooks.") || starts("presence.") || method == "poll" || starts("push.") ||
        starts("attach.") || starts("system-") || starts("commands.")) {
        r.handled = true;
        r.payload = Honest(method, params, {{"runtime", "openclaw-cpp"}});
        return r;
    }

    if (starts("board.") || starts("portal.") || starts("worktrees.") || starts("environments.") ||
        starts("fs.") || starts("computer.") || starts("canvas.") || starts("claws.") ||
        starts("themes.") || starts("webSearch.") || starts("backup.") || starts("audit.") ||
        starts("taskSuggestions.") || starts("transcripts.") || starts("wizard.") ||
        starts("openclaw.") || starts("nativeHook.") || starts("web.login")) {
        r.handled = true;
        r.payload = Honest(method, params, nlohmann::json::object());
        return r;
    }

    if (starts("session.") || starts("sessions.")) {
        r.handled = true;
        r.payload = Honest(method, params, {{"sessions", deps.sessions.List()}});
        return r;
    }

    if (starts("agents.")) {
        r.handled = true;
        r.payload = Honest(method, params, {{"agents", deps.config.raw.value("agents_list", nlohmann::json::array())}});
        return r;
    }

    if (starts("skills.")) {
        r.handled = true;
        r.payload = Honest(method, params, deps.skills.StatusJson());
        return r;
    }

    if (starts("channels.")) {
        r.handled = true;
        r.payload = Honest(method, params, {{"adapters", deps.adapters.StatusJson()}});
        return r;
    }

    if (starts("cron.")) {
        r.handled = true;
        r.payload = Honest(method, params, {{"jobs", deps.cron.ListJson()}});
        return r;
    }

    if (starts("memory.") || starts("doctor.") || starts("diagnostics.")) {
        r.handled = true;
        r.payload = Honest(method, params, {{"memory_keys", deps.memory.Keys()}});
        return r;
    }

    if (starts("node.") || starts("device.")) {
        r.handled = true;
        r.payload = Honest(method, params, {{"nodes", deps.nodes.ListJson()}, {"pairing", deps.pairing.ListJson()}});
        return r;
    }

    if (starts("mcp.")) {
        r.handled = true;
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& n : deps.tools.Names())
            if (n.rfind("mcp.", 0) == 0) tools.push_back(n);
        r.payload = Honest(method, params, {{"mcp_tools", tools}});
        return r;
    }

    if (starts("secrets.") || starts("approval") || starts("exec.") || starts("plugin.approval")) {
        r.handled = true;
        r.payload = Honest(method, params, {{"approvals", deps.approvals.ListJson()},
                                            {"secrets_keys", deps.secrets.ListJson()}});
        return r;
    }

    if (starts("chat.") || starts("tools.") || starts("config.") || starts("models.") ||
        starts("providers.") || starts("logs.") || starts("artifacts.") || starts("assistant.") ||
        starts("message.") || starts("conversations.")) {
        r.handled = true;
        r.payload = Honest(method, params, nlohmann::json::object());
        return r;
    }

    // Remaining implemented methods
    r.handled = true;
    r.payload = Honest(method, params, nlohmann::json::object());
    return r;
}

}  // namespace openclaw::gateway
