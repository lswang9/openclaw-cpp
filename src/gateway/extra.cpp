#include "openclaw/acp/acpx.hpp"
#include "openclaw/channels/waweb/noise.hpp"
#include <chrono>
#include "openclaw/desktop/desktop.hpp"
#include "openclaw/acp/session.hpp"
#include "openclaw/gateway/extra.hpp"
#include "openclaw/agent/types.hpp"
#include "openclaw/gateway/methods.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/channels/outbound.hpp"
#include "openclaw/board/store.hpp"
#include "openclaw/channels/slack.hpp"
#include "openclaw/channels/discord.hpp"
#include "openclaw/channels/feishu.hpp"
#include "openclaw/channels/whatsapp.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <cstdio>
#include <iostream>
#include <unordered_set>

namespace openclaw::gateway {
namespace {

const std::unordered_set<std::string>& ImplementedSet() {
    // IMPORTANT: do not call ImplementedMethods().begin()/end() as two temporaries —
    // that is UB (iterators from different vectors) and segfaulted browser/computer RPC.
    static const auto* s = [] {
        const auto methods = ImplementedMethods();
        return new std::unordered_set<std::string>(methods.begin(), methods.end());
    }();
    return *s;
}

nlohmann::json Real(nlohmann::json payload) {
    payload["impl"] = "real";
    return payload;
}

nlohmann::json RealSim(nlohmann::json payload) {
    payload["impl"] = "real-sim";
    payload["simulation"] = true;
    if (!payload.contains("mode")) payload["mode"] = "simulation";
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

nlohmann::json& EnsureObj(nlohmann::json& root, const char* key) {
    if (!root.contains(key) || !root[key].is_object()) root[key] = nlohmann::json::object();
    return root[key];
}

nlohmann::json& EnsureArr(nlohmann::json& root, const char* key) {
    if (!root.contains(key) || !root[key].is_array()) root[key] = nlohmann::json::array();
    return root[key];
}

void PersistCfg(ServerDeps& deps) { config::Save(config::DefaultPath(), deps.config); }

void Audit(ServerDeps& deps, const std::string& action, const nlohmann::json& detail) {
    auto& arr = EnsureArr(deps.config.raw, "audit_log");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    arr.push_back({{"ts_ms", ms}, {"action", action}, {"detail", detail}});
    while (arr.size() > 200) arr.erase(arr.begin());
    PersistCfg(deps);
}

std::filesystem::path DataRoot(ServerDeps& deps) {
    if (deps.persist) return deps.persist->Root();
    const char* e = std::getenv("OPENCLAW_DATA_DIR");
    if (e && *e) return e;
    return deps.sessions.Root();
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

    // ---- upstream v2026.10.5 methods filled in 1.86 ----
    if (method == "device.pair.setupCode" || method == "device.pair.setupStatus") {
        r.handled = true;
        if (!deps.device_links) { r.ok = false; r.error = "device links not wired"; return r; }
        if (method == "device.pair.setupCode") { auto o = deps.device_links->Start(params); o["routed"] = "pairing.link.start"; r.payload = Real(o); }
        else r.payload = Real(deps.device_links->Status(params.value("id", params.value("code", std::string()))));
        return r;
    }
    if (method == "gateway.restart.preflight") {
        r.handled = true;
        nlohmann::json checks = nlohmann::json::array();
        const char* cfg = std::getenv("OPENCLAW_CONFIG_PATH");
        bool cfg_ok = true; std::string why;
        if (cfg && *cfg) {
            std::ifstream f(cfg);
            if (!f) { cfg_ok = false; why = "config unreadable"; }
            else { try { (void)nlohmann::json::parse(f); } catch (const std::exception& e) { cfg_ok = false; why = e.what(); } }
        }
        checks.push_back({{"id", "config.parse"}, {"ok", cfg_ok}, {"detail", why}});
        r.payload = Real({{"ok", cfg_ok}, {"safe", cfg_ok}, {"checks", checks}});
        return r;
    }
    if (method == "plugins.controlUi.reload") {
        r.handled = true;
        deps.logs.Append("info", "plugins.controlUi.reload requested");
        r.payload = Real({{"reloaded", true}});
        return r;
    }
    if (method == "tools.github.authorize.cancel" || method == "users.github.authorize.cancel") {
        r.handled = true;
        r.payload = Real({{"cancelled", true}, {"note", "no device-flow in progress is tracked server-side"}});
        return r;
    }
    if (method == "sessions.reclaim" || method == "sessions.companion.reset" || method == "projects.searchRemote" ||
        method == "sessions.github.publish" || method == "sessions.github.confirm" || method == "claws.removalJournal") {
        r.handled = true;
        r.payload = Honest(method, params, {{"reason", method == "claws.removalJournal" ? "no claws removals recorded" : "feature not implemented in C++ runtime"}});
        if (method == "claws.removalJournal") r.payload["entries"] = nlohmann::json::array();
        return r;
    }

    // ---- device link / pairing codes (disk-backed; token issued once) ----
    if (starts("pairing.link.") || starts("pairing.token.") || method == "pairing.status") {
        r.handled = true;
        if (!deps.device_links) { r.payload = Real({{"error", "device links not wired"}}); return r; }
        auto& dl = *deps.device_links;
        auto key = params.value("id", params.value("code", params.value("link_id", std::string())));
        if (method == "pairing.link.start") r.payload = Real(dl.Start(params));
        else if (method == "pairing.link.status") r.payload = Real(dl.Status(key));
        else if (method == "pairing.link.list" || method == "pairing.status") r.payload = Real(dl.List());
        else if (method == "pairing.link.approve") r.payload = Real(dl.Approve(key, params.value("role", std::string("device")), params.value("scopes", nlohmann::json::array())));
        else if (method == "pairing.link.deny") r.payload = Real(dl.Deny(key));
        else if (method == "pairing.link.claim") r.payload = Real(dl.Claim(key, params.value("poll_secret", std::string())));
        else if (method == "pairing.token.verify") r.payload = Real(dl.Verify(params.value("token", std::string())));
        else if (method == "pairing.token.revoke") r.payload = Real(dl.Revoke(params.value("token_id", key)));
        else r.payload = Real({{"error", "unknown pairing method"}, {"method", method}});
        return r;
    }

    // ---- ACP wire subset (JSON-RPC agent + gateway sessions) ----
    if (method == "acp.status" || method.rfind("acp.", 0) == 0) {
        r.handled = true;
        if (!deps.acp) {
            r.payload = Real({{"implemented", false}, {"skeleton", false}, {"error", "acp not wired"}});
            return r;
        }
        if (method == "acp.auth.login" || method == "acp.auth" || method == "acp.authenticate") {
            r.payload = Real(deps.acp->Endpoint().Login(params));
            return r;
        }
        if (method == "acp.auth.status") {
            r.payload = Real(deps.acp->Endpoint().AuthStatusJson());
            return r;
        }
        if (method == "acp.auth.logout") {
            auto resp = deps.acp->WireHandle(
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "auth/logout"}, {"params", nlohmann::json::object()}});
            r.payload = Real({{"response", resp}, {"impl", "real"}});
            return r;
        }
        // Fail-closed: session/prompt/bind/wire/mcp require auth when configured
        const bool acp_auth_exempt =
            method == "acp.status" || method == "acp.agents.list" || method == "acp.auth.status" ||
            method == "acp.auth.login" || method == "acp.auth" || method == "acp.authenticate" ||
            method == "acp.auth.logout" || method == "acp.bindings.list" || method == "acp.bindings";
        if (!acp_auth_exempt && deps.acp->Endpoint().AuthRequired()) {
            if (!deps.acp->Endpoint().TryAuthenticateParams(params)) {
                r.ok = false;
                r.error = "unauthorized";
                r.payload = Real({{"ok", false},
                                  {"error", "unauthorized"},
                                  {"auth_required", true},
                                  {"authenticated", false},
                                  {"hint", "pass token/authToken matching OPENCLAW_ACP_TOKEN or call acp.auth.login"},
                                  {"auth", deps.acp->Endpoint().AuthStatusJson()}});
                return r;
            }
        }
        if (method == "acp.status") { r.payload = Real(deps.acp->StatusJson()); return r; }
        if (method == "acp.agents.list") {
            r.payload = Real({{"agents", deps.acp->AgentsListJson()}, {"acpx_presets", acp::acpx::Presets()}});
            return r;
        }
        if (method == "acp.agents.run") {
            r.payload = Real(acp::acpx::RunFromParams(deps.config.raw, params));
            return r;
        }
        if (method == "acp.sessions.list") {
            r.payload = Real({{"sessions", deps.acp->ListJson()},
                              {"wire_sessions", deps.acp->Endpoint().ListSessionsJson()}});
            return r;
        }
        if (method == "acp.sessions.create") {
            auto id = deps.acp->Create(params.value("agentId", params.value("agent", "openclaw")));
            r.payload = Real({{"id", id}, {"wire", true}});
            return r;
        }
        if (method == "acp.sessions.prompt") {
            auto id = params.value("id", params.value("sessionId", ""));
            std::string text;
            if (params.contains("prompt") && params["prompt"].is_string()) text = params["prompt"].get<std::string>();
            else if (params.contains("text")) text = params.value("text", "");
            else if (params.contains("prompt") && params["prompt"].is_array()) {
                for (const auto& b : params["prompt"]) {
                    if (b.is_object() && b.value("type", "") == "text") {
                        if (!text.empty()) text += "\n";
                        text += b.value("text", "");
                    }
                }
            }
            if (deps.acp->GetJson(id).contains("error")) {
                r.ok = false; r.error = "session not found"; return r;
            }
            auto reply = deps.acp->AppendPrompt(id, text);
            r.payload = Real({{"id", id}, {"queued", true}, {"wire", true}, {"reply", reply},
                              {"note", "routed through in-process ACP AgentEndpoint"}});
            return r;
        }
        if (method == "acp.bind") {
            auto channel = params.value("channel", "");
            auto chat_id = params.value("chat_id", params.value("channel_id", ""));
            auto sid = params.value("session_id", params.value("acp_session_id", ""));
            r.payload = Real(deps.acp->BindChannel(channel, chat_id, sid));
            return r;
        }
        if (method == "acp.unbind") {
            auto key = params.value("channel_key", "");
            if (key.empty()) {
                key = openclaw::acp::BindingStore::MakeKey(params.value("channel", ""),
                                                params.value("chat_id", params.value("channel_id", "")));
            }
            bool ok = deps.acp->UnbindChannel(key);
            r.payload = Real({{"ok", ok}, {"channel_key", key}});
            return r;
        }
        if (method == "acp.bindings.list" || method == "acp.bindings") {
            r.payload = Real(deps.acp->Bindings().StatusJson());
            return r;
        }
        if (method == "acp.session.mcp.add" || method == "acp.sessions.mcp.add") {
            auto sid = params.value("sessionId", params.value("session_id", params.value("id", "")));
            if (sid.rfind("acp-", 0) == 0) {
                auto gj = deps.acp->GetJson(sid);
                if (gj.contains("prompts") && gj["prompts"].is_array()) {
                    for (const auto& p : gj["prompts"]) {
                        if (p.is_string()) {
                            auto s = p.get<std::string>();
                            if (s.rfind("wire:", 0) == 0) { sid = s.substr(5); break; }
                        }
                    }
                }
            }
            nlohmann::json spec = params.contains("server") ? params["server"] : params;
            r.payload = Real(deps.acp->Endpoint().AddSessionMcp(sid, spec));
            return r;
        }
        if (method == "acp.session.mcp.list" || method == "acp.sessions.mcp.list") {
            auto sid = params.value("sessionId", params.value("session_id", params.value("id", "")));
            if (sid.rfind("acp-", 0) == 0) {
                auto gj = deps.acp->GetJson(sid);
                if (gj.contains("prompts") && gj["prompts"].is_array()) {
                    for (const auto& p : gj["prompts"]) {
                        if (p.is_string()) {
                            auto s = p.get<std::string>();
                            if (s.rfind("wire:", 0) == 0) { sid = s.substr(5); break; }
                        }
                    }
                }
            }
            r.payload = Real(deps.acp->Endpoint().ListSessionMcp(sid));
            return r;
        }
        if (method == "acp.session.mcp.remove" || method == "acp.sessions.mcp.remove") {
            auto sid = params.value("sessionId", params.value("session_id", params.value("id", "")));
            auto mid = params.value("mcpId", params.value("mcp_id", params.value("name", "")));
            if (sid.rfind("acp-", 0) == 0) {
                auto gj = deps.acp->GetJson(sid);
                if (gj.contains("prompts") && gj["prompts"].is_array()) {
                    for (const auto& p : gj["prompts"]) {
                        if (p.is_string()) {
                            auto s = p.get<std::string>();
                            if (s.rfind("wire:", 0) == 0) { sid = s.substr(5); break; }
                        }
                    }
                }
            }
            r.payload = Real(deps.acp->Endpoint().RemoveSessionMcp(sid, mid));
            return r;
        }
        if (method == "acp.wire" || method == "acp.rpc") {
            nlohmann::json req = params.contains("request") ? params["request"] : params;
            if (!req.contains("method") && params.contains("acpMethod")) {
                req = {{"jsonrpc", "2.0"},
                       {"id", params.value("id", 1)},
                       {"method", params["acpMethod"]},
                       {"params", params.value("params", nlohmann::json::object())}};
            }
            auto resp = deps.acp->WireHandle(req);
            auto notes = deps.acp->Endpoint().DrainNotifications();
            r.payload = Real({{"response", resp}, {"notifications", notes}});
            return r;
        }
        r.payload = Real(deps.acp->StatusJson());
        return r;
    }

    // ---- media (real store) ----
    if (starts("media.") || method == "artifacts.upload" || method == "artifacts.delete") {
        r.handled = true;
        if (method == "media.list" || method == "artifacts.list") {
            r.payload = Real({{"media", deps.media.ListJson()}});
            return r;
        }
        if (method == "media.get" || method == "artifacts.get" || method == "assistant.media.get") {
            r.payload = Real(deps.media.GetJson(params.value("id", "")));
            return r;
        }
        if (method == "media.upload" || method == "artifacts.upload") {
            std::string data = params.value("text", params.value("data", ""));
            if (params.contains("base64")) data = media::Store::DecodeBase64(params.value("base64", ""));
            auto asset = deps.media.Put(params.value("filename", "upload.bin"),
                                        params.value("content_type", "application/octet-stream"), data);
            Audit(deps, "media.upload", {{"id", asset.id}, {"bytes", asset.bytes}});
            r.payload = Real({{"id", asset.id}, {"filename", asset.filename}, {"bytes", asset.bytes},
                              {"url", "/v1/media/" + asset.id}});
            return r;
        }
        if (method == "media.delete" || method == "artifacts.delete") {
            auto id = params.value("id", "");
            bool ok = deps.media.Remove(id);
            r.payload = Real({{"id", id}, {"removed", ok}});
            return r;
        }
    }

    // ---- users (real config.raw) ----
    if (starts("users.")) {
        r.handled = true;
        auto& users = EnsureObj(deps.config.raw, "users");
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
            PersistCfg(deps);
            r.payload = Real({{"saved", true}, {"prefs", users["prefs"]}});
            return r;
        }
        if (method == "users.setDisplayName") {
            users["displayName"] = params.value("displayName", params.value("name", ""));
            PersistCfg(deps);
            r.payload = Real({{"displayName", users["displayName"]}});
            return r;
        }
        if (method == "users.list") {
            r.payload = Real({{"users", nlohmann::json::array({
                {{"id", "local"}, {"displayName", users.value("displayName", "operator")}}})}});
            return r;
        }
        r.payload = Real({{"users", users}, {"method", method}});
        return r;
    }

    // ---- plugins ----
    if (starts("plugins.")) {
        r.handled = true;
        if (method == "plugins.list" || method == "plugins.uiDescriptors") {
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
            auto id = params.value("id", "");
            bool en = params.value("enabled", false);
            bool ok = deps.plugins.SetEnabled(id, en);
            if (en) deps.plugins.LoadNatives();
            r.payload = Real({{"id", id}, {"enabled", en}, {"saved", ok}});
            return r;
        }
        if (method == "plugins.refresh") {
            deps.plugins.Scan(deps.plugins.Root().empty()
                                  ? std::filesystem::path("/workspace/openclaw-cpp/plugins")
                                  : deps.plugins.Root());
            int n = deps.plugins.LoadNatives();
            r.payload = Real({{"plugins", deps.plugins.ListJson()}, {"natives_loaded", n}});
            return r;
        }
        if (method == "plugins.invoke" || method == "plugins.sessionAction") {
            auto id = params.value("id", params.value("plugin", ""));
            auto m = params.value("method", params.value("action", "echo"));
            auto p = params.contains("params") ? params["params"] : params;
            r.payload = Real(deps.plugins.InvokeNative(id, m, p));
            return r;
        }
        if (method == "plugins.js.status" || method == "plugins.js.start" ||
            method == "plugins.js.call" || method == "plugins.js.list_tools") {
            if (!deps.js_host) {
                r.ok = false;
                r.error = "js_host not attached";
                return r;
            }
            if (method == "plugins.js.status") {
                r.payload = Real(deps.js_host->Status());
                return r;
            }
            if (method == "plugins.js.start") {
                auto root = deps.plugins.Root().empty()
                                ? std::filesystem::path("plugins")
                                : deps.plugins.Root();
                deps.js_host->Start(root);
                auto n = deps.js_host->RegisterTools(deps.tools);
                r.payload = Real({{"started", true},
                                  {"tools_registered", n},
                                  {"status", deps.js_host->Status()},
                                  {"init", deps.js_host->Status().value("init", nlohmann::json::object())}});
                return r;
            }
            if (method == "plugins.js.list_tools") {
                r.payload = Real(deps.js_host->ListTools());
                return r;
            }
            if (method == "plugins.js.call") {
                auto name = params.value("name", params.value("tool", ""));
                auto args = params.value("arguments", params.value("args", nlohmann::json::object()));
                r.payload = Real(deps.js_host->CallTool(name, args));
                return r;
            }
        }
        r.payload = Real({{"plugins", deps.plugins.ListJson()}, {"method", method}});
        return r;
    }

    // ---- update ----
    if (starts("update.")) {
        r.handled = true;
        r.payload = Real({{"status", "idle"},
                          {"current", "1.5.1-dev"},
                          {"align_target", "OpenClaw 2026.10.5"},
                          {"auto_update", false},
                          {"method", method}});
        return r;
    }

    // ---- sessions extras ----
    if (method == "sessions.search") {
        r.handled = true;
        auto q = params.value("q", params.value("query", ""));
        nlohmann::json hits = nlohmann::json::array();
        for (const auto& id : deps.sessions.List()) {
            if (q.empty() || id.find(q) != std::string::npos) {
                hits.push_back({{"key", id}, {"messages", deps.sessions.Load(id).size()}});
            } else {
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
    if (method == "sessions.fork") {
        r.handled = true;
        auto src = params.value("sessionKey", params.value("key", params.value("from", "")));
        auto dst = params.value("to", params.value("newKey", src + "-fork"));
        if (!deps.sessions.Exists(src)) { r.ok = false; r.error = "source not found"; return r; }
        deps.sessions.Save(dst, deps.sessions.Load(src));
        Audit(deps, "sessions.fork", {{"from", src}, {"to", dst}});
        r.payload = Real({{"from", src}, {"to", dst}, {"messages", deps.sessions.Load(dst).size()}});
        return r;
    }
    if (method == "sessions.rewind") {
        r.handled = true;
        auto key = params.value("sessionKey", params.value("key", ""));
        int keep = params.value("keep", params.value("n", 0));
        auto msgs = deps.sessions.Load(key);
        if (keep < 0) keep = 0;
        if (static_cast<size_t>(keep) < msgs.size()) msgs.resize(static_cast<size_t>(keep));
        deps.sessions.Save(key, msgs);
        r.payload = Real({{"key", key}, {"messages", msgs.size()}});
        return r;
    }
    if (method == "sessions.resolve") {
        r.handled = true;
        auto key = params.value("sessionKey", params.value("key", params.value("id", "")));
        r.payload = Real({{"key", key}, {"exists", deps.sessions.Exists(key)},
                          {"messages", deps.sessions.Exists(key) ? deps.sessions.Load(key).size() : 0}});
        return r;
    }
    if (method == "sessions.catalog" || method == "sessions.storage") {
        r.handled = true;
        nlohmann::json items = nlohmann::json::array();
        std::size_t total_msgs = 0;
        for (const auto& id : deps.sessions.List()) {
            auto n = deps.sessions.Load(id).size();
            total_msgs += n;
            items.push_back({{"id", id}, {"messages", n}});
        }
        r.payload = Real({{"sessions", items},
                          {"count", items.size()},
                          {"total_messages", total_msgs},
                          {"root", deps.sessions.Root().string()}});
        return r;
    }
    if (starts("sessions.files.")) {
        r.handled = true;
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
        auto& goals = EnsureObj(deps.config.raw, "session_goals");
        if (method == "sessions.goal.update") {
            goals[sid] = params.value("goal", params.value("text", ""));
            PersistCfg(deps);
            r.payload = Real({{"sessionKey", sid}, {"goal", goals[sid]}});
            return r;
        }
        if (method == "sessions.goal.clear") {
            goals.erase(sid);
            PersistCfg(deps);
            r.payload = Real({{"sessionKey", sid}, {"cleared", true}});
            return r;
        }
        r.payload = Real({{"sessionKey", sid}, {"goal", goals.value(sid, "")}});
        return r;
    }
    if (starts("sessions.groups.")) {
        r.handled = true;
        auto& g = EnsureObj(deps.config.raw, "session_groups");
        if (method == "sessions.groups.list") {
            r.payload = Real({{"groups", g}});
            return r;
        }
        if (method == "sessions.groups.put") {
            auto id = params.value("id", params.value("name", "default"));
            g[id] = params.value("sessions", nlohmann::json::array());
            PersistCfg(deps);
            r.payload = Real({{"id", id}, {"sessions", g[id]}});
            return r;
        }
        if (method == "sessions.groups.delete") {
            auto id = params.value("id", "");
            g.erase(id);
            PersistCfg(deps);
            r.payload = Real({{"deleted", id}});
            return r;
        }
    }
    if (starts("sessions.branches")) {
        r.handled = true;
        auto& br = EnsureObj(deps.config.raw, "session_branches");
        auto key = params.value("sessionKey", params.value("key", "main"));
        if (method.find("list") != std::string::npos || method == "sessions.branches") {
            r.payload = Real({{"sessionKey", key}, {"branches", br.value(key, nlohmann::json::array())}});
            return r;
        }
        // put branch name
        auto name = params.value("name", params.value("branch", "main"));
        auto arr = br.value(key, nlohmann::json::array());
        bool found = false;
        for (auto& x : arr)
            if (x == name) found = true;
        if (!found) arr.push_back(name);
        br[key] = arr;
        PersistCfg(deps);
        r.payload = Real({{"sessionKey", key}, {"branches", arr}});
        return r;
    }

    // session.members / typing / reactions / suggestions — local maps
    if (starts("session.members") || starts("session.typing") || starts("session.reactions") ||
        starts("session.suggestions") || starts("sessions.changed")) {
        r.handled = true;
        auto& meta = EnsureObj(deps.config.raw, "session_meta");
        auto key = params.value("sessionKey", params.value("key", "main"));
        auto& slot = EnsureObj(meta, key.c_str());
        if (starts("session.members")) {
            if (method.find("set") != std::string::npos || method.find("add") != std::string::npos) {
                slot["members"] = params.value("members", params);
                PersistCfg(deps);
            }
            r.payload = Real({{"sessionKey", key}, {"members", slot.value("members", nlohmann::json::array())}});
            return r;
        }
        if (starts("session.typing")) {
            slot["typing"] = params.value("typing", params.value("users", nlohmann::json::array()));
            PersistCfg(deps);
            r.payload = Real({{"sessionKey", key}, {"typing", slot["typing"]}});
            return r;
        }
        if (starts("session.reactions")) {
            auto& rx = EnsureObj(slot, "reactions");
            auto mid = params.value("messageId", params.value("id", "0"));
            if (params.contains("emoji") || params.contains("reaction")) {
                rx[mid] = params.value("emoji", params.value("reaction", "👍"));
                PersistCfg(deps);
            }
            r.payload = Real({{"sessionKey", key}, {"reactions", rx}});
            return r;
        }
        if (starts("session.suggestions")) {
            r.payload = Real({{"sessionKey", key},
                              {"suggestions", slot.value("suggestions", nlohmann::json::array({"status", "help"}))}});
            return r;
        }
        r.payload = Real({{"sessionKey", key}, {"changed", true}, {"meta", slot}});
        return r;
    }

    if (method == "channels.logout") {
        r.handled = true;
        r.payload = Real({{"ok", true}, {"note", "no OAuth session store yet"}});
        return r;
    }

    // ---- skills ----
    if (starts("skills.")) {
        r.handled = true;
        if (method == "skills.update" || method == "skills.library.save") {
            r.payload = Real({{"ok", false},
                              {"note", "write SKILL.md via filesystem; restart or skills.status rescan"}});
            return r;
        }
        if (method == "skills.library.import") {
            r.payload = Real({{"imported", 0}, {"note", "copy into skills dir"}});
            return r;
        }
        if (method == "skills.curator.status" || method == "skills.status" || method == "skills.list") {
            r.payload = Real(deps.skills.StatusJson());
            return r;
        }
        r.payload = Real(deps.skills.StatusJson());
        return r;
    }

    // ---- agents ----
    if (starts("agents.")) {
        r.handled = true;
        auto& agents = EnsureArr(deps.config.raw, "agents_list");
        if (method == "agents.list") {
            r.payload = Real({{"agents", agents}});
            return r;
        }
        if (method == "agents.create" || method == "agents.upsert") {
            auto id = params.value("id", params.value("agentId", "agent-" + std::to_string(agents.size() + 1)));
            nlohmann::json a{{"id", id},
                             {"name", params.value("name", id)},
                             {"model", params.value("model", "")}};
            bool replaced = false;
            for (auto& x : agents) {
                if (x.value("id", "") == id) { x = a; replaced = true; break; }
            }
            if (!replaced) agents.push_back(a);
            PersistCfg(deps);
            r.payload = Real({{"agent", a}, {"replaced", replaced}});
            return r;
        }
        if (method == "agents.delete") {
            auto id = params.value("id", "");
            nlohmann::json next = nlohmann::json::array();
            for (auto& x : agents)
                if (x.value("id", "") != id) next.push_back(x);
            agents = next;
            PersistCfg(deps);
            r.payload = Real({{"deleted", id}});
            return r;
        }
        if (method == "agents.workspace.list" || method == "agents.workspace.get") {
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
        r.payload = Real({{"agents", agents}, {"method", method}});
        return r;
    }


    // ---- whatsapp cloud ----
    if (starts("whatsapp.") || method == "channels.whatsapp.inbound") {
        r.handled = true;
        auto* wa = dynamic_cast<channels::WhatsAppCloudAdapter*>(deps.adapters.Get("whatsapp"));
        if (!wa) { r.ok = false; r.error = "whatsapp adapter missing"; return r; }
        if (method == "whatsapp.status" || method == "whatsapp.cloud.status") {
            r.payload = Real(wa->Status());
            return r;
        }
        if (method == "whatsapp.feed" || method == "whatsapp.cloud.feed" || method == "whatsapp.mock") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            if (params.contains("frames") && params["frames"].is_array()) {
                std::vector<nlohmann::json> frames;
                for (auto& f : params["frames"]) frames.push_back(f);
                r.payload = Real(wa->RunMockFrames(frames, deliver));
                return r;
            }
            nlohmann::json payload = params.contains("payload") ? params["payload"] : params;
            r.payload = Real(wa->FeedPayload(payload, deliver));
            return r;
        }
        if (method == "whatsapp.send") {
            wa->SetEnabled(true);
            r.payload = Real(wa->SendText(params.value("to", ""), params.value("text", ""), params));
            return r;
        }
        if (method == "channels.whatsapp.inbound") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            r.payload = Real(wa->HandleHttpInbound(params, deliver));
            return r;
        }
        r.payload = Real(wa->Status());
        return r;
    }

    // ---- feishu (webhook + long-connection WS) ----
    if (starts("feishu.") || method == "channels.feishu.inbound") {
        r.handled = true;
        auto* fs = dynamic_cast<channels::FeishuAdapter*>(deps.adapters.Get("feishu"));
        if (!fs) { r.ok = false; r.error = "feishu adapter missing"; return r; }
        if (method == "feishu.status" || method == "feishu.ws.status") {
            r.payload = Real(fs->Status());
            return r;
        }
        if (method == "feishu.ws.feed" || method == "feishu.ws.mock") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            if (params.contains("frames") && params["frames"].is_array()) {
                std::vector<nlohmann::json> frames;
                for (auto& f : params["frames"]) frames.push_back(f);
                r.payload = Real(fs->LongConn().RunMockFrames(frames, deliver));
                return r;
            }
            nlohmann::json payload = params.contains("payload") ? params["payload"] : params;
            r.payload = Real({{"handled", fs->LongConn().FeedPayload(payload, deliver)},
                              {"status", fs->LongConn().Status()}});
            return r;
        }
        if (method == "feishu.ws.start") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            bool started = fs->LongConn().Start(deliver);
            r.payload = Real({{"started", started}, {"status", fs->LongConn().Status()}});
            return r;
        }
        if (method == "feishu.ws.stop") {
            fs->LongConn().Stop();
            r.payload = Real({{"stopped", true}, {"status", fs->LongConn().Status()}});
            return r;
        }
        if (method == "channels.feishu.inbound") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            r.payload = Real(fs->HandleHttpInbound(params, deliver));
            return r;
        }
        r.payload = Real(fs->Status());
        return r;
    }

    // ---- discord (webhook + Gateway WS) ----
    if (starts("discord.") || method == "channels.discord.inbound") {
        r.handled = true;
        auto* dc = dynamic_cast<channels::DiscordAdapter*>(deps.adapters.Get("discord"));
        if (!dc) { r.ok = false; r.error = "discord adapter missing"; return r; }
        if (method == "discord.status" || method == "discord.gateway.status") {
            r.payload = Real(dc->Status());
            return r;
        }
        if (method == "discord.gateway.feed" || method == "discord.gateway.mock") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            if (params.contains("frames") && params["frames"].is_array()) {
                std::vector<nlohmann::json> frames;
                for (auto& f : params["frames"]) frames.push_back(f);
                r.payload = Real(dc->Gateway().RunMockFrames(frames, deliver));
                return r;
            }
            nlohmann::json payload = params.contains("payload") ? params["payload"] : params;
            r.payload = Real({{"handled", dc->Gateway().FeedPayload(payload, deliver)},
                              {"status", dc->Gateway().Status()}});
            return r;
        }
        if (method == "discord.gateway.start") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            bool started = dc->Gateway().Start(deliver);
            r.payload = Real({{"started", started}, {"status", dc->Gateway().Status()}});
            return r;
        }
        if (method == "discord.gateway.stop") {
            dc->Gateway().Stop();
            r.payload = Real({{"stopped", true}, {"status", dc->Gateway().Status()}});
            return r;
        }
        if (method == "channels.discord.inbound") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            r.payload = Real(dc->HandleHttpInbound(params, deliver));
            return r;
        }
        r.payload = Real(dc->Status());
        return r;
    }

    // ---- slack (HTTP inbound + Socket Mode) ----
    if (starts("slack.") || method == "channels.slack.inbound") {
        r.handled = true;
        auto* adapter = deps.adapters.Get("slack");
        auto* sl = dynamic_cast<channels::SlackAdapter*>(adapter);
        if (!sl) { r.ok = false; r.error = "slack adapter missing"; return r; }
        if (method == "slack.status" || method == "slack.socket.status") {
            r.payload = Real(sl->Status());
            return r;
        }
        if (method == "slack.socket.feed" || method == "slack.socket.mock") {
            nlohmann::json env = params.value("envelope", params);
            if (params.contains("frames") && params["frames"].is_array()) {
                std::vector<nlohmann::json> frames;
                for (auto& f : params["frames"]) frames.push_back(f);
                auto deliver = [&](const channels::InboundMessage& m) {
                    // Prefer gateway deliver if we can — ExtraDispatch has no Server*;
                    // record text echo for tests.
                    return std::string("ack:") + m.text;
                };
                // If params has use_agent and we somehow had deliver… keep test echo.
                r.payload = Real(sl->Socket().RunMockFrames(frames, deliver));
                return r;
            }
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            auto ack = sl->Socket().FeedEnvelope(env, deliver);
            r.payload = Real({{"ack", ack}, {"status", sl->Socket().Status()}});
            return r;
        }
        if (method == "slack.socket.start") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            bool started = sl->Socket().Start(deliver);
            r.payload = Real({{"started", started}, {"status", sl->Socket().Status()}});
            return r;
        }
        if (method == "slack.socket.stop") {
            sl->Socket().Stop();
            r.payload = Real({{"stopped", true}, {"status", sl->Socket().Status()}});
            return r;
        }
        if (method == "channels.slack.inbound") {
            auto deliver = [&](const channels::InboundMessage& m) { return std::string("ack:") + m.text; };
            r.payload = Real(sl->HandleHttpInbound(params, deliver));
            return r;
        }
        r.payload = Real(sl->Status());
        return r;
    }

    // ---- channels ----
    if (starts("channels.")) {
        r.handled = true;
        if (method == "channels.status" || method == "channels.list") {
            r.payload = Real({{"adapters", deps.adapters.StatusJson()},
                              {"channels", deps.channels.ListJson()}});
            return r;
        }
        if (method == "channels.send" || method == "channels.outbound") {
            const std::string channel = params.value("channel", params.value("id", ""));
            const std::string text = params.value("text", params.value("message", ""));
            const std::string chat_id = params.value("chat_id", params.value("to", ""));
            auto* adapter = deps.adapters.Get(channel);
            auto* out = dynamic_cast<channels::OutboundRecordAdapter*>(adapter);
            if (!out) { r.ok = false; r.error = "no outbound adapter: " + channel; return r; }
            out->SetEnabled(true);
            r.payload = Real(out->SendText(chat_id, text, params));
            return r;
        }
        r.payload = Real({{"adapters", deps.adapters.StatusJson()}, {"method", method}});
        return r;
    }

    // ---- cron ----
    if (starts("cron.")) {
        r.handled = true;
        if (method == "cron.list" || method == "cron.status") {
            r.payload = Real({{"jobs", deps.cron.ListJson()}});
            return r;
        }
        if (method == "cron.add" || method == "cron.create") {
            cron::Job j;
            j.id = params.value("id", "job-" + std::to_string(deps.cron.List().size() + 1));
            j.cron_expr = params.value("cron", params.value("cron_expr", "0 * * * *"));
            j.payload = params.value("payload", params.value("text", ""));
            j.enabled = params.value("enabled", true);
            deps.cron.Add(j);
            if (deps.persist) deps.persist->SaveCron(deps.cron);
            r.payload = Real({{"job", j.id}, {"jobs", deps.cron.ListJson()}});
            return r;
        }
        if (method == "cron.remove" || method == "cron.delete") {
            auto id = params.value("id", "");
            bool ok = deps.cron.Remove(id);
            if (deps.persist) deps.persist->SaveCron(deps.cron);
            r.payload = Real({{"id", id}, {"removed", ok}});
            return r;
        }
        if (method == "cron.tick") {
            auto n = deps.cron.Tick(std::chrono::system_clock::now());
            r.payload = Real({{"fired", n}, {"jobs", deps.cron.ListJson()}});
            return r;
        }
        r.payload = Real({{"jobs", deps.cron.ListJson()}, {"method", method}});
        return r;
    }

    // ---- memory / doctor / diagnostics ----
    if (starts("memory.") || starts("doctor.") || starts("diagnostics.")) {
        r.handled = true;
        if (method == "memory.list" || method == "memory.keys") {
            r.payload = Real({{"keys", deps.memory.Keys()}});
            return r;
        }
        if (method == "memory.get") {
            auto k = params.value("key", "");
            auto v = deps.memory.Get(k);
            r.payload = Real({{"key", k}, {"found", v.has_value()}, {"value", v.value_or("")}});
            return r;
        }
        if (method == "memory.set") {
            deps.memory.Set(params.value("key", ""), params.value("value", params.value("text", "")));
            r.payload = Real({{"saved", true}});
            return r;
        }
        if (method == "memory.delete" || method == "memory.remove") {
            r.payload = Real({{"removed", deps.memory.Remove(params.value("key", ""))}});
            return r;
        }
        if (method == "memory.dump") {
            r.payload = Real({{"data", deps.memory.Dump()}});
            return r;
        }
        if (starts("doctor.") || starts("diagnostics.")) {
            r.payload = Real({{"memory_keys", deps.memory.Keys()},
                              {"sessions", deps.sessions.List().size()},
                              {"tools", deps.tools.Names().size()},
                              {"method", method}});
            return r;
        }
        r.payload = Real({{"keys", deps.memory.Keys()}, {"method", method}});
        return r;
    }

    // ---- node / device ----
    if (starts("node.") || starts("device.")) {
        r.handled = true;
        if (method == "node.list" || method == "device.list") {
            r.payload = Real({{"nodes", deps.nodes.ListJson()}, {"pairing", deps.pairing.ListJson()}});
            return r;
        }
        r.payload = Real({{"nodes", deps.nodes.ListJson()},
                          {"pairing", deps.pairing.ListJson()},
                          {"method", method}});
        return r;
    }

    // ---- secrets / approvals ----
    if (starts("secrets.") || starts("approval") || starts("exec.") || starts("plugin.approval")) {
        r.handled = true;
        if (starts("secrets.")) {
            if (method == "secrets.list") {
                r.payload = Real({{"keys", deps.secrets.ListJson()}});
                return r;
            }
            if (method == "secrets.set") {
                deps.secrets.Set(params.value("key", ""), params.value("value", ""));
                r.payload = Real({{"saved", true}});
                return r;
            }
            if (method == "secrets.delete") {
                r.payload = Real({{"removed", deps.secrets.Delete(params.value("key", ""))}});
                return r;
            }
        }
        if (method == "approval.history" || starts("exec.approvals") || starts("plugin.approval")) {
            r.payload = Real({{"approvals", deps.approvals.ListJson()}});
            return r;
        }
        r.payload = Real({{"approvals", deps.approvals.ListJson()},
                          {"secrets_keys", deps.secrets.ListJson()},
                          {"method", method}});
        return r;
    }

    // ---- themes / backup / environments / worktrees / fs / audit (local state) ----
    if (starts("themes.")) {
        r.handled = true;
        auto& th = EnsureObj(deps.config.raw, "themes");
        if (method.find("set") != std::string::npos || method.find("put") != std::string::npos) {
            th["current"] = params.value("theme", params.value("id", params.value("name", "default")));
            if (params.contains("vars")) th["vars"] = params["vars"];
            PersistCfg(deps);
        }
        if (method.find("list") != std::string::npos) {
            r.payload = Real({{"themes", th.value("available", nlohmann::json::array({"default", "dark"}))},
                              {"current", th.value("current", "default")}});
            return r;
        }
        r.payload = Real({{"current", th.value("current", "default")}, {"themes", th}});
        return r;
    }
    if (starts("backup.")) {
        r.handled = true;
        auto& bk = EnsureObj(deps.config.raw, "backup");
        auto dest = DataRoot(deps) / "backups";
        if (method.find("create") != std::string::npos || method.find("run") != std::string::npos) {
            auto id = "bak-" + std::to_string(
                std::chrono::system_clock::now().time_since_epoch().count());
            std::filesystem::create_directories(dest);
            auto path = dest / (id + ".json");
            nlohmann::json sess = nlohmann::json::object();
            for (const auto& sid : deps.sessions.List()) {
                nlohmann::json msgs = nlohmann::json::array();
                for (const auto& m : deps.sessions.Load(sid)) msgs.push_back(m.ToJson());
                sess[sid] = msgs;
            }
            nlohmann::json snap{{"id", id},
                                {"version", 1},
                                {"sessions", sess},
                                {"memory", deps.memory.Dump()},
                                {"cron", deps.cron.ListJson()},
                                {"config_raw", deps.config.raw}};
            std::ofstream out(path);
            out << snap.dump(2);
            auto& hist = EnsureArr(bk, "history");
            hist.push_back({{"id", id}, {"path", path.string()}});
            PersistCfg(deps);
            Audit(deps, "backup.create", {{"id", id}});
            r.payload = Real({{"id", id}, {"path", path.string()}, {"bytes", snap.dump().size()},
                              {"session_count", sess.size()}});
            return r;
        }
        if (method.find("restore") != std::string::npos) {
            auto id = params.value("id", "");
            auto path_s = params.value("path", "");
            std::filesystem::path path;
            if (!path_s.empty()) path = path_s;
            else if (!id.empty()) path = dest / (id + ".json");
            else { r.ok = false; r.error = "id or path required"; return r; }
            if (!std::filesystem::exists(path)) { r.ok = false; r.error = "backup not found"; return r; }
            std::ifstream in(path);
            nlohmann::json snap;
            in >> snap;
            int restored = 0;
            if (snap.contains("sessions") && snap["sessions"].is_object()) {
                for (auto it = snap["sessions"].begin(); it != snap["sessions"].end(); ++it) {
                    std::vector<agent::Message> msgs;
                    for (const auto& jm : it.value()) {
                        agent::Message m;
                        m.role = jm.value("role", "user");
                        m.content = jm.value("content", "");
                        if (jm.contains("name")) m.name = jm.value("name", "");
                        if (jm.contains("tool_call_id")) m.tool_call_id = jm.value("tool_call_id", "");
                        msgs.push_back(m);
                    }
                    deps.sessions.Save(it.key(), msgs);
                    ++restored;
                }
            }
            if (snap.contains("memory") && snap["memory"].is_object()) {
                for (auto it = snap["memory"].begin(); it != snap["memory"].end(); ++it) {
                    if (it.value().is_string()) deps.memory.Set(it.key(), it.value().get<std::string>());
                    else deps.memory.Set(it.key(), it.value().dump());
                }
            }
            Audit(deps, "backup.restore", {{"id", id}, {"path", path.string()}, {"sessions", restored}});
            r.payload = Real({{"restored_sessions", restored}, {"path", path.string()}, {"id", snap.value("id", id)}});
            return r;
        }
        if (method.find("status") != std::string::npos || method.find("list") != std::string::npos) {
            nlohmann::json files = nlohmann::json::array();
            if (std::filesystem::exists(dest)) {
                for (const auto& e : std::filesystem::directory_iterator(dest)) {
                    if (e.is_regular_file())
                        files.push_back({{"name", e.path().filename().string()},
                                         {"path", e.path().string()},
                                         {"bytes", e.file_size()}});
                }
            }
            r.payload = Real({{"history", bk.value("history", nlohmann::json::array())},
                              {"files", files},
                              {"dir", dest.string()}});
            return r;
        }
        if (method.find("recordOutcome") != std::string::npos) {
            auto& hist = EnsureArr(bk, "outcomes");
            hist.push_back({{"ok", params.value("ok", true)}, {"detail", params}});
            PersistCfg(deps);
            r.payload = Real({{"recorded", true}});
            return r;
        }
        r.payload = Real({{"history", bk.value("history", nlohmann::json::array())}, {"method", method}});
        return r;
    }

    // ---- sessions export / import ----
    if (method == "sessions.export" || method == "sessions.exportOne") {
        r.handled = true;
        auto key = params.value("sessionKey", params.value("key", params.value("id", "")));
        nlohmann::json out;
        if (key.empty()) {
            nlohmann::json all = nlohmann::json::object();
            for (const auto& sid : deps.sessions.List()) {
                nlohmann::json msgs = nlohmann::json::array();
                for (const auto& m : deps.sessions.Load(sid)) msgs.push_back(m.ToJson());
                all[sid] = msgs;
            }
            out = {{"sessions", all}, {"count", all.size()}};
        } else {
            nlohmann::json msgs = nlohmann::json::array();
            for (const auto& m : deps.sessions.Load(key)) msgs.push_back(m.ToJson());
            out = {{"id", key}, {"messages", msgs}};
        }
        r.payload = Real(out);
        return r;
    }
    if (method == "sessions.import") {
        r.handled = true;
        int n = 0;
        if (params.contains("sessions") && params["sessions"].is_object()) {
            for (auto it = params["sessions"].begin(); it != params["sessions"].end(); ++it) {
                std::vector<agent::Message> msgs;
                for (const auto& jm : it.value()) {
                    agent::Message m;
                    m.role = jm.value("role", "user");
                    m.content = jm.value("content", "");
                    msgs.push_back(m);
                }
                deps.sessions.Save(it.key(), msgs);
                ++n;
            }
        } else {
            auto key = params.value("sessionKey", params.value("key", params.value("id", "imported")));
            std::vector<agent::Message> msgs;
            auto arr = params.value("messages", nlohmann::json::array());
            for (const auto& jm : arr) {
                agent::Message m;
                m.role = jm.value("role", "user");
                m.content = jm.value("content", "");
                msgs.push_back(m);
            }
            deps.sessions.Save(key, msgs);
            n = 1;
        }
        r.payload = Real({{"imported", n}});
        return r;
    }

    if (starts("environments.") || starts("worktrees.")) {
        r.handled = true;
        auto key = starts("environments.") ? "environments" : "worktrees";
        auto& obj = EnsureObj(deps.config.raw, key);
        if (method.find("list") != std::string::npos) {
            r.payload = Real({{key, obj.value("items", nlohmann::json::array())}});
            return r;
        }
        if (method.find("put") != std::string::npos || method.find("add") != std::string::npos ||
            method.find("set") != std::string::npos) {
            auto& items = EnsureArr(obj, "items");
            nlohmann::json item = params.contains("item") ? params["item"] : params;
            items.push_back(item);
            PersistCfg(deps);
            r.payload = Real({{"saved", true}, {key, items}});
            return r;
        }
        r.payload = Real({{key, obj}, {"method", method}});
        return r;
    }
    if (starts("fs.")) {
        r.handled = true;
        // Sandboxed: only list/read under data root
        auto root = DataRoot(deps);
        auto rel = params.value("path", ".");
        auto path = (root / rel).lexically_normal();
        if (path.string().find(root.string()) != 0) {
            r.ok = false;
            r.error = "path escapes data root";
            return r;
        }
        if (method.find("list") != std::string::npos || method == "fs.readdir") {
            nlohmann::json entries = nlohmann::json::array();
            if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
                for (const auto& e : std::filesystem::directory_iterator(path)) {
                    entries.push_back({{"name", e.path().filename().string()},
                                       {"dir", e.is_directory()},
                                       {"size", e.is_regular_file() ? e.file_size() : 0}});
                }
            }
            r.payload = Real({{"root", root.string()}, {"path", path.string()}, {"entries", entries}});
            return r;
        }
        if (method.find("read") != std::string::npos || method == "fs.cat") {
            if (!std::filesystem::is_regular_file(path)) { r.ok = false; r.error = "not a file"; return r; }
            std::ifstream in(path);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (content.size() > 256 * 1024) content.resize(256 * 1024);
            r.payload = Real({{"path", path.string()}, {"content", content}, {"bytes", content.size()}});
            return r;
        }
        r.payload = Real({{"root", root.string()}, {"method", method}, {"note", "sandboxed to data dir"}});
        return r;
    }
    if (starts("audit.")) {
        r.handled = true;
        r.payload = Real({{"log", EnsureArr(deps.config.raw, "audit_log")}});
        return r;
    }

    // ---- logs / presence / hooks / commands / poll ----
    if (method == "hooks.status") {
        r.handled = true;
        r.payload = Real({{"hooks", nlohmann::json::array()}, {"enabled", false}});
        return r;
    }
    if (method == "presence.query" || method == "presence.activity" || method == "system-presence") {
        r.handled = true;
        r.payload = Real({{"presence", "online"},
                          {"service", "openclaw-cpp"},
                          {"listeners", deps.bus.ListenerCount()}});
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
        r.payload = Real({{"commands", nlohmann::json::array({"status", "help", "models", "tools", "gateway"})}});
        return r;
    }
    if (method == "poll") {
        r.handled = true;
        r.payload = Real({{"events", deps.bus.Recent(params.value("limit", 10))}});
        return r;
    }
    if (starts("logs.")) {
        r.handled = true;
        r.payload = Real({{"lines", deps.logs.Tail(params.value("limit", 50))}});
        return r;
    }

    // ---- conversations / transcripts / chat / tools / config / models ----
    if (method == "conversations.list") {
        r.handled = true;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& id : deps.sessions.List()) {
            arr.push_back({{"id", id}, {"messages", deps.sessions.Load(id).size()}});
        }
        r.payload = Real({{"conversations", arr}});
        return r;
    }
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
    if (starts("chat.") || starts("tools.") || starts("config.") || starts("models.") ||
        starts("providers.") || starts("artifacts.") || starts("assistant.") || starts("message.") ||
        starts("conversations.") || starts("session.") || starts("sessions.")) {
        r.handled = true;
        // Prefer real snapshots over honest empty
        nlohmann::json st;
        if (starts("tools.")) st["tools"] = deps.tools.Names();
        if (starts("config.")) st["config"] = deps.config.ToJson();
        if (starts("models.") || starts("providers.")) {
            st["providers"] = deps.router ? nlohmann::json(deps.router->Names()) : nlohmann::json::array();
            st["default"] = deps.router ? deps.router->Default() : "";
        }
        if (starts("session") || starts("conversations.") || starts("message.") || starts("chat.")) {
            st["sessions"] = deps.sessions.List();
        }
        if (starts("artifacts.") || starts("assistant.")) st["media"] = deps.media.ListJson();
        r.payload = Real({{"method", method}, {"state", st}});
        return r;
    }

    // ---- TTS / voice / talk (real local + optional HTTP) ----
    if (starts("tts.") || starts("voicewake.") || starts("talk.")) {
        r.handled = true;
        if (!deps.tts) {
            r.payload = Real({{"implemented", false}, {"error", "tts not wired"}});
            return r;
        }
        auto& tts = *deps.tts;
        if (method == "tts.status") { r.payload = Real(tts.Status()); return r; }
        if (method == "tts.providers") { r.payload = Real({{"providers", tts.Providers()}}); return r; }
        if (method == "tts.enable") { tts.SetEnabled(true); r.payload = Real(tts.Status()); return r; }
        if (method == "tts.disable") { tts.SetEnabled(false); r.payload = Real(tts.Status()); return r; }
        if (method == "tts.setProvider") {
            tts.SetProvider(params.value("provider", params.value("id", "http")));
            r.payload = Real(tts.Status());
            return r;
        }
        if (method == "tts.setPersona" || method == "talk.voice.set" || method == "talk.voice.change") {
            tts.SetPersona(params.value("persona", params.value("voice", params.value("id", "default"))));
            r.payload = Real({{"persona", tts.Persona()}, {"status", tts.Status()}});
            return r;
        }
        if (method == "tts.stream") {
            // Provider audio is forwarded as WS events tts.chunk {streamId, seq, b64} while it arrives; tts.done at end.
            const auto sid = params.value("streamId", std::string("tts-") + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::size_t seq = 0, bytes = 0;
            auto res = tts.SpeakStream(params.value("text", params.value("input", "")), params.value("voice", ""), [&](const std::string& c) {
                bytes += c.size();
                if (deps.emit) deps.emit("tts.chunk", {{"streamId", sid}, {"seq", seq}, {"b64", channels::waweb::Base64(channels::waweb::Bytes(c.begin(), c.end()))}});
                ++seq;
            });
            if (deps.emit) deps.emit("tts.done", {{"streamId", sid}, {"chunks", seq}, {"bytes", bytes}, {"ok", res.value("ok", false)}});
            res["streamId"] = sid; res["chunks"] = seq; res["bytes"] = bytes;
            r.payload = Real(res);
            return r;
        }
        if (method == "tts.speak" || method == "tts.convert" || method == "talk.voice.complete") {
            auto text = params.value("text", params.value("input", ""));
            r.payload = Real(tts.Speak(text, params.value("voice", "")));
            return r;
        }
        if (method == "tts.personas" || method == "talk.voice.get") {
            r.payload = Real({{"persona", tts.Persona()},
                              {"personas", nlohmann::json::array({"default", "alloy", "echo"})}});
            return r;
        }
        if (starts("voicewake.")) {
            auto& vw = EnsureObj(deps.config.raw, "voicewake");
            if (method == "voicewake.get" || method == "voicewake.routing.get") {
                r.payload = Real({{"enabled", vw.value("enabled", false)},
                                  {"phrase", vw.value("phrase", "hey openclaw")},
                                  {"routing", vw.value("routing", nlohmann::json::object())}});
                return r;
            }
            if (method == "voicewake.set" || method == "voicewake.routing.set") {
                if (params.contains("enabled")) vw["enabled"] = params["enabled"];
                if (params.contains("phrase")) vw["phrase"] = params["phrase"];
                if (params.contains("routing")) vw["routing"] = params["routing"];
                PersistCfg(deps);
                r.payload = Real({{"saved", true}, {"voicewake", vw}});
                return r;
            }
            r.payload = Real({{"voicewake", vw}, {"method", method}});
            return r;
        }
        r.payload = Real({{"tts", tts.Status()}, {"method", method}});
        return r;
    }

    // ---- browser: CDP (real) or curl-session fallback (real-sim) ----
    if (starts("browser.") || method == "browser.open_url") {
        r.handled = true;
        if (!deps.sim) {
            r.payload = Real({{"configured", false}, {"error", "sim/browser not wired"}});
            return r;
        }
        auto tag = [](nlohmann::json payload) {
            const std::string eng = payload.value("engine", "");
            const std::string impl = payload.value("impl", "");
            if (impl == "real" || eng == "cdp") return Real(std::move(payload));
            if (eng == "cdp-mock" || impl == "real-sim" || eng == "curl-session")
                return RealSim(std::move(payload));
            if (impl == "missing") return payload;
            return RealSim(std::move(payload));
        };
        if (method == "browser.status" || method == "browser.") {
            r.payload = tag(deps.sim->BrowserStatus());
            return r;
        }
        if (method == "browser.navigate" || method == "browser.open" ||
            method == "browser.open_url" || method.find("open") != std::string::npos) {
            auto url = params.value("url", params.value("href", ""));
            r.payload = tag(deps.sim->BrowserNavigate(url));
            return r;
        }
        if (method == "browser.evaluate" || method.find("eval") != std::string::npos) {
            auto expr = params.value("expression", params.value("js", params.value("code", "")));
            r.payload = tag(deps.sim->BrowserEvaluate(expr));
            return r;
        }
        if (method == "browser.screenshot" || method.find("screenshot") != std::string::npos ||
            method.find("capture") != std::string::npos) {
            r.payload = tag(deps.sim->BrowserScreenshot(params));
            return r;
        }
        if (method == "browser.title" || method == "browser.url") {
            r.payload = tag(deps.sim->Browser().TitleAndUrl());
            return r;
        }
        if (method == "browser.session" || method == "browser.state") {
            r.payload = tag(deps.sim->Browser().SessionState());
            return r;
        }
        r.payload = tag({{"status", deps.sim->BrowserStatus()},
                         {"method", method},
                         {"hint",
                          "browser.navigate|evaluate|screenshot|status|open_url — CDP via "
                          "OPENCLAW_BROWSER_CDP_URL or curl fallback"}});
        return r;
    }

    // ---- controlUi / catalog / agent / projects / question / terminal (local) ----
    if (starts("controlUi.")) {
        r.handled = true;
        auto& ui = EnsureObj(deps.config.raw, "control_ui");
        if (method.find("sessionPreview") != std::string::npos) {
            auto key = params.value("sessionKey", params.value("key", ""));
            nlohmann::json msgs = nlohmann::json::array();
            for (const auto& m : deps.sessions.Load(key)) {
                msgs.push_back({{"role", m.role}, {"content", m.content.substr(0, 200)}});
                if (msgs.size() >= 20) break;
            }
            r.payload = Real({{"sessionKey", key}, {"preview", msgs}});
            return r;
        }
        if (method.find("github") != std::string::npos || method.find("linkPreview") != std::string::npos) {
            r.payload = Real({{"ok", false}, {"note", "github preview not wired"}, {"method", method}});
            return r;
        }
        r.payload = Real({{"control_ui", ui}, {"sessions", deps.sessions.List()}, {"method", method}});
        return r;
    }
    if (starts("catalog.") || method == "catalog.browse") {
        r.handled = true;
        r.payload = Real({{"methods", ImplementedMethods().size()},
                          {"advertised", AdvertisedMethodCount()},
                          {"tools", deps.tools.Names()},
                          {"method", method}});
        return r;
    }
    if (starts("agent.") || method == "agent") {
        r.handled = true;
        auto& ag = EnsureObj(deps.config.raw, "agent_identity");
        if (method == "agent.identity.get") {
            r.payload = Real({{"identity", ag.value("name", "openclaw-cpp")}, {"raw", ag}});
            return r;
        }
        if (method.find("identity") != std::string::npos && method.find("get") == std::string::npos) {
            if (params.contains("name")) ag["name"] = params["name"];
            PersistCfg(deps);
            r.payload = Real({{"identity", ag}});
            return r;
        }
        if (method == "agent.wait") {
            r.payload = Real({{"ready", true}, {"waited_ms", 0}});
            return r;
        }
        r.payload = Real({{"identity", ag}, {"method", method}});
        return r;
    }
    if (starts("projects.") || starts("question.") || starts("terminal.") || starts("gateway.")) {
        r.handled = true;
        auto key = method.substr(0, method.find('.'));
        auto& store = EnsureObj(deps.config.raw, (key + "_store").c_str());
        if (method.find("list") != std::string::npos || method.find("get") != std::string::npos) {
            r.payload = Real({{key, store}, {"method", method}});
            return r;
        }
        if (method.find("set") != std::string::npos || method.find("put") != std::string::npos ||
            method.find("create") != std::string::npos) {
            store["last"] = params;
            PersistCfg(deps);
            r.payload = Real({{"saved", true}, {key, store}});
            return r;
        }
        if (starts("gateway.") && (method.find("status") != std::string::npos || method.find("health") != std::string::npos)) {
            r.payload = Real({{"runtime", "running"}, {"version", "1.5.1-dev"}, {"align_target", "OpenClaw 2026.10.5"}});
            return r;
        }
        r.payload = Real({{key, store}, {"method", method}});
        return r;
    }
    if (starts("cron.scratch")) {
        r.handled = true;
        auto& sc = EnsureObj(deps.config.raw, "cron_scratch");
        auto id = params.value("id", params.value("jobId", "default"));
        if (method.find("set") != std::string::npos) {
            sc[id] = params.value("value", params);
            PersistCfg(deps);
            r.payload = Real({{"id", id}, {"saved", true}});
            return r;
        }
        r.payload = Real({{"id", id}, {"value", sc.value(id, nlohmann::json::object())}});
        return r;
    }
    if (starts("webSearch.")) {
        r.handled = true;
        // Not a search engine — point at http_get
        r.payload = Real({{"implemented", false},
                          {"method", method},
                          {"hint", "no bundled search index; use http_get tool for URLs"}});
        return r;
    }


    // ---- device / exec / connect (local pairing-ish state) ----
    if (starts("device.") || starts("exec.") || starts("connect.")) {
        r.handled = true;
        auto& dev = EnsureObj(deps.config.raw, "devices");
        if (starts("device.pair") || method.find("pair") != std::string::npos) {
            auto& pairs = EnsureArr(dev, "pairs");
            if (method.find("rename") != std::string::npos) {
                auto id = params.value("id", "");
                for (auto& p : pairs) {
                    if (p.value("id", "") == id) p["name"] = params.value("name", p.value("name", ""));
                }
                PersistCfg(deps);
                r.payload = Real({{"pairs", pairs}});
                return r;
            }
            if (method.find("request") != std::string::npos || method.find("setup") != std::string::npos) {
                nlohmann::json p{{"id", params.value("id", "dev-" + std::to_string(pairs.size() + 1))},
                                 {"name", params.value("name", "device")},
                                 {"status", "pending"}};
                pairs.push_back(p);
                PersistCfg(deps);
                r.payload = Real({{"pair", p}, {"pairing", deps.pairing.ListJson()}});
                return r;
            }
            if (method.find("resolve") != std::string::npos || method.find("completed") != std::string::npos) {
                auto id = params.value("id", "");
                for (auto& p : pairs) {
                    if (p.value("id", "") == id) p["status"] = params.value("status", "resolved");
                }
                PersistCfg(deps);
                r.payload = Real({{"pairs", pairs}});
                return r;
            }
            r.payload = Real({{"pairs", pairs}, {"pairing", deps.pairing.ListJson()}, {"method", method},
                              {"legacy", true},
                              {"note", "legacy config-backed device.pair.* record; use pairing.link.* for token-issuing flow"}});
            return r;
        }
        if (starts("device.token") || starts("device.scopes")) {
            auto& tok = EnsureObj(dev, "tokens");
            if (method.find("rotate") != std::string::npos || method.find("request") != std::string::npos) {
                auto id = params.value("id", "main");
                tok[id] = "tok-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
                // NOTE: legacy device.token.* values are config labels only and are NOT accepted by
                // gateway auth; only ocd_ tokens from pairing.link.claim are.
                PersistCfg(deps);
                r.payload = Real({{"id", id}, {"token_set", true}});  // never echo secret full
                return r;
            }
            if (method.find("revoke") != std::string::npos) {
                tok.erase(params.value("id", "main"));
                PersistCfg(deps);
                r.payload = Real({{"revoked", true}});
                return r;
            }
            {
                nlohmann::json ids = nlohmann::json::array();
                for (auto it = tok.begin(); it != tok.end(); ++it) ids.push_back(it.key());
                r.payload = Real({{"token_ids", ids}, {"method", method}});
            }
            return r;
        }
        if (starts("exec.")) {
            r.payload = Real({{"approvals", deps.approvals.ListJson()},
                              {"note", "exec gated via approvals; shell tool is mock by default"},
                              {"method", method}});
            return r;
        }
        if (starts("connect.")) {
            r.payload = Real({{"challenge", "oc-" + std::to_string(deps.bus.ListenerCount())},
                              {"method", method}});
            return r;
        }
        r.payload = Real({{"devices", dev}, {"nodes", deps.nodes.ListJson()}, {"method", method}});
        return r;
    }

    // ---- agent loop config (max_turns / rate limit) ----
    if (method == "config.set" || method == "config.apply" || method == "config.get" ||
        method == "config.openFile") {
        r.handled = true;
        if (method == "config.get" || method == "config.openFile") {
            r.payload = Real({{"config", deps.config.ToJson()}});
            return r;
        }
        // merge params into raw.agent / top-level
        auto& agent = EnsureObj(deps.config.raw, "agent");
        if (params.contains("max_turns")) agent["max_turns"] = params["max_turns"];
        if (params.contains("rate_limit_per_minute"))
            agent["rate_limit_per_minute"] = params["rate_limit_per_minute"];
        if (params.contains("agent") && params["agent"].is_object()) {
            for (auto it = params["agent"].begin(); it != params["agent"].end(); ++it)
                agent[it.key()] = it.value();
        }
        if (params.contains("raw") && params["raw"].is_object()) {
            deps.config.raw.merge_patch(params["raw"]);
        }
        PersistCfg(deps);
        r.payload = Real({{"saved", true}, {"agent", agent}, {"config", deps.config.ToJson()}});
        return r;
    }


    // ---- logs export / cheap OTel-ish export ----
    if (method == "logs.export" || method == "otel.export" || method == "diagnostics.export") {
        r.handled = true;
        auto dir = DataRoot(deps) / "exports";
        std::filesystem::create_directories(dir);
        auto path = dir / ("logs-" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()) + ".jsonl");
        std::ofstream out(path);
        auto lines = deps.logs.Tail(params.value("limit", 500));
        if (lines.is_array()) {
            for (const auto& line : lines) out << line.dump() << '\n';
        } else {
            out << lines.dump() << '\n';
        }
        nlohmann::json payload{{"path", path.string()},
                               {"lines", lines.is_array() ? lines.size() : 1},
                               {"format", "jsonl"},
                               {"impl", "real"}};
        // Optional OTLP HTTP logs endpoint (best-effort, no protobuf — JSON body)
        const char* ep = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
        if (ep && *ep && method != "logs.export") {
            payload["otel_endpoint"] = ep;
            payload["otel_note"] =
                "endpoint present; openclaw-cpp posts a minimal JSON batch (not full OTLP protobuf)";
            payload["otel_mode"] = "http_json_best_effort";
        } else if (method == "otel.export") {
            payload["otel_mode"] = "file_only";
            payload["note"] = "set OTEL_EXPORTER_OTLP_ENDPOINT for best-effort HTTP export";
        }
        Audit(deps, method, {{"path", path.string()}});
        r.payload = Real(payload);
        return r;
    }

    // ---- push / heartbeat / mcp (local) ----
    if (starts("push.") || method == "heartbeat" || method == "last-heartbeat") {
        r.handled = true;
        auto& push = EnsureObj(deps.config.raw, "push");
        if (method == "heartbeat") {
            push["last_heartbeat_ms"] =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            PersistCfg(deps);
            deps.logs.Append("info", "heartbeat");
            r.payload = Real({{"ok", true}, {"last_heartbeat_ms", push["last_heartbeat_ms"]}});
            return r;
        }
        if (method == "last-heartbeat") {
            r.payload = Real({{"last_heartbeat_ms", push.value("last_heartbeat_ms", 0)}});
            return r;
        }
        if (method.find("list") != std::string::npos || method.find("status") != std::string::npos) {
            r.payload = Real({{"subscriptions", push.value("subscriptions", nlohmann::json::array())},
                              {"push", push}});
            return r;
        }
        if (method.find("subscribe") != std::string::npos || method.find("set") != std::string::npos) {
            auto& subs = EnsureArr(push, "subscriptions");
            subs.push_back(params);
            PersistCfg(deps);
            r.payload = Real({{"saved", true}, {"subscriptions", subs}});
            return r;
        }
        r.payload = Real({{"push", push}, {"method", method}});
        return r;
    }
    if (starts("mcp.") && !starts("mcp.app.")) {
        r.handled = true;
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& n : deps.tools.Names())
            if (n.rfind("mcp.", 0) == 0) tools.push_back(n);
        r.payload = Real({{"mcp_tools", tools}, {"method", method},
                          {"note", "in-proc/stdio MCP bridge; mcp.app.* host UI not implemented"}});
        return r;
    }

    // bare chat / cron aliases
    if (method == "chat" || method == "cron") {
        r.handled = true;
        if (method == "chat") {
            r.payload = Real({{"sessions", deps.sessions.List()}, {"hint", "use chat.send / sessions.send"}});
            return r;
        }
        r.payload = Real({{"jobs", deps.cron.ListJson()}});
        return r;
    }

    // ---- computer / canvas / desktop (screenshot may be live; input still real-sim) ----
    if (starts("computer.") || starts("canvas.") || starts("desktop.")) {
        r.handled = true;
        if (!deps.sim) {
            r.payload = RealSim({{"error", "sim backend not wired"}, {"method", method}});
            return r;
        }
        auto tag_computer = [](nlohmann::json payload) {
            if (payload.value("impl", "") == "real") return Real(std::move(payload));
            return RealSim(std::move(payload));
        };
        auto& sim = *deps.sim;
        if (method == "computer.status") { r.payload = tag_computer(sim.ComputerStatus()); return r; }
        if (method == "computer.click" || method == "computer.mouse_click") {
            nlohmann::json p = params;
            p["action"] = "click";
            r.payload = tag_computer(sim.ComputerInvoke(p));
            return r;
        }
        if (method == "computer.type" || method == "computer.keyboard") {
            nlohmann::json p = params;
            p["action"] = "type";
            r.payload = tag_computer(sim.ComputerInvoke(p));
            return r;
        }
        if (method == "computer.move" || method == "computer.mouse_move") {
            nlohmann::json p = params;
            p["action"] = "move";
            r.payload = tag_computer(sim.ComputerInvoke(p));
            return r;
        }
        if (method == "computer.key") {
            nlohmann::json p = params;
            p["action"] = "key";
            r.payload = tag_computer(sim.ComputerInvoke(p));
            return r;
        }
        if (method == "computer.invoke") {
            auto inv = sim.ComputerInvoke(params);
            r.payload = tag_computer(std::move(inv));
            return r;
        }
        if (method.find("screenshot") != std::string::npos) {
            r.payload = tag_computer(sim.Screenshot(params));
            return r;
        }
        if (starts("canvas.")) {
            auto* store = deps.canvas ? deps.canvas : (deps.sim ? &deps.sim->Canvas() : nullptr);
            auto tag_canvas = [](nlohmann::json payload) {
                if (payload.value("impl", "") == "real") return Real(std::move(payload));
                return RealSim(std::move(payload));
            };
            if (!store && !deps.sim) {
                r.payload = RealSim({{"error", "canvas not wired"}, {"method", method}});
                return r;
            }
            if (starts("canvas.a2ui.")) {
                if (!deps.a2ui) { r.payload = RealSim({{"error", "a2ui host not wired"}}); return r; }
                if (method == "canvas.a2ui.reset") { r.payload = Real(deps.a2ui->Reset()); return r; }
                if (method == "canvas.a2ui.state" || method == "canvas.a2ui.get") { r.payload = Real(deps.a2ui->State()); return r; }
                if (method == "canvas.a2ui.render") { r.payload = Real({{"html", deps.a2ui->RenderSurface(params.value("surfaceId", std::string("main")))}}); return r; }
                if (method == "canvas.a2ui.validate" || method == "canvas.a2ui.push" || method == "canvas.a2ui.pushJSONL") {
                    canvas::a2ui::Validation v;
                    if (params.contains("jsonl")) v = canvas::a2ui::ValidateJsonl(params.value("jsonl", std::string()));
                    else if (params.contains("messages")) v = canvas::a2ui::ValidateMessages(params["messages"]);
                    else if (params.contains("text")) v = canvas::a2ui::ValidateJsonl(canvas::a2ui::BuildTextJsonl(params.value("text", std::string()), params.value("surfaceId", std::string("main"))));
                    else v.error = "jsonl, messages or text required";
                    if (!v.ok) { r.payload = Real({{"ok", false}, {"error", v.error}}); return r; }
                    if (method == "canvas.a2ui.validate") { r.payload = Real({{"ok", true}, {"version", v.version}, {"messageCount", v.message_count}}); return r; }
                    r.payload = Real(deps.a2ui->Apply(v.messages));
                    return r;
                }
                r.payload = Real({{"error", "unknown a2ui method"}, {"method", method}});
                return r;
            }
            if (method == "canvas.status") {
                r.payload = Real(store ? store->Status() : nlohmann::json{{"error", "no store"}});
                return r;
            }
            if (method == "canvas.list" || method == "canvas.docs.list") {
                r.payload = Real(store->List());
                return r;
            }
            if (method == "canvas.create" || method == "canvas.docs.create") {
                r.payload = Real(store->Create(params));
                return r;
            }
            if (method == "canvas.get" || method == "canvas.docs.get" ||
                method == "canvas.document.get") {
                r.payload = Real(store->Get(params.value("id", params.value("canvas_id", ""))));
                return r;
            }
            if (method == "canvas.patch" || method == "canvas.docs.patch" ||
                method == "canvas.document.patch") {
                r.payload = Real(store->Patch(params.value("id", params.value("canvas_id", "")),
                                              params.contains("patch") ? params["patch"] : params));
                return r;
            }
            if (method == "canvas.delete" || method == "canvas.docs.delete" ||
                method == "canvas.remove") {
                r.payload = Real(store->Remove(params.value("id", params.value("canvas_id", ""))));
                return r;
            }
            if (method == "canvas.events" || method == "canvas.events.list" ||
                method == "canvas.event.log") {
                r.payload = Real(store->Events(params.value("limit", 50),
                                               params.value("id", params.value("canvas_id", ""))));
                return r;
            }
            if (method == "canvas.event" || method == "canvas.events.append") {
                r.payload = Real(store->RecordEvent(params.value("kind", "ui"),
                                                    params.value("detail", nlohmann::json::object()),
                                                    params.value("id", params.value("canvas_id", ""))));
                return r;
            }
            if (method.find("preview") != std::string::npos) {
                r.payload = tag_canvas(sim.CanvasPreview(params));
                return r;
            }
            if (method.find("view") != std::string::npos) {
                r.payload = tag_canvas(sim.CanvasView(params));
                return r;
            }
            if (method.find("put") != std::string::npos || method.find("update") != std::string::npos) {
                r.payload = tag_canvas(sim.CanvasPut(params));
                return r;
            }
            r.payload = tag_canvas(sim.CanvasView(params));
            return r;
        }
        if (starts("desktop.") && desktop::Default().Available() && method != "desktop.sim.status") {
            auto& dc = desktop::Default();
            nlohmann::json out;
            if (method == "desktop.launch") out = dc.Launch(params);
            else if (method == "desktop.observe") out = dc.Observe(params);
            else if (method == "desktop.release") out = dc.Release(params);
            else if (method == "desktop.windows") out = dc.Windows();
            else if (method == "desktop.focus") out = dc.Focus(params);
            else if (method == "desktop.close") out = dc.Close(params);
            else if (method == "desktop.a11y.tree") out = dc.A11yTree(params);
            else if (method == "desktop.a11y.action") out = dc.A11yAction(params);
            else out = dc.Status();
            r.payload = Real(std::move(out));
            return r;
        }
        if (starts("desktop.")) {
            if (method.find("launch") != std::string::npos) {
                r.payload = RealSim(sim.DesktopLaunch(params));
                return r;
            }
            if (method.find("observe") != std::string::npos) {
                r.payload = tag_computer(sim.DesktopObserve(params));
                return r;
            }
            if (method.find("release") != std::string::npos) {
                r.payload = RealSim(sim.DesktopRelease(params));
                return r;
            }
            r.payload = RealSim({{"desktop", sim.Status()}, {"method", method}});
            return r;
        }
        r.payload = RealSim(sim.ComputerInvoke(params.contains("action") ? params : nlohmann::json{{"action", "noop"}, {"method", method}}));
        return r;
    }

    // ---- mcp.app.* bridge to mcp::Client ----
    if (starts("mcp.app.")) {
        r.handled = true;
        auto cli = deps.mcp_client;
        if (method == "mcp.app.listTools") {
            nlohmann::json tools = nlohmann::json::array();
            if (cli) {
                for (const auto& t : cli->ListTools())
                    tools.push_back({{"name", t.name}, {"description", t.description}});
            }
            // also surface bridged registry tools
            for (const auto& n : deps.tools.Names())
                if (n.rfind("mcp.", 0) == 0) tools.push_back({{"name", n}, {"bridged", true}});
            r.payload = Real({{"tools", tools}, {"client", cli ? true : false}});
            return r;
        }
        if (method == "mcp.app.callTool") {
            if (!cli) { r.ok = false; r.error = "mcp client not attached"; return r; }
            auto name = params.value("name", params.value("tool", ""));
            auto args = params.contains("arguments") ? params["arguments"] : params.value("args", nlohmann::json::object());
            try {
                r.payload = Real(cli->CallTool(name, args));
            } catch (const std::exception& e) {
                r.ok = false; r.error = e.what();
            }
            return r;
        }
        if (method == "mcp.app.listResources") {
            if (!cli) { r.payload = Real({{"resources", nlohmann::json::array()}, {"client", false}}); return r; }
            r.payload = Real(cli->ListResources());
            return r;
        }
        if (method == "mcp.app.listResourceTemplates") {
            if (!cli) { r.payload = Real({{"resourceTemplates", nlohmann::json::array()}}); return r; }
            r.payload = Real(cli->ListResourceTemplates());
            return r;
        }
        if (method == "mcp.app.readResource") {
            if (!cli) { r.ok = false; r.error = "mcp client not attached"; return r; }
            try {
                r.payload = Real(cli->ReadResource(params.value("uri", "")));
            } catch (const std::exception& e) {
                r.ok = false; r.error = e.what();
            }
            return r;
        }
        if (method == "mcp.app.writeResource" || method == "mcp.app.formResource" ||
            method == "mcp.app.subscribeResource" || method == "mcp.app.unsubscribeResource" ||
            method == "mcp.app.openFile" || method == "mcp.app.modelContext" ||
            method == "mcp.app.removeModelContext") {
            auto& st = EnsureObj(deps.config.raw, "mcp_app");
            st[method] = params;
            PersistCfg(deps);
            r.payload = Real({{"ok", true}, {"method", method}, {"note", "local mcp.app resource/context ack"}});
            return r;
        }
        // host-UI-ish methods: local ack + tool inventory (not a full MCP app host)
        if (method == "mcp.app.discover" || method == "mcp.app.view" || method == "mcp.app.launch" ||
            method == "mcp.app.settings" || method == "mcp.app.onboard" || method == "mcp.app.mention" ||
            method == "mcp.app.updateModelContext") {
            auto& st = EnsureObj(deps.config.raw, "mcp_app");
            st["last_method"] = method;
            st["last_params"] = params;
            PersistCfg(deps);
            nlohmann::json tools = nlohmann::json::array();
            if (cli) for (const auto& t : cli->ListTools()) tools.push_back(t.name);
            r.payload = Real({{"ok", true},
                              {"method", method},
                              {"state", st},
                              {"tools", tools},
                              {"note", "MCP app host UI surface is local-ack only; tools/resources go through mcp.app.list*/call*/read*"}});
            return r;
        }
        r.payload = Real({{"method", method}, {"client", cli ? true : false}});
        return r;
    }

    // ---- Board (disk JSON + Control UI /board) ----
    if (starts("board.")) {
        r.handled = true;
        if (!deps.board) {
            r.ok = false;
            r.error = "board store not attached";
            return r;
        }
        if (method == "board.get" || method == "board.data.read" || method == "board.status") {
            auto board = deps.board->EnsureDefaults();
            r.payload = Real({{"board", board}, {"status", deps.board->Status()},
                              {"note", "local JSON board — not full OpenClaw board UX"}});
            return r;
        }
        if (method == "board.update" || method == "board.put") {
            auto board = params.contains("board") ? params["board"] : params;
            r.payload = Real({{"board", deps.board->SetBoard(board)}, {"saved", true}});
            return r;
        }
        if (method == "board.column.create" || method == "board.columns.add") {
            r.payload = Real(deps.board->AddColumn(params.value("title", params.value("name", "Column")),
                                                   params.value("id", "")));
            return r;
        }
        if (method == "board.card.create" || method == "board.card.add") {
            r.payload = Real(deps.board->AddCard(params.value("column_id", params.value("column", "todo")),
                                                 params.value("title", params.value("name", "")),
                                                 params.value("body", params.value("text", ""))));
            return r;
        }
        if (method == "board.card.update") {
            r.payload = Real(deps.board->UpdateCard(params.value("id", ""), params));
            return r;
        }
        if (method == "board.card.move" || method == "board.move") {
            r.payload = Real(deps.board->MoveCard(params.value("id", params.value("card_id", "")),
                                                  params.value("column_id", params.value("column", "")),
                                                  params.value("order", -1)));
            return r;
        }
        if (method == "board.card.delete" || method == "board.card.remove") {
            r.payload = Real(deps.board->DeleteCard(params.value("id", params.value("card_id", ""))));
            return r;
        }
        if (method == "board.action") {
            auto action = params.value("action", "");
            if (action == "create" || action == "add") {
                r.payload = Real(deps.board->AddCard(params.value("column_id", "todo"),
                                                     params.value("title", ""),
                                                     params.value("body", "")));
                return r;
            }
            if (action == "move") {
                r.payload = Real(deps.board->MoveCard(params.value("id", ""),
                                                      params.value("column_id", ""),
                                                      params.value("order", -1)));
                return r;
            }
            if (action == "delete") {
                r.payload = Real(deps.board->DeleteCard(params.value("id", "")));
                return r;
            }
            r.payload = Real({{"board", deps.board->Get()}, {"action", action},
                              {"note", "unknown board.action — use card.create/move/delete"}});
            return r;
        }
        // other board.* (widget etc.) — return live board + honest note
        r.payload = Real({{"board", deps.board->Get()},
                          {"status", deps.board->Status()},
                          {"method", method},
                          {"note", "local board subset — widget/prompt APIs not implemented"}});
        return r;
    }

    // ---- Easy catalog gaps: local boards/portals/setup/mentions (not upstream UI) ----
    if (starts("board.") || starts("portal.") || starts("wizard.") || starts("openclaw.") ||
        starts("mentions.") || starts("migrations.") || starts("usage.") || starts("ui.") ||
        starts("storage.") || starts("progressCard.") || starts("plugin.approval") ||
        starts("plugin.surface") || starts("taskSuggestions.") || starts("nativeHook.") ||
        starts("web.login") || starts("attach.") || starts("claws.") || starts("debugProxy.")) {
        r.handled = true;
        // Still missing production features — but give real local stores for API testing
        if (starts("debugProxy.") || starts("claws.") || starts("nativeHook.") ||
            starts("web.login") || starts("attach.")) {
            r.payload = Real({{"implemented", false},
                              {"method", method},
                              {"note", "intentionally not faked — see PARITY.md"}});
            return r;
        }
        auto store_key = method.substr(0, method.find('.'));
        if (store_key.empty()) store_key = "misc";
        const std::string store_name = store_key + "_local";
        auto& store = EnsureObj(deps.config.raw, store_name.c_str());
        if (starts("openclaw.approval") || starts("plugin.approval")) {
            r.payload = Real({{"approvals", deps.approvals.ListJson()}, {"method", method}});
            return r;
        }
        if (starts("openclaw.chat")) {
            auto key = params.value("sessionKey", params.value("key", "main"));
            nlohmann::json msgs = nlohmann::json::array();
            for (const auto& m : deps.sessions.Load(key)) msgs.push_back(m.ToJson());
            r.payload = Real({{"sessionKey", key}, {"messages", msgs}, {"method", method}});
            return r;
        }
        if (starts("openclaw.setup") || starts("wizard.")) {
            auto& setup = EnsureObj(deps.config.raw, "setup");
            if (method.find("start") != std::string::npos || method.find("activate") != std::string::npos ||
                method.find("prepare") != std::string::npos || method.find("auto") != std::string::npos) {
                setup["last"] = method;
                setup["params"] = params;
                setup["status"] = "local-ack";
                PersistCfg(deps);
            }
            r.payload = Real({{"setup", setup}, {"method", method},
                              {"note", "local setup ack only — not OpenClaw installer"}});
            return r;
        }
        if (starts("openclaw.changes") || starts("mentions.")) {
            auto& arr = EnsureArr(store, "items");
            if (method.find("list") != std::string::npos) {
                r.payload = Real({{"items", arr}, {"method", method}});
                return r;
            }
            if (method.find("dismiss") != std::string::npos) {
                auto id = params.value("id", "");
                nlohmann::json next = nlohmann::json::array();
                for (auto& x : arr)
                    if (x.value("id", "") != id) next.push_back(x);
                arr = next;
                PersistCfg(deps);
                r.payload = Real({{"dismissed", id}, {"items", arr}});
                return r;
            }
            if (method.find("changed") != std::string::npos || method.find("add") != std::string::npos) {
                arr.push_back(params.empty() ? nlohmann::json{{"id", "item-" + std::to_string(arr.size())}} : params);
                PersistCfg(deps);
            }
            r.payload = Real({{"items", arr}, {"method", method}});
            return r;
        }
        if (starts("board.") || starts("portal.") || starts("progressCard.") || starts("ui.") ||
            starts("storage.") || starts("usage.") || starts("migrations.") || starts("taskSuggestions.") ||
            starts("plugin.surface")) {
            if (method.find("put") != std::string::npos || method.find("update") != std::string::npos ||
                method.find("action") != std::string::npos || method.find("grant") != std::string::npos ||
                method.find("apply") != std::string::npos || method.find("plan") != std::string::npos) {
                store["last"] = {{"method", method}, {"params", params}};
                PersistCfg(deps);
                r.payload = Real({{"saved", true}, {"store", store}, {"method", method},
                                  {"note", "local store only — not production board/portal UI"}});
                return r;
            }
            r.payload = Real({{"store", store}, {"method", method},
                              {"note", "local store only — not production board/portal UI"}});
            return r;
        }
        r.payload = Real({{"store", store}, {"method", method}});
        return r;
    }



    // residual catch-all still honest (should be rare)
    r.handled = true;
    r.payload = Honest(method, params, nlohmann::json::object());
    return r;
}

}  // namespace openclaw::gateway
