#include "openclaw/gateway/core_tools.hpp"

#include "openclaw/agent/pool.hpp"
#include "openclaw/config/config.hpp"
#include "openclaw/gateway/node_hub.hpp"
#include "openclaw/gateway/subsystems.hpp"
#include "openclaw/channels/misc_channels.hpp"
#include "openclaw/provider/router.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>

namespace openclaw::gateway {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
std::atomic<provider::Router*> g_router{nullptr};
std::mutex g_mu;

std::string Err(const std::string& msg, const std::string& code = "") {
    json j{{"ok", false}, {"error", msg}};
    if (!code.empty()) j["code"] = code;
    return j.dump();
}
std::string S(const json& a, const char* k) { return a.contains(k) && a[k].is_string() ? a[k].get<std::string>() : std::string(); }
// In-process gateway RPC: payload on success, {ok:false,error} otherwise.
json Rpc(const std::string& m, const json& p) {
    auto r = GatewayRpc(m, p);
    if (r.value("ok", false)) return r.contains("payload") ? r["payload"] : json::object();
    json e = r.contains("error") ? r["error"] : json("rpc failed");
    return {{"ok", false}, {"error", e.is_object() ? e.value("message", e.dump()) : e.is_string() ? e.get<std::string>() : e.dump()}, {"method", m}};
}
bool Failed(const json& j) { return j.is_object() && j.contains("ok") && j["ok"] == false; }
long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string Hex(const std::string& s) {
    unsigned char h[SHA256_DIGEST_LENGTH]; SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), h);
    static const char* x = "0123456789abcdef"; std::string o; for (auto b : h) { o.push_back(x[b >> 4]); o.push_back(x[b & 15]); } return o;
}
std::string ReadFile(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::ostringstream ss; ss << in.rdbuf(); return ss.str(); }
void WriteFile(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }

// Upstream senderIsOwner: an authenticated owner chat principal. Here: sessions that did not arrive from an
// external channel (RPC / CLI / Control UI / cron) count as operator-owned; channel sessions need
// commands.ownerAllowFrom to name the channel.
bool IsOwnerSession(const std::string& key, const json& cfg) {
    static const std::set<std::string> kChannels = {"telegram", "slack", "discord", "whatsapp", "signal", "matrix", "msteams", "googlechat",
                                                    "mattermost", "feishu", "line", "imessage", "irc", "webchat", "email", "sms", "voice"};
    std::vector<std::string> parts; std::stringstream ss(key); std::string t; while (std::getline(ss, t, ':')) parts.push_back(t);
    if (parts.size() < 3 || !kChannels.count(parts[2])) return true;
    auto allow = config::GetPath(cfg, "commands.ownerAllowFrom");
    if (allow && allow->is_array()) for (auto& x : *allow) if (x.is_string() && (x == parts[2] || x == "*")) return true;
    return false;
}

// ---------------------------------------------------------------- gateway
std::string GatewayTool(const json& a, const std::string& key, const json& cfg) {
    const auto action = S(a, "action");
    if (action == "update.run") {
        if (!IsOwnerSession(key, cfg))
            return json{{"ok", false}, {"code", "owner_required"}, {"reason", "owner_required"},
                        {"message", "No authenticated owner chat principal or operator-scheduled admission authorizes this update."}}.dump();
        auto r = Rpc("update.run", {{"sessionKey", key}, {"note", S(a, "note")}});
        return (Failed(r) ? r : json{{"ok", true}, {"result", r}}).dump();
    }
    if (action == "config.get") {
        auto snap = Rpc("config.get", json::object());
        if (Failed(snap)) return snap.dump();
        json result = snap;
        if (!S(a, "path").empty()) {
            const json conf = snap.contains("config") ? snap["config"] : snap.contains("raw") ? snap["raw"] : snap;
            auto v = config::GetPath(conf, S(a, "path"));
            if (!v) return Err("config path not found: " + S(a, "path"));
            result = {{"path", S(a, "path")}, {"config", *v}};
            if (snap.contains("hash")) result["hash"] = snap["hash"];
        }
        json payload{{"ok", true}, {"result", result}};
        auto text = payload.dump(2);
        if (text.size() > 60000) return Err("config.get response is too large; use path to request a narrower config subtree");
        return text;
    }
    if (action == "config.schema.lookup") {
        if (S(a, "path").empty()) return Err("path required");
        auto r = Rpc("config.schema.lookup", {{"path", S(a, "path")}});
        if (Failed(r)) return json{{"ok", false}, {"code", "schema_path_not_found"}, {"path", S(a, "path")}, {"message", r["error"]}}.dump();
        return json{{"ok", true}, {"result", r}}.dump();
    }
    return Err("Unknown action: " + action + " (config.get | config.schema.lookup | update.run)");
}

// ---------------------------------------------------------------- plugins
std::string PluginsTool(const json& a) {
    const auto action = S(a, "action");
    auto need = [&](const char* k) { return S(a, k); };
    json consent = json::object();
    if (!S(a, "reviewToken").empty()) consent["acknowledgeCapabilities"] = {{"reviewToken", S(a, "reviewToken")}};
    std::string m; json p = consent;
    if (action == "list") {
        auto inv = Rpc("plugins.list", json::object());
        if (Failed(inv)) return inv.dump();
        std::string q = S(a, "query"); std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        json match = json::array();
        for (auto& pl : inv.value("plugins", json::array())) {
            std::string hay = pl.value("id", "") + " " + pl.value("name", "") + " " + pl.value("description", "");
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (q.empty() || hay.find(q) != std::string::npos) match.push_back(pl);
        }
        json rows = json::array();
        for (size_t i = 0; i < match.size() && i < 20; ++i)
            rows.push_back({{"id", match[i].value("id", "")}, {"state", match[i].value("enabled", false) ? "enabled" : "disabled"}, {"version", match[i].value("version", "")}});
        return json{{"plugins", rows}, {"matching", match.size()}, {"omitted", match.size() > 20 ? match.size() - 20 : 0}, {"mutationAllowed", true},
                    {"next", "Use query to narrow this inventory or inspect a pluginId for details."}}.dump();
    }
    if (action == "inspect" || action == "uninstall") { if (need("pluginId").empty()) return Err("pluginId required"); m = "plugins." + action; p = {{"pluginId", need("pluginId")}}; }
    else if (action == "reload" || action == "reloadAndResume") { if (need("pluginId").empty()) return Err("pluginId required"); m = "plugins.reload"; p["plugins"] = json::array({{{"pluginId", need("pluginId")}}}); }
    else if (action == "search") { if (need("query").empty()) return Err("query required"); m = "plugins.search"; p = {{"query", need("query")}, {"limit", 10}}; }
    else if (action == "enable" || action == "disable") { if (need("pluginId").empty()) return Err("pluginId required"); m = "plugins.setEnabled"; p["pluginId"] = need("pluginId"); p["enabled"] = action == "enable"; }
    else if (action == "install") {
        const auto src = need("source");
        if (src != "official" && src != "clawhub") return Err("Unknown plugin installation source: " + src);
        if (src == "official" && a.contains("version")) return Err("Official catalog installs do not accept a version. Use the CLI for a version-constrained install, or omit version to use the catalog selection.");
        m = "plugins.install"; p["source"] = src;
        if (src == "official") { if (need("pluginId").empty()) return Err("pluginId required"); p["pluginId"] = need("pluginId"); }
        else { if (need("packageName").empty()) return Err("packageName required"); p["packageName"] = need("packageName"); if (!S(a, "version").empty()) p["version"] = S(a, "version"); }
        if (a.value("acknowledgeInstallPolicyWarning", false)) p["acknowledgeInstallPolicyWarning"] = true;
    } else return Err("Unknown plugin action: " + action);
    auto r = Rpc(m, p);
    if (action == "reloadAndResume" && !Failed(r)) r["resume"] = "Plugin reloaded; the next turn sees the refreshed tools.";
    return r.dump();
}

// ---------------------------------------------------------------- progress_card
std::string ProgressCardTool(const json& a, const std::string& agent, const std::string& key) {
    if (key.empty()) return Err("progress_card requires an agent session");
    json p{{"sessionKey", key}, {"agentId", agent}};
    if (a.contains("markdown")) { if (!a["markdown"].is_string() || a["markdown"].get<std::string>().size() > 4000) return Err("markdown must be a string of at most 4000 characters"); p["markdown"] = a["markdown"]; }
    if (a.contains("plan")) {
        if (!a["plan"].is_array() || a["plan"].size() > 20) return Err("plan must be an array of at most 20 steps");
        static const std::set<std::string> st = {"pending", "in_progress", "completed", "cancelled", "blocked"};
        json steps = json::array();
        for (auto& s : a["plan"]) {
            if (!s.is_object()) return Err("each plan step must be an object");
            const auto title = s.value("title", s.value("step", std::string()));
            if (title.empty()) return Err("each plan step needs a title");
            const auto status = s.value("status", std::string("pending"));
            if (!st.count(status)) return Err("invalid step status: " + status);
            steps.push_back({{"title", title}, {"status", status}});
        }
        p["plan"] = steps;
    }
    auto r = Rpc("progressCard.put", p);
    if (Failed(r)) return r.dump();
    const json card = r.value("card", json(nullptr));
    int done = 0, total = 0;
    if (card.is_object()) for (auto& s : card.value("steps", json::array())) { ++total; if (s.value("status", "") == "completed") ++done; }
    json out{{"revision", card.is_object() ? json(card.value("revision", 0)) : json(nullptr)}, {"steps", total ? json{{"completed", done}, {"total", total}} : json(nullptr)}};
    out["text"] = card.is_null() ? std::string("Progress card cleared")
                                 : total ? "Progress card updated (rev " + std::to_string(card.value("revision", 0)) + ", " + std::to_string(done) + "/" + std::to_string(total) + " done)"
                                         : "Progress card updated (rev " + std::to_string(card.value("revision", 0)) + ")";
    return out.dump();
}

// ---------------------------------------------------------------- screen
std::string ScreenTool(const json& a, const std::string& agent, const std::string& key) {
    const auto action = S(a, "action");
    auto sk = [&]() { auto s = S(a, "sessionKey"); return s.empty() || s == "current" ? key : s; };
    const auto dock = S(a, "dock");
    if (!dock.empty() && dock != "bottom" && dock != "right") return Err("dock must be bottom or right");
    json cmd;
    if (action == "split_right" || action == "split_down") cmd = {{"kind", "split"}, {"direction", action == "split_right" ? "right" : "down"}, {"sessionKey", sk()}};
    else if (action == "close_pane" || action == "focus" || action == "navigate") cmd = {{"kind", action == "close_pane" ? "close-pane" : action}, {"sessionKey", sk()}};
    else if (action == "sidebar_show" || action == "sidebar_hide") cmd = {{"kind", "sidebar"}, {"visible", action == "sidebar_show"}};
    else if (std::regex_match(action, std::regex("(terminal|browser|desktop|portal)_(show|hide)"))) {
        const bool open = action.size() > 5 && action.substr(action.size() - 5) == "_show";
        const auto panel = action.substr(0, action.find('_'));
        cmd = {{"kind", "panel"}, {"panel", panel}, {"open", open}};
        if (open) cmd["dock"] = dock.empty() ? (panel == "desktop" || panel == "portal" ? "right" : "bottom") : dock;
        if (panel == "desktop" && !S(a, "environmentId").empty()) cmd["environmentId"] = S(a, "environmentId");
        if (panel == "portal") {
            if (!S(a, "portalId").empty() && !S(a, "environmentId").empty()) return Err("Choose portalId or a pending environmentId, not both");
            if (!S(a, "portalId").empty()) cmd["portalId"] = S(a, "portalId"); else if (!S(a, "environmentId").empty()) cmd["environmentId"] = S(a, "environmentId");
        }
    } else return Err("Unknown action: " + action);
    json p{{"command", cmd}, {"agentId", agent}};
    if (!sk().empty()) p["sessionKey"] = sk();
    return Rpc("ui.command", p).dump();
}

// ---------------------------------------------------------------- terminal (operator-opened shared terminals)
std::string TerminalTool(const json& a, const std::string& agent, const std::string& key, const json& cfg) {
    const auto action = S(a, "action");
    static const std::set<std::string> acts = {"read", "list", "resize", "close", "input"};
    if (!acts.count(action)) return Err("terminal action unavailable; use list, read, resize, close, or input");
    if (key.empty()) return Err("agent session required");
    const std::string unavailable = "Terminal session unavailable. Use action=list to find a shared terminal or ask the operator to open one in this chat.";
    auto list = Rpc("terminal.list", json::object());
    if (Failed(list)) return list.dump();
    json mine = json::array();
    for (auto& s : list.value("sessions", json::array()))
        if (s.value("agentId", std::string("main")) == agent && (s.value("sessionKey", key) == key))
            mine.push_back({{"sessionId", s.value("sessionId", "")}, {"agentId", s.value("agentId", "main")}, {"shell", s.value("shell", "")}, {"cwd", s.value("cwd", "")},
                            {"attached", s.value("attached", false)}, {"owner", "agent:" + agent}, {"createdAtMs", s.value("createdAtMs", 0LL)}});
    if (action == "list") return json{{"sessions", mine}}.dump();
    const auto sid = S(a, "sessionId");
    if (sid.empty()) return Err("sessionId required");
    if (std::none_of(mine.begin(), mine.end(), [&](const json& s) { return s["sessionId"] == sid; })) return Err(unavailable);
    if (action == "read") {
        auto r = Rpc("terminal.attach", {{"sessionId", sid}});
        if (Failed(r)) return Err(unavailable);
        std::string text = r.value("buffer", std::string());
        text = std::regex_replace(text, std::regex("\x1b\\[[0-9;?]*[ -/]*[@-~]"), "");  // strip CSI escapes
        if (text.size() > 20000) text = text.substr(text.size() - 20000);
        return json{{"sessionId", sid}, {"text", text}}.dump();
    }
    if (action == "resize") {
        const int c = a.value("cols", 0), r = a.value("rows", 0);
        if (c < 1 || c > 2000) return Err("cols must be an integer from 1 to 2000");
        if (r < 1 || r > 2000) return Err("rows must be an integer from 1 to 2000");
        auto x = Rpc("terminal.resize", {{"sessionId", sid}, {"cols", c}, {"rows", r}});
        return Failed(x) ? Err("Terminal resize failed. Use action=list to find a shared terminal.") : json{{"ok", true}}.dump();
    }
    if (action == "close") { auto x = Rpc("terminal.close", {{"sessionId", sid}}); return Failed(x) ? Err(unavailable) : json{{"ok", true}}.dump(); }
    if (!a.contains("data") || !a["data"].is_string()) return Err("data required");
    // Upstream asks the operator for every input unless the exec policy is "full"; without an interactive approval
    // route for shared terminals here, only an explicit full policy admits input.
    auto sec = config::GetPath(cfg, "tools.exec.security"); auto mode = config::GetPath(cfg, "tools.exec.mode");
    const bool full = (sec && *sec == "full") || (mode && *mode == "full");
    if (mode && *mode == "deny") return Err("Terminal input denied by execution policy");
    if (!full) return Err("Terminal input denied: operator approval required (set tools.exec.security=\"full\" to allow agent terminal input)");
    auto x = Rpc("terminal.input", {{"sessionId", sid}, {"data", a["data"]}});
    return Failed(x) ? Err(unavailable) : json{{"ok", true}}.dump();
}

// ---------------------------------------------------------------- portal
std::string PortalTool(const json& a, const std::string& key, const json& cfg) {
    const auto action = S(a, "action");
    if (!IsOwnerSession(key, cfg)) return Err("portal requires an owner session");
    json env = json::object(); if (!S(a, "environmentId").empty()) env["environmentId"] = S(a, "environmentId");
    if (action == "list") {
        auto r = Rpc("portal.list", env); if (Failed(r)) return r.dump();
        const auto n = r.value("portals", json::array()).size();
        r["text"] = std::to_string(n) + " active portal" + (n == 1 ? "" : "s") + ". The operator can see them in the Control UI Portals page.";
        return r.dump();
    }
    if (action == "close") {
        if (S(a, "id").empty()) return Err("id required");
        json p = env; p["id"] = S(a, "id"); auto r = Rpc("portal.close", p); if (Failed(r)) return r.dump();
        r["text"] = "Portal " + S(a, "id") + " closed. The Control UI Portals page has been updated."; return r.dump();
    }
    if (action != "open") return Err("Unknown portal action: " + action);
    const int port = a.value("port", 0);
    if (port < 1 || port > 65535) return Err("port must be an integer from 1 to 65535");
    if (!S(a, "path").empty() && S(a, "path")[0] != '/') return Err("path must start with /");
    json p = env; p["port"] = port;
    for (auto k : {"title", "description", "path"}) if (a.contains(k) && a[k].is_string()) p[k] = a[k];
    auto r = Rpc("portal.open", p); if (Failed(r)) return r.dump();
    r["text"] = "Portal route allocated at " + r.value("url", std::string()) + ". Pass PUBLIC_URL=" + r.value("publicUrl", r.value("url", std::string())) +
                " and PORT=" + std::to_string(r.value("port", port)) + " when starting the dev server.";
    return r.dump();
}

// ---------------------------------------------------------------- dashboard (per-session tabs + widgets)
fs::path DashboardPath(const std::string& key) { return agent::AgentsRoot().parent_path() / "dashboards" / (Hex(key).substr(0, 32) + ".json"); }
std::string DashboardTool(const json& a, const std::string& agent, const std::string& key) {
    if (key.empty()) return Err("agent session required");
    const auto action = S(a, "action");
    static const std::regex tab_re("^[a-z0-9-]{1,40}$"), name_re("^[a-z0-9][a-z0-9._-]{0,63}$"), kind_re("^[a-z0-9][a-z0-9-]{0,63}:[a-z0-9][a-z0-9._-]{0,63}$");
    std::lock_guard<std::mutex> l(g_mu);
    const auto path = DashboardPath(key);
    json db = json::parse(ReadFile(path), nullptr, false);
    if (!db.is_object()) db = {{"sessionKey", key}, {"revision", 0}, {"tabs", json::array({{{"tabId", "main"}, {"title", "Main"}, {"widgets", json::array()}}})}, {"presentation", "split"}};
    auto snapshot = [&]() { return json{{"sessionKey", key}, {"revision", db["revision"]}, {"tabs", db["tabs"]}, {"defaultPresentation", db.value("presentation", "split")}}; };
    auto save = [&]() { db["revision"] = db.value("revision", 0) + 1; db["updatedAtMs"] = NowMs(); WriteFile(path, db.dump(2));
                        GatewayEmit("board.changed", {{"sessionKey", key}, {"revision", db["revision"]}, {"source", "dashboard"}}); return snapshot().dump(); };
    auto tab_of = [&](const std::string& id) -> json* { for (auto& t : db["tabs"]) if (t["tabId"] == id) return &t; return nullptr; };
    auto find_widget = [&](const std::string& n, json** tab, size_t* idx) { for (auto& t : db["tabs"]) for (size_t i = 0; i < t["widgets"].size(); ++i) if (t["widgets"][i]["name"] == n) { *tab = &t; *idx = i; return true; } return false; };
    const auto tabId = S(a, "tabId"), name = S(a, "name");
    if (!tabId.empty() && !std::regex_match(tabId, tab_re)) return Err("tabId must be a lowercase slug up to 40 characters");
    if (!name.empty() && !std::regex_match(name, name_re)) return Err("name must be a stable widget name");
    if (action == "read") return snapshot().dump();
    if (action == "set_default_presentation" || action == "set_presentation") {
        const auto pr = S(a, "presentation"); if (pr != "split" && pr != "expanded") return Err("presentation must be split or expanded");
        if (action == "set_presentation") { GatewayEmit("board.command", {{"sessionKey", key}, {"agentId", agent}, {"command", {{"kind", "set_chat_dock"}, {"dock", pr == "expanded" ? "hidden" : "right"}}}}); return json{{"ok", true}, {"sent", true}}.dump(); }
        db["presentation"] = pr; save(); return json{{"ok", true}, {"sessionKey", key}, {"defaultPresentation", pr}}.dump();
    }
    if (action == "focus_tab") { if (!tab_of(tabId)) return Err("unknown tab: " + tabId); GatewayEmit("board.command", {{"sessionKey", key}, {"agentId", agent}, {"command", {{"kind", "focus_tab"}, {"tabId", tabId}}}}); return json{{"ok", true}, {"sent", true}}.dump(); }
    if (action == "tab_create") { if (tabId.empty() || S(a, "title").empty()) return Err("tab_create requires tabId and title"); if (tab_of(tabId)) return Err("tab exists: " + tabId);
        db["tabs"].push_back({{"tabId", tabId}, {"title", S(a, "title")}, {"widgets", json::array()}}); return save(); }
    if (action == "tab_update") { auto* t = tab_of(tabId); if (!t) return Err("unknown tab: " + tabId);
        if (S(a, "title").empty() && !a.contains("position")) return Err("tab_update requires title or position");
        if (!S(a, "title").empty()) (*t)["title"] = S(a, "title");
        if (a.contains("position")) { json copy = *t; auto& tabs = db["tabs"]; for (size_t i = 0; i < tabs.size(); ++i) if (tabs[i]["tabId"] == tabId) { tabs.erase(i); break; }
            const size_t pos = std::min<size_t>(std::max(0, a.value("position", 0)), tabs.size()); tabs.insert(tabs.begin() + pos, copy); }
        return save(); }
    if (action == "tab_delete") { auto& tabs = db["tabs"]; for (size_t i = 0; i < tabs.size(); ++i) if (tabs[i]["tabId"] == tabId) { tabs.erase(i); return save(); } return Err("unknown tab: " + tabId); }
    if (action == "tabs_reorder") {
        if (!a.contains("tabIds") || !a["tabIds"].is_array() || a["tabIds"].size() != db["tabs"].size()) return Err("tabIds must list every tab exactly once");
        json out = json::array(); for (auto& id : a["tabIds"]) { auto* t = id.is_string() ? tab_of(id.get<std::string>()) : nullptr; if (!t) return Err("unknown tab in tabIds"); out.push_back(*t); }
        db["tabs"] = out; return save(); }
    if (action == "widget_put") {
        const auto kind = S(a, "pluginKind");
        if (!std::regex_match(kind, kind_re)) return Err("pluginKind must use the <pluginId>:<name> format");
        if (name.empty()) return Err("name required");
        if (a.contains("props") && !a["props"].is_object()) return Err("props must be an object");
        if (a.contains("props") && a["props"].dump().size() > 8192) return Err("props exceed 8KB");
        json w{{"name", name}, {"title", S(a, "title").empty() ? name : S(a, "title")}, {"content", {{"kind", "plugin"}, {"pluginKind", kind}, {"props", a.value("props", json::object())}}},
               {"size", S(a, "size").empty() ? "md" : S(a, "size")}};
        json* t = nullptr; size_t i = 0;
        if (find_widget(name, &t, &i)) { w["size"] = S(a, "size").empty() ? (*t)["widgets"][i].value("size", "md") : S(a, "size"); (*t)["widgets"][i] = w; }
        else { t = tabId.empty() ? &db["tabs"][0] : tab_of(tabId); if (!t) return Err("unknown tab: " + tabId);
            size_t pos = (*t)["widgets"].size(); if (!S(a, "after").empty()) for (size_t k = 0; k < (*t)["widgets"].size(); ++k) if ((*t)["widgets"][k]["name"] == S(a, "after")) pos = k + 1;
            (*t)["widgets"].insert((*t)["widgets"].begin() + pos, w); }
        Rpc("board.widget.put", {{"sessionKey", key}, {"agentId", agent}, {"name", name}, {"title", w["title"]}, {"content", w["content"]}});
        return save();
    }
    if (action == "widget_move" || action == "widget_resize" || action == "widget_remove") {
        json* t = nullptr; size_t i = 0; if (name.empty() || !find_widget(name, &t, &i)) return Err("unknown widget: " + name);
        if (action == "widget_remove") { (*t)["widgets"].erase(i); return save(); }
        if (action == "widget_resize") {
            if (!S(a, "size").empty()) (*t)["widgets"][i]["size"] = S(a, "size");
            if (a.contains("sizeW")) (*t)["widgets"][i]["sizeW"] = std::clamp(a.value("sizeW", 1), 1, 12);
            if (a.contains("sizeH")) (*t)["widgets"][i]["sizeH"] = std::clamp(a.value("sizeH", 1), 1, 20);
            return save(); }
        if (a.contains("position") && !S(a, "after").empty()) return Err("widget_move accepts either position or after, not both");
        json w = (*t)["widgets"][i]; (*t)["widgets"].erase(i);
        json* dst = tabId.empty() ? t : tab_of(tabId); if (!dst) return Err("unknown tab: " + tabId);
        size_t pos = (*dst)["widgets"].size();
        if (a.contains("position")) pos = std::min<size_t>(std::max(0, a.value("position", 0)), pos);
        if (!S(a, "after").empty()) for (size_t k = 0; k < (*dst)["widgets"].size(); ++k) if ((*dst)["widgets"][k]["name"] == S(a, "after")) pos = k + 1;
        (*dst)["widgets"].insert((*dst)["widgets"].begin() + pos, w); return save();
    }
    return Err("Unknown dashboard action: " + action);
}

// ---------------------------------------------------------------- node-backed: computer / mobile_ui
std::string PickNode(const std::string& want, const std::string& command) {
    auto& hub = NodeHub::Global();
    if (!want.empty()) return hub.Connected(want) && hub.HasCommand(want, command) ? want : std::string();
    for (auto& n : hub.List()) { const auto id = n.value("nodeId", ""); if (hub.HasCommand(id, command)) return id; }
    return {};
}
std::string NodeCall(const std::string& node, const std::string& command, const json& params, int timeout_ms) {
    auto r = NodeHub::Global().Invoke(node, command, params, timeout_ms);
    if (!r.value("ok", false)) return json{{"ok", false}, {"nodeId", node}, {"error", r.value("error", json("node invoke failed"))}}.dump();
    json out = r.value("payload", json::object()); if (!out.is_object()) out = {{"result", out}};
    out["nodeId"] = node;
    // Screenshots come back as base64 PNG/JPEG; expose them as a dataUrl so the loop attaches them as image input.
    for (auto k : {"image", "screenshot", "imageBase64"})
        if (out.contains(k) && out[k].is_string() && !out.contains("dataUrl")) {
            const auto& b = out[k].get_ref<const std::string&>();
            out["dataUrl"] = b.rfind("data:", 0) == 0 ? b : "data:" + out.value("mimeType", std::string("image/png")) + ";base64," + b;
            out.erase(k);
        }
    return out.dump();
}
std::string ComputerTool(const json& a) {
    static const std::set<std::string> acts = {"screenshot", "left_click", "right_click", "middle_click", "double_click", "triple_click", "mouse_move", "left_click_drag",
                                               "left_mouse_down", "left_mouse_up", "scroll", "type", "key", "hold_key", "wait"};
    const auto action = S(a, "action");
    if (!acts.count(action)) return Err("unsupported computer action: " + action);
    const auto node = PickNode(S(a, "node"), "computer.act");
    if (node.empty()) return Err("no connected node declares computer.act (pair a desktop node with computer use enabled)", "UNAVAILABLE");
    json p = a; p.erase("node");
    return NodeCall(node, "computer.act", p, std::clamp(a.value("timeoutMs", 30000), 1000, 120000));
}
std::string MobileUiTool(const json& a) {
    const auto action = S(a, "action");
    if (action != "observe" && action != "act") return Err("action must be observe or act");
    const auto cmd = action == "observe" ? "mobile.ui.observe" : "mobile.ui.act";
    const auto node = PickNode(S(a, "node"), cmd);
    if (node.empty()) return Err(std::string("no connected node declares ") + cmd + " (pair the Android app with mobile UI control enabled)", "UNAVAILABLE");
    json p = json::object();
    if (action == "act") {
        if (S(a, "snapshotId").empty()) return Err("act: snapshotId from the latest observation is required");
        if (!a.contains("action_spec") && !a.contains("uiAction")) return Err("act: uiAction is required");
        const json ua = a.contains("uiAction") ? a["uiAction"] : a["action_spec"];
        static const std::set<std::string> types = {"activate", "set_text", "scroll", "tap", "swipe", "global_action", "wait"};
        if (!ua.is_object() || !types.count(ua.value("type", ""))) return Err("act: uiAction.type must be one of activate, set_text, scroll, tap, swipe, global_action, wait");
        p = {{"snapshotId", S(a, "snapshotId")}, {"action", ua}};
    } else if (a.contains("maxNodes")) p["maxNodes"] = a["maxNodes"];
    return NodeCall(node, cmd, p, std::clamp(a.value("timeoutMs", 30000), 1000, 100000));
}

// ---------------------------------------------------------------- decision_evaluate
std::string DecisionModel(const json& cfg, const std::string& agent) {
    if (auto list = config::GetPath(cfg, "agents.list"); list && list->is_array())
        for (auto& e : *list) if (e.is_object() && e.value("id", "") == agent && e.contains("decisionModel") && e["decisionModel"].is_string()) return e["decisionModel"];
    if (auto v = config::GetPath(cfg, "agents.defaults.decisionModel"); v && v->is_string()) return *v;
    return {};
}
std::string DecisionTool(const json& a, const std::string& agent, const json& cfg) {
    // 1.139: prefer a plugin registerDecisionProvider when one is loaded.
    {
        auto plug = GatewayRpc("plugins.invokeRegistration", {{"kind", "registerDecisionProvider"}, {"method", "evaluate"}, {"args", a}, {"agentId", agent}});
        if (plug.value("ok", false)) {
            auto payload = plug.contains("payload") ? plug["payload"] : plug;
            if (payload.value("ok", false) && payload.contains("result")) {
                json ans = payload["result"];
                if (ans.is_object() && ans.contains("result")) ans = ans["result"];
                return json{{"status", "ok"}, {"result", ans}, {"provenance", {{"provider", "plugin"}, {"plugin", payload.value("plugin", "")}}}}.dump();
            }
        }
    }
    const auto model = DecisionModel(cfg, agent);
    if (model.empty()) return json{{"status", "unavailable"}, {"reason", "no-decision-model"}, {"guidance", "Set agents.defaults.decisionModel to enable decision_evaluate."}}.dump();
    auto bad = [] { return Err("Invalid decision_evaluate input: provide state and a nonempty questions map with boolean, choice, or score questions; no evidence was sent."); };
    if (!a.is_object() || !a.contains("state") || !a.contains("questions") || !a["questions"].is_object() || a["questions"].empty()) return bad();
    for (auto& [k, v] : a.items()) if (k != "state" && k != "questions") return bad();
    for (auto& [q, spec] : a["questions"].items()) {
        const auto t = spec.is_object() ? spec.value("type", "") : "";
        if (t == "choice" && !(spec.contains("criteria") && spec["criteria"].is_object() && spec["criteria"].size() >= 2)) return bad();
        if (t == "score" && !(spec.contains("criteria") && spec["criteria"].is_array() && spec["criteria"].size() >= 2)) return bad();
        if (t != "boolean" && t != "choice" && t != "score") return bad();
    }
    auto* router = g_router.load();
    if (!router) return json{{"status", "unavailable"}, {"reason", "no-router"}}.dump();
    std::vector<agent::Message> msgs{
        {.role = "system", .content = "You evaluate questions about the supplied state only. Reply with one JSON object and nothing else. For each question id: "
                                      "boolean -> {\"probabilityTrue\": number 0..1}; choice -> {\"distribution\": {alternative: probability}} summing to 1; "
                                      "score -> {\"distribution\": [probability per level]} summing to 1."},
        {.role = "user", .content = json{{"state", a["state"]}, {"questions", a["questions"]}}.dump()}};
    auto r = router->CompleteRoute(model, msgs, json::array());
    if (!r.ok) return json{{"status", "unavailable"}, {"reason", "provider-error"}, {"detail", r.error}}.dump();
    auto txt = r.message.content; const auto s = txt.find('{'), e = txt.rfind('}');
    json parsed = s == std::string::npos ? json() : json::parse(txt.substr(s, e - s + 1), nullptr, false);
    if (!parsed.is_object()) return json{{"status", "unavailable"}, {"reason", "unparseable-output"}}.dump();
    json result = json::object();
    for (auto& [q, spec] : a["questions"].items()) {
        const auto t = spec["type"].get<std::string>();
        const json ans = parsed.value(q, json::object());
        if (t == "boolean") { double p = ans.is_object() ? ans.value("probabilityTrue", -1.0) : ans.is_number() ? ans.get<double>() : -1.0;
            if (p < 0 || p > 1) return json{{"status", "unavailable"}, {"reason", "invalid-output"}, {"question", q}}.dump();
            result[q] = {{"type", "boolean"}, {"probabilityTrue", p}}; continue; }
        json dist = ans.is_object() ? ans.value("distribution", json()) : json();
        double sum = 0;
        if (t == "choice") { if (!dist.is_object()) return json{{"status", "unavailable"}, {"reason", "invalid-output"}, {"question", q}}.dump();
            json d = json::object(); for (auto& [alt, _] : spec["criteria"].items()) { double v = dist.value(alt, 0.0); d[alt] = std::max(0.0, v); sum += std::max(0.0, v); }
            if (sum <= 0) return json{{"status", "unavailable"}, {"reason", "invalid-output"}, {"question", q}}.dump();
            for (auto& [alt, v] : d.items()) v = v.get<double>() / sum;
            result[q] = {{"type", "choice"}, {"distribution", d}}; continue; }
        const size_t n = spec["criteria"].size();
        if (!dist.is_array() || dist.size() != n) return json{{"status", "unavailable"}, {"reason", "invalid-output"}, {"question", q}}.dump();
        std::vector<double> d; for (auto& v : dist) { d.push_back(std::max(0.0, v.is_number() ? v.get<double>() : 0.0)); sum += d.back(); }
        if (sum <= 0) return json{{"status", "unavailable"}, {"reason", "invalid-output"}, {"question", q}}.dump();
        double expected = 0; for (size_t i = 0; i < n; ++i) { d[i] /= sum; expected += d[i] * static_cast<double>(i) / static_cast<double>(n - 1); }
        result[q] = {{"type", "score"}, {"distribution", d}, {"expectedPosition", expected}};
    }
    const auto rubric = "decision-v1-" + Hex(a["questions"].dump()).substr(0, 24);
    return json{{"status", "ok"}, {"result", result}, {"provenance", {{"model", model}, {"rubricVersion", rubric}}},
                {"note", "Results never authorize actions; confidence is not demonstrated accuracy."}}.dump();
}

// ---------------------------------------------------------------- personal_instructions / github_identity_status
std::string PersonalInstructionsTool(const json& a, const std::string& agent) {
    const auto action = S(a, "action");
    const auto target = S(a, "agentId").empty() ? agent : S(a, "agentId");
    if (action == "get") {
        auto r = Rpc("users.personalFile.get", {{"agentId", target}, {"name", "USER.md"}});
        if (Failed(r)) return r.dump();
        if (!r.contains("hash")) r["hash"] = r.value("exists", false) ? json(Hex(r.value("content", std::string()))) : json(nullptr);
        return r.dump();
    }
    if (action != "set" || !a.contains("content") || !a["content"].is_string() || !a.contains("expectedHash"))
        return Err("Use get, or set with content and the hash returned by get.");
    if (a["content"].get<std::string>().size() > 4000) return Err("content exceeds 4,000 characters");
    auto cur = Rpc("users.personalFile.get", {{"agentId", target}, {"name", "USER.md"}});
    if (Failed(cur)) return cur.dump();
    const json have = cur.value("exists", false) ? json(cur.contains("hash") ? cur["hash"] : json(Hex(cur.value("content", std::string())))) : json(nullptr);
    if (have != a["expectedHash"]) return json{{"ok", false}, {"code", "hash_mismatch"}, {"error", "USER.md changed since get; get again and retry"}, {"hash", have}}.dump();
    auto r = Rpc("users.personalFile.set", {{"agentId", target}, {"name", "USER.md"}, {"content", a["content"]}});
    if (Failed(r)) return r.dump();
    r["hash"] = Hex(a["content"].get<std::string>());
    return r.dump();
}
std::string GithubIdentityTool(const std::string& agent) {
    auto r = Rpc("tools.github.status", {{"agentId", agent}, {"selectedScope", "agent"}, {"scope", "agent"}});
    if (Failed(r)) return r.dump();
    const json eff = r.value("effective", json::object());
    const auto cred = eff.value("credentialState", std::string(r.value("connected", false) ? "available" : "missing"));
    const auto refresh = eff.value("refreshState", std::string());
    if (cred != "available" || refresh == "expired" || refresh == "failed" || refresh == "unavailable")
        r["nextAction"] = "Ask the operator to connect or reconnect GitHub under Settings → Agents → Tools.";
    return r.dump();
}

// ---------------------------------------------------------------- skill_workshop (versioned learned skills)
bool ValidSkillName(const std::string& n) { static const std::regex re("^[a-z0-9][a-z0-9-]{0,63}$"); return std::regex_match(n, re); }
bool ValidSupportPath(const std::string& p) {
    if (p == "SKILL.md") return true;
    static const std::regex re("^(references|templates|scripts|assets)/[A-Za-z0-9._/-]{1,200}$");
    return std::regex_match(p, re) && p.find("..") == std::string::npos;
}
std::string SkillWorkshopTool(const json& a, const std::string& agent) {
    const fs::path root = WorkshopSkillsDir(agent), archive = root.parent_path() / "workshop-archive", versions = root.parent_path() / "workshop-versions";
    const auto action = S(a, "action"), name = S(a, "name");
    std::lock_guard<std::mutex> l(g_mu);
    std::error_code ec;
    if (action == "list") {
        std::ostringstream o; int n = 0;
        if (fs::is_directory(root, ec)) for (auto& e : fs::directory_iterator(root)) if (fs::exists(e.path() / "SKILL.md")) { ++n; o << "- " << e.path().filename().string() << "\n"; }
        std::string arch; if (fs::is_directory(archive, ec)) for (auto& e : fs::directory_iterator(archive)) arch += "- " + e.path().filename().string() + "\n";
        std::string text = n ? "Learned skills:\n" + o.str() : "No learned skills yet. Add one with action=create.";
        if (!arch.empty()) text += "\nArchived (restore with action=restore):\n" + arch;
        return json{{"text", text}, {"root", root.string()}}.dump();
    }
    if (!ValidSkillName(name)) return Err("name must be a lowercase slug (a-z, 0-9, -)");
    const fs::path dir = root / name;
    const auto file = S(a, "file_path").empty() ? std::string("SKILL.md") : S(a, "file_path");
    if (!ValidSupportPath(file)) return Err("file_path must be SKILL.md or under references/, templates/, scripts/, assets/");
    auto snapshot = [&](const std::string& reason) {  // version the whole skill dir before a change
        if (!fs::is_directory(dir, ec)) return 0;
        int v = 1; if (fs::is_directory(versions / name, ec)) for (auto& e : fs::directory_iterator(versions / name)) v = std::max(v, std::atoi(e.path().filename().c_str()) + 1);
        fs::create_directories(versions / name / std::to_string(v)); fs::copy(dir, versions / name / std::to_string(v), fs::copy_options::recursive, ec);
        WriteFile(versions / name / std::to_string(v) / ".reason", reason); return v;
    };
    auto changed = [&](const std::string& what) {
        GatewayEmit("skills.changed", {{"agentId", agent}, {"skill", name}, {"change", what}, {"reason", S(a, "reason")}});
        return json{{"ok", true}, {"skill", name}, {"change", what}, {"path", (dir / file).string()},
                    {"text", "Skill " + name + " " + what + ". Undo with action=restore name=" + name + "."}}.dump();
    };
    if (action == "view") {
        if (!S(a, "version").empty() || a.contains("version")) {
            const auto v = a["version"].is_string() ? a["version"].get<std::string>() : std::to_string(a.value("version", 0));
            const auto p = versions / name / v / file; if (!fs::exists(p)) return Err("version not found: " + v);
            return json{{"name", name}, {"version", v}, {"file_path", file}, {"content", ReadFile(p)}}.dump();
        }
        if (!fs::exists(dir / file)) return Err("skill file not found: " + name + "/" + file);
        json files = json::array(); for (auto& e : fs::recursive_directory_iterator(dir)) if (e.is_regular_file()) files.push_back(fs::relative(e.path(), dir).string());
        return json{{"name", name}, {"file_path", file}, {"content", ReadFile(dir / file)}, {"files", files}}.dump();
    }
    if (action == "create") {
        const auto content = S(a, "content");
        if (fs::exists(dir / "SKILL.md")) return Err("skill exists: " + name + " (use patch or write_file)");
        static const std::regex fm("^---\\s*\\n([\\s\\S]*?)\\n---");
        std::smatch m; if (!std::regex_search(content, m, fm)) return Err("content must start with YAML frontmatter (name, description)");
        const std::string front = m[1];
        if (!std::regex_search(front, std::regex("(^|\\n)name:\\s*" + name + "\\s*(\\n|$)"))) return Err("frontmatter name must equal " + name);
        if (!std::regex_search(front, std::regex("(^|\\n)description:\\s*\\S"))) return Err("frontmatter description is required");
        WriteFile(dir / "SKILL.md", content); return changed("created");
    }
    if (!fs::is_directory(dir, ec) && action != "restore") return Err("unknown skill: " + name + ". View it first: call skill_workshop action=list.");
    if (action == "patch") {
        const auto oldt = S(a, "old_text"), newt = S(a, "new_text");
        if (oldt.empty()) return Err("old_text required");
        auto body = ReadFile(dir / file); const auto pos = body.find(oldt);
        if (pos == std::string::npos) return Err("old_text not found. View it first: call skill_workshop action=view name=" + name + ", then retry once.");
        if (body.find(oldt, pos + 1) != std::string::npos) return Err("old_text is not unique; include more context");
        snapshot(S(a, "reason")); body.replace(pos, oldt.size(), newt); WriteFile(dir / file, body); return changed("patched");
    }
    if (action == "write_file") { if (!a.contains("content")) return Err("content required"); snapshot(S(a, "reason")); WriteFile(dir / file, S(a, "content")); return changed("updated"); }
    if (action == "remove_file") { if (file == "SKILL.md") return Err("remove_file only removes support files; use archive for a whole skill");
        if (!fs::exists(dir / file)) return Err("file not found: " + file); snapshot(S(a, "reason")); fs::remove(dir / file, ec); return changed("file removed"); }
    if (action == "archive") {
        if (S(a, "reason").empty() && S(a, "absorbed_into").empty()) return Err("archive requires absorbed_into or reason");
        snapshot(S(a, "reason")); fs::create_directories(archive); fs::remove_all(archive / name, ec); fs::rename(dir, archive / name, ec);
        if (ec) return Err("archive failed: " + ec.message()); return changed("archived");
    }
    if (action == "restore") {
        if (!fs::is_directory(dir, ec) && fs::is_directory(archive / name, ec)) { fs::create_directories(root); fs::rename(archive / name, dir, ec); return changed("restored from archive"); }
        int v = 0;
        if (a.contains("version")) v = a["version"].is_string() ? std::atoi(a["version"].get<std::string>().c_str()) : a.value("version", 0);
        else if (fs::is_directory(versions / name, ec)) for (auto& e : fs::directory_iterator(versions / name)) v = std::max(v, std::atoi(e.path().filename().c_str()));
        if (v <= 0 || !fs::is_directory(versions / name / std::to_string(v), ec)) return Err("nothing to restore");
        fs::remove_all(dir, ec); fs::copy(versions / name / std::to_string(v), dir, fs::copy_options::recursive, ec); fs::remove(dir / ".reason", ec);
        fs::remove_all(versions / name / std::to_string(v), ec);
        return changed("restored to version " + std::to_string(v));
    }
    return Err("Unknown action \"" + action + "\". Use list, view, create, patch, write_file, remove_file, archive, or restore.");
}

// ---------------------------------------------------------------- line_richmenu (1.168: agent tool wrapping LineRichMenu CLI)
std::string LineRichMenuTool(const json& a, const json& cfg) {
    json op = a.is_object() ? a : json::object();
    if (!op.contains("action") || !op["action"].is_string() || op["action"].get<std::string>().empty())
        return Err("action required: create|validate|list|delete|default|link|unlink");
    if (!op.contains("definition") && op.contains("def")) op["definition"] = op["def"];
    if (!op.contains("richMenuId") && op.contains("id")) op["richMenuId"] = op["id"];
    if (!op.contains("userId") && op.contains("user")) op["userId"] = op["user"];
    if (!op.contains("image") && op.contains("imagePath")) op["image"] = op["imagePath"];
    // Load definition from a workspace path when the agent passes definitionPath (or a string definition).
    std::string def_path = S(op, "definitionPath");
    if (def_path.empty() && op.contains("definition") && op["definition"].is_string()) def_path = op["definition"].get<std::string>();
    if (!def_path.empty()) {
        auto body = ReadFile(def_path);
        if (body.empty()) return Err("cannot read definitionPath: " + def_path);
        auto j = json::parse(body, nullptr, false);
        if (!j.is_object()) return Err("definitionPath must be a JSON object: " + def_path);
        op["definition"] = j;
    }
    auto r = channels::LineRichMenu(cfg, op);
    return r.dump();
}

}  // namespace

std::string WorkshopSkillsDir(const std::string& agent) { return (agent::AgentsRoot() / (agent.empty() ? "main" : agent) / "agent" / "workshop-skills").string(); }
void SetModelRouter(provider::Router* router) { g_router.store(router); }

std::vector<std::string> CoreGatewayToolNames() {
    return {"computer", "dashboard", "decision_evaluate", "gateway", "github_identity_status", "line_richmenu", "mobile_ui", "personal_instructions",
            "plugins", "portal", "progress_card", "screen", "skill_workshop", "terminal"};
}

bool CoreGatewayTool(const std::string& name, const json& a, const std::string& agent, const std::string& key, const json& cfg, std::string* out) {
    if (name == "gateway") *out = GatewayTool(a, key, cfg);
    else if (name == "plugins") *out = PluginsTool(a);
    else if (name == "progress_card") *out = ProgressCardTool(a, agent, key);
    else if (name == "screen") *out = ScreenTool(a, agent, key);
    else if (name == "terminal") *out = TerminalTool(a, agent, key, cfg);
    else if (name == "portal") *out = PortalTool(a, key, cfg);
    else if (name == "dashboard") *out = DashboardTool(a, agent, key);
    else if (name == "computer") *out = ComputerTool(a);
    else if (name == "mobile_ui") *out = MobileUiTool(a);
    else if (name == "decision_evaluate") *out = DecisionTool(a, agent, cfg);
    else if (name == "personal_instructions") *out = PersonalInstructionsTool(a, agent);
    else if (name == "github_identity_status") *out = GithubIdentityTool(agent);
    else if (name == "skill_workshop") *out = SkillWorkshopTool(a, agent);
    else if (name == "line_richmenu") *out = LineRichMenuTool(a, cfg);
    else return false;
    return true;
}
}  // namespace openclaw::gateway
