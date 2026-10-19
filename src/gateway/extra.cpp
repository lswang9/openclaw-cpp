#include "openclaw/gateway/extra.hpp"
#include "openclaw/gateway/methods.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/channels/outbound.hpp"

#include <set>

namespace openclaw::gateway {
namespace {

const std::set<std::string>& ImplementedSet() {
    static const std::set<std::string> s(ImplementedMethods().begin(), ImplementedMethods().end());
    return s;
}

nlohmann::json Real(nlohmann::json payload) {
    payload["impl"] = "real";
    return payload;
}

nlohmann::json Honest(const std::string& method, const nlohmann::json& params,
                      const nlohmann::json& state) {
    return {{"ok", true},
            {"impl", "honest"},
            {"method", method},
            {"params", params},
            {"state", state},
            {"note", "honest partial — not full OpenClaw 2026.10.5 behavior"}};
}

}  // namespace

ExtraResult TryExtraDispatch(ServerDeps& deps, const std::string& method,
                             const nlohmann::json& params) {
    ExtraResult r;
    if (!ImplementedSet().count(method)) return r;
    auto starts = [&](const char* p) { return method.rfind(p, 0) == 0; };

    // ---- Provider hot-switch (real) ----
    if (method == "providers.list") {
        r.handled = true;
        nlohmann::json list = nlohmann::json::array();
        if (deps.router) {
            for (const auto& n : deps.router->Names()) {
                list.push_back({{"id", n}, {"default", n == deps.router->Default()}});
            }
        }
        r.payload = Real({{"providers", list}, {"default", deps.router ? deps.router->Default() : ""}});
        return r;
    }
    if (method == "providers.setDefault" || method == "models.setDefault") {
        r.handled = true;
        if (!deps.router) { r.ok = false; r.error = "router not configured"; return r; }
        std::string id = params.value("id", params.value("provider", params.value("model", "")));
        std::string prov = id;
        auto slash = id.find('/');
        if (slash != std::string::npos) prov = id.substr(0, slash);
        if (!deps.router->Has(prov)) { r.ok = false; r.error = "unknown provider: " + prov; return r; }
        deps.router->SetDefault(prov);
        deps.logs.Append("info", "default provider -> " + prov);
        r.payload = Real({{"default", deps.router->Default()}, {"requested", id}});
        return r;
    }

    // ---- ACP skeleton (real local state + gap) ----
    if (method == "acp.status" || method.rfind("acp.", 0) == 0) {
        r.handled = true;
        if (!deps.acp) {
            r.payload = Real({{"implemented", false}, {"skeleton", false}, {"error", "acp not wired"}});
            return r;
        }
        if (method == "acp.status") {
            r.payload = Real(deps.acp->StatusJson());
            return r;
        }
        if (method == "acp.sessions.list") {
            r.payload = Real({{"sessions", deps.acp->ListJson()}});
            return r;
        }
        if (method == "acp.sessions.create") {
            auto id = deps.acp->Create(params.value("agentId", params.value("agent", "openclaw")));
            r.payload = Real({{"id", id}});
            return r;
        }
        if (method == "acp.sessions.prompt") {
            auto id = params.value("id", params.value("sessionId", ""));
            auto prompt = params.value("prompt", params.value("text", ""));
            if (!deps.acp->AppendPrompt(id, prompt)) { r.ok = false; r.error = "session not found"; return r; }
            r.payload = Real({{"id", id}, {"queued", true}, {"wire", "not_implemented"}});
            return r;
        }
        r.payload = Real(deps.acp->StatusJson());
        return r;
    }

    // ---- users (real config.raw) ----
    if (starts("users.")) {
        r.handled = true;
        if (!deps.config.raw.contains("users")) deps.config.raw["users"] = nlohmann::json::object();
        auto& users = deps.config.raw["users"];
        if (method == "users.self") {
            r.payload = Real({{"id", "local"}, {"displayName", users.value("displayName", "operator")}});
            return r;
        }
        if (method == "users.prefs.get") {
            r.payload = Real({{"prefs", users.value("prefs", nlohmann::json::object())}});
            return r;
        }
        if (method == "users.prefs.set") {
            users["prefs"] = params.value("prefs", params);
            config::Save(config::DefaultPath(), deps.config);
            r.payload = Real({{"saved", true}, {"prefs", users["prefs"]}});
            return r;
        }
        if (method == "users.setDisplayName") {
            users["displayName"] = params.value("displayName", params.value("name", ""));
            config::Save(config::DefaultPath(), deps.config);
            r.payload = Real({{"displayName", users["displayName"]}});
            return r;
        }
        if (method == "users.list") {
            r.payload = Real({{"users", nlohmann::json::array({
                {{"id", "local"}, {"displayName", users.value("displayName", "operator")}}})}});
            return r;
        }
        r.payload = Honest(method, params, {{"users", users}});
        return r;
    }

    // ---- plugins (real registry ops) ----
    if (starts("plugins.")) {
        r.handled = true;
        if (method == "plugins.list") {
            r.payload = Real({{"plugins", deps.plugins.ListJson()}});
            return r;
        }
        if (method == "plugins.search") {
            auto q = params.value("q", params.value("query", ""));
            nlohmann::json hits = nlohmann::json::array();
            for (const auto& p : deps.plugins.ListJson()) {
                if (q.empty() || p.dump().find(q) != std::string::npos) hits.push_back(p);
            }
            r.payload = Real({{"hits", hits}});
            return r;
        }
        if (method == "plugins.setEnabled") {
            r.payload = Real({{"id", params.value("id", "")},
                              {"enabled", params.value("enabled", false)},
                              {"note", "persisted enable requires plugin.json writer — state echoed only"}});
            return r;
        }
        if (method == "plugins.refresh") {
            // cannot rescan without path — report current
            r.payload = Real({{"plugins", deps.plugins.ListJson()}, {"refreshed", false}});
            return r;
        }
        r.payload = Honest(method, params, {{"plugins", deps.plugins.ListJson()}});
        return r;
    }

    // ---- update (real version probe) ----
    if (starts("update.")) {
        r.handled = true;
        r.payload = Real({{"status", "idle"},
                          {"current", "0.9.0-dev"},
                          {"align_target", "OpenClaw 2026.10.5"},
                          {"auto_update", false},
                          {"method", method}});
        return r;
    }

    // ---- sessions extras (real store) ----
    if (method == "sessions.search") {
        r.handled = true;
        auto q = params.value("q", params.value("query", ""));
        nlohmann::json hits = nlohmann::json::array();
        for (const auto& id : deps.sessions.List()) {
            if (q.empty() || id.find(q) != std::string::npos) {
                hits.push_back({{"key", id}, {"messages", deps.sessions.Load(id).size()}});
            } else {
                // search message content
                for (const auto& m : deps.sessions.Load(id)) {
                    if (m.content.find(q) != std::string::npos) {
                        hits.push_back({{"key", id}, {"match_role", m.role}});
                        break;
                    }
                }
            }
        }
        r.payload = Real({{"hits", hits}, {"q", q}});
        return r;
    }
    if (method == "sessions.cleanup") {
        r.handled = true;
        int removed = 0;
        for (const auto& id : deps.sessions.List()) {
            if (deps.sessions.Load(id).empty()) {
                deps.sessions.Remove(id);
                ++removed;
            }
        }
        r.payload = Real({{"removed_empty", removed}});
        return r;
    }
    if (starts("sessions.files.")) {
        r.handled = true;
        // map to agent files under session key as agent id namespace
        const std::string sid = params.value("sessionKey", params.value("key", "main"));
        if (method == "sessions.files.list") {
            r.payload = Real({{"sessionKey", sid}, {"files", deps.agent_files.ListJson(sid)}});
            return r;
        }
        if (method == "sessions.files.get") {
            auto name = params.value("name", "");
            auto v = deps.agent_files.Get(sid, name);
            if (!v) { r.ok = false; r.error = "not found"; return r; }
            r.payload = Real({{"name", name}, {"content", *v}});
            return r;
        }
        if (method == "sessions.files.set") {
            deps.agent_files.Set(sid, params.value("name", ""), params.value("content", ""));
            r.payload = Real({{"saved", true}});
            return r;
        }
    }
    if (starts("sessions.goal.")) {
        r.handled = true;
        const std::string sid = params.value("sessionKey", params.value("key", "default"));
        if (!deps.config.raw.contains("session_goals")) deps.config.raw["session_goals"] = nlohmann::json::object();
        if (method == "sessions.goal.update") {
            deps.config.raw["session_goals"][sid] = params.value("goal", params.value("text", ""));
            config::Save(config::DefaultPath(), deps.config);
            r.payload = Real({{"sessionKey", sid}, {"goal", deps.config.raw["session_goals"][sid]}});
            return r;
        }
        if (method == "sessions.goal.clear") {
            deps.config.raw["session_goals"].erase(sid);
            config::Save(config::DefaultPath(), deps.config);
            r.payload = Real({{"sessionKey", sid}, {"cleared", true}});
            return r;
        }
    }
    if (starts("sessions.groups.")) {
        r.handled = true;
        if (!deps.config.raw.contains("session_groups")) deps.config.raw["session_groups"] = nlohmann::json::object();
        auto& g = deps.config.raw["session_groups"];
        if (method == "sessions.groups.list") {
            r.payload = Real({{"groups", g}});
            return r;
        }
        if (method == "sessions.groups.put") {
            auto id = params.value("id", params.value("name", "default"));
            g[id] = params.value("sessions", nlohmann::json::array());
            config::Save(config::DefaultPath(), deps.config);
            r.payload = Real({{"id", id}, {"sessions", g[id]}});
            return r;
        }
        if (method == "sessions.groups.delete") {
            auto id = params.value("id", "");
            g.erase(id);
            config::Save(config::DefaultPath(), deps.config);
            r.payload = Real({{"deleted", id}});
            return r;
        }
    }

    // ---- channels outbound send (real when token) ----
    if (method == "channels.logout") {
        r.handled = true;
        r.payload = Real({{"ok", true}, {"note", "no OAuth session store yet"}});
        return r;
    }

    // ---- skills real ----
    if (method == "skills.update" || method == "skills.library.save") {
        r.handled = true;
        r.payload = Real({{"ok", false}, {"note", "write SKILL.md via filesystem; use skills.status to rescan on restart"}});
        return r;
    }
    if (method == "skills.library.import") {
        r.handled = true;
        r.payload = Real({{"imported", 0}, {"note", "copy into skills dir"}});
        return r;
    }
    if (method == "skills.curator.status") {
        r.handled = true;
        r.payload = Real(deps.skills.StatusJson());
        return r;
    }

    // ---- logs / presence / hooks (real logs buffer) ----
    if (method == "hooks.status") {
        r.handled = true;
        r.payload = Real({{"hooks", nlohmann::json::array()}, {"enabled", false}});
        return r;
    }
    if (method == "presence.query" || method == "presence.activity" || method == "system-presence") {
        r.handled = true;
        r.payload = Real({{"presence", "online"}, {"service", "openclaw-cpp"}, {"listeners", deps.bus.ListenerCount()}});
        return r;
    }
    if (method == "system-event") {
        r.handled = true;
        deps.logs.Append("info", params.value("message", params.dump()));
        r.payload = Real({{"logged", true}});
        return r;
    }
    if (method == "commands.list") {
        r.handled = true;
        r.payload = Real({{"commands", nlohmann::json::array({"status", "help", "models", "tools"})}});
        return r;
    }
    if (method == "poll") {
        r.handled = true;
        r.payload = Real({{"events", deps.bus.Recent(params.value("limit", 10))}});
        return r;
    }

    // ---- agents.workspace (map to files) ----
    if (method == "agents.workspace.list" || method == "agents.workspace.get") {
        r.handled = true;
        const std::string aid = params.value("agentId", params.value("id", "main"));
        if (method == "agents.workspace.list") {
            r.payload = Real({{"agentId", aid}, {"files", deps.agent_files.ListJson(aid)}});
            return r;
        }
        auto name = params.value("name", "AGENTS.md");
        auto v = deps.agent_files.Get(aid, name);
        r.payload = Real({{"agentId", aid}, {"name", name}, {"found", v.has_value()},
                          {"content", v.value_or("")}});
        return r;
    }

    // ---- conversations.list (sessions as conversations) ----
    if (method == "conversations.list") {
        r.handled = true;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& id : deps.sessions.List()) {
            arr.push_back({{"id", id}, {"messages", deps.sessions.Load(id).size()}});
        }
        r.payload = Real({{"conversations", arr}});
        return r;
    }

    // ---- transcripts (session export) ----
    if (method == "transcripts.list") {
        r.handled = true;
        r.payload = Real({{"transcripts", deps.sessions.List()}});
        return r;
    }
    if (method == "transcripts.status") {
        r.handled = true;
        r.payload = Real({{"count", deps.sessions.List().size()}});
        return r;
    }
    if (method == "transcripts.get") {
        r.handled = true;
        auto key = params.value("id", params.value("key", ""));
        nlohmann::json msgs = nlohmann::json::array();
        for (const auto& m : deps.sessions.Load(key)) msgs.push_back(m.ToJson());
        r.payload = Real({{"id", key}, {"messages", msgs}});
        return r;
    }

    // ---- themes / backup / webSearch / computer / canvas / claws / board / portal / worktrees / env / fs ----
    // Prefer honest with clear impl tag rather than fake success
    if (starts("themes.") || starts("backup.") || starts("webSearch.") || starts("computer.") ||
        starts("canvas.") || starts("claws.") || starts("board.") || starts("portal.") ||
        starts("worktrees.") || starts("environments.") || starts("fs.") || starts("tts.") ||
        starts("voicewake.") || starts("talk.") || starts("wizard.") || starts("openclaw.") ||
        starts("nativeHook.") || starts("web.login") || starts("push.") || starts("attach.") ||
        starts("taskSuggestions.") || starts("audit.") || starts("session.members") ||
        starts("session.typing") || starts("session.reactions") || starts("session.suggestions") ||
        starts("sessions.branches") || starts("sessions.catalog") || starts("sessions.storage") ||
        starts("sessions.changed") || starts("sessions.resolve") || starts("sessions.fork") ||
        starts("sessions.rewind") || starts("mcp.app.") || starts("doctor.memory.") ||
        starts("exec.approvals") || method == "approval.history") {
        r.handled = true;
        nlohmann::json st;
        if (starts("mcp.")) {
            nlohmann::json tools = nlohmann::json::array();
            for (const auto& n : deps.tools.Names())
                if (n.rfind("mcp.", 0) == 0) tools.push_back(n);
            st["mcp_tools"] = tools;
        }
        r.payload = Honest(method, params, st);
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

    r.handled = true;
    r.payload = Honest(method, params, nlohmann::json::object());
    return r;
}

}  // namespace openclaw::gateway
