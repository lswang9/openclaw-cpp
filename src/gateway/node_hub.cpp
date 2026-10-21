#include "openclaw/gateway/node_hub.hpp"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>

namespace openclaw::gateway {
namespace {
using json = nlohmann::json;
struct NodeConn {
    std::string id, name, platform, version;
    std::set<std::string> commands;
    json caps = json::array();
    NodeHub::SendFn send;
    long long connected_at = 0;
};
struct Pending {
    std::string node_id;
    int conn = -1;
    bool done = false;
    json result;
};
std::mutex g_mu;
std::condition_variable g_cv;
std::map<int, NodeConn> g_conns;
std::map<std::string, Pending> g_pending;
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string NewId() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) s.push_back(hex[rng() % 16]);
    return s;
}
const NodeConn* Find(const std::string& id) {
    for (auto& [fd, c] : g_conns) if (c.id == id) return &c;
    return nullptr;
}
}  // namespace

NodeHub& NodeHub::Global() { static NodeHub h; return h; }

std::string NodeHub::Attach(int conn, const json& p, SendFn send, std::string* err) {
    const json client = p.contains("client") && p["client"].is_object() ? p["client"] : json::object();
    std::string id = client.value("id", p.value("nodeId", std::string()));
    static const std::regex id_re("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$");
    if (!std::regex_match(id, id_re)) { if (err) *err = "invalid node id"; return {}; }
    NodeConn c;
    c.id = id;
    c.name = client.value("displayName", id);
    c.platform = client.value("platform", "");
    c.version = client.value("version", "");
    if (p.contains("commands") && p["commands"].is_array())
        for (auto& x : p["commands"]) if (x.is_string()) c.commands.insert(x.get<std::string>());
    if (p.contains("caps") && p["caps"].is_array()) c.caps = p["caps"];
    c.send = std::move(send);
    c.connected_at = NowMs();
    std::lock_guard l(g_mu);
    for (auto it = g_conns.begin(); it != g_conns.end();) it = (it->second.id == id && it->first != conn) ? g_conns.erase(it) : std::next(it);  // newest connection wins
    g_conns[conn] = std::move(c);
    return id;
}

std::string NodeHub::Detach(int conn) {
    std::lock_guard l(g_mu);
    auto it = g_conns.find(conn);
    if (it == g_conns.end()) return {};
    const auto id = it->second.id;
    g_conns.erase(it);
    for (auto& [rid, p] : g_pending)
        if (p.conn == conn && !p.done) { p.done = true; p.result = {{"ok", false}, {"error", {{"code", "UNAVAILABLE"}, {"message", "node disconnected"}}}}; }
    g_cv.notify_all();
    return Find(id) ? std::string() : id;
}

bool NodeHub::Connected(const std::string& id) const { std::lock_guard l(g_mu); return Find(id) != nullptr; }
bool NodeHub::HasCommand(const std::string& id, const std::string& cmd) const {
    std::lock_guard l(g_mu);
    auto* c = Find(id);
    return c && c->commands.count(cmd);
}
json NodeHub::Describe(const std::string& id) const {
    std::lock_guard l(g_mu);
    auto* c = Find(id);
    if (!c) return nullptr;
    return {{"nodeId", c->id}, {"displayName", c->name}, {"platform", c->platform}, {"version", c->version},
            {"caps", c->caps}, {"commands", json(std::vector<std::string>(c->commands.begin(), c->commands.end()))},
            {"connected", true}, {"connectedAtMs", c->connected_at}};
}
json NodeHub::List() const {
    std::lock_guard l(g_mu);
    json out = json::array();
    for (auto& [fd, c] : g_conns)
        out.push_back({{"nodeId", c.id}, {"displayName", c.name}, {"platform", c.platform}, {"connected", true},
                       {"commands", json(std::vector<std::string>(c.commands.begin(), c.commands.end()))}});
    return out;
}

json NodeHub::Invoke(const std::string& node_id, const std::string& command, const json& params, int timeout_ms, const std::string& idem) {
    auto fail = [](const std::string& code, const std::string& msg) { return json{{"ok", false}, {"error", {{"code", code}, {"message", msg}}}}; };
    std::unique_lock l(g_mu);
    const NodeConn* c = Find(node_id);
    if (!c) return fail("UNAVAILABLE", "node not connected: " + node_id);
    if (!c->commands.count(command)) return fail("INVALID_REQUEST", "node " + node_id + " does not declare command " + command);
    int conn = -1;
    for (auto& [fd, x] : g_conns) if (&x == c) conn = fd;
    const auto id = NewId();
    g_pending[id] = Pending{node_id, conn, false, json()};
    json ev{{"type", "event"}, {"event", "node.invoke.request"},
            {"payload", {{"id", id}, {"nodeId", node_id}, {"command", command}, {"paramsJSON", params.dump()},
                         {"timeoutMs", timeout_ms}, {"idempotencyKey", idem.empty() ? id : idem}}}};
    auto send = c->send;
    l.unlock();
    if (!send(ev.dump())) { l.lock(); g_pending.erase(id); return fail("UNAVAILABLE", "node connection send failed"); }
    l.lock();
    const bool ok = g_cv.wait_for(l, std::chrono::milliseconds(std::max(1, timeout_ms)), [&] { return g_pending[id].done; });
    json r = ok ? g_pending[id].result : fail("TIMEOUT", "node did not answer " + command + " within " + std::to_string(timeout_ms) + " ms");
    g_pending.erase(id);
    return r;
}

bool NodeHub::Result(int conn, const json& p, std::string* err) {
    std::lock_guard l(g_mu);
    const auto id = p.value("id", std::string());
    auto it = g_pending.find(id);
    if (it == g_pending.end()) { if (err) *err = "unknown or expired invoke id"; return false; }
    if (it->second.conn != conn || p.value("nodeId", it->second.node_id) != it->second.node_id) { if (err) *err = "invoke result from a different node"; return false; }
    json r{{"ok", p.value("ok", false)}};
    if (r["ok"].get<bool>()) {
        if (p.contains("payloadJSON") && p["payloadJSON"].is_string()) {
            r["payloadJSON"] = p["payloadJSON"];
            try { r["payload"] = json::parse(p["payloadJSON"].get<std::string>()); } catch (...) { r["payload"] = nullptr; }
        } else { r["payload"] = p.value("payload", json()); r["payloadJSON"] = r["payload"].dump(); }
    } else {
        r["error"] = p.contains("error") && p["error"].is_object() ? p["error"] : json{{"code", "UNAVAILABLE"}, {"message", p.value("error", std::string("node invoke failed"))}};
    }
    it->second.result = r;
    it->second.done = true;
    g_cv.notify_all();
    return true;
}

}  // namespace openclaw::gateway
