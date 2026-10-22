#include "openclaw/plugins/js_host.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace openclaw::plugins {
namespace {

bool EnvTruthy(const char* k) {
    const char* v = std::getenv(k);
    if (!v || !*v) return false;
    return std::string(v) == "1" || std::string(v) == "true" || std::string(v) == "yes";
}

bool ForceMock() {
    return EnvTruthy("OPENCLAW_JS_FORCE_MOCK") || EnvTruthy("OPENCLAW_JS_PLUGIN_FORCE_MOCK");
}

std::filesystem::path FindNode() {
    // OPENCLAW_NODE overrides (e.g. a Node >= 22.6 build for TypeScript plugins); else PATH "node".
    if (const char* n = std::getenv("OPENCLAW_NODE"); n && *n) return n;
    return "node";
}

bool NodeSupportsStripTypes() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    cached = 0;
    const std::string cmd = "'" + FindNode().string() + "' --version 2>/dev/null";
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[64] = {0};
        if (fgets(buf, sizeof buf, p)) {
            int maj = 0, min = 0;
            if (std::sscanf(buf, "v%d.%d", &maj, &min) == 2) cached = (maj > 22 || (maj == 22 && min >= 6)) ? 1 : 0;
        }
        pclose(p);
    }
    return cached == 1;
}

}  // namespace

JsHost::JsHost() = default;
JsHost::~JsHost() { Stop(); }

void JsHost::EnableMock() {
    std::lock_guard lock(mu_);
    mock_ = true;
    started_ = true;
    last_error_.clear();
}

bool JsHost::Start(const std::filesystem::path& plugins_dir,
                   const std::filesystem::path& bridge_js) {
    Stop();
    plugins_dir_ = plugins_dir;
    bridge_js_ = bridge_js;
    if (bridge_js_.empty()) {
        bridge_js_ = plugins_dir_ / "js_bridge" / "host.mjs";
    }
    if (ForceMock() || !std::filesystem::exists(bridge_js_)) {
        EnableMock();
        if (!std::filesystem::exists(bridge_js_)) {
            last_error_ = "bridge missing — mock mode";
        } else {
            last_error_ = "FORCE_MOCK";
        }
        { std::lock_guard sl(state_mu_); last_init_ = MockHandle("initialize", {{"plugins_dir", plugins_dir_.string()}}); }
        return true;
    }

    auto proc = std::make_unique<mcp::StdioProcess>();
    std::vector<std::string> argv{FindNode().string()};
    if (NodeSupportsStripTypes()) { argv.push_back("--experimental-strip-types"); argv.push_back("--no-warnings"); }
    argv.push_back(bridge_js_.string());
    if (!proc->Spawn(argv)) {
        last_error_ = "failed to spawn node bridge — falling back to mock";
        EnableMock();
        { std::lock_guard sl(state_mu_); last_init_ = MockHandle("initialize", {{"plugins_dir", plugins_dir_.string()}}); }
        return true;
    }
    {
        std::lock_guard lock(mu_);
        proc_ = std::move(proc);
        mock_ = false;
        started_ = true;
        last_error_.clear();
    }
    {
        auto init = Initialize();
        std::lock_guard sl(state_mu_);
        last_init_ = std::move(init);
        if (last_init_.contains("error")) last_error_ = last_init_["error"].dump();
    }
    return true;
}

void JsHost::Stop() {
    std::lock_guard lock(mu_);
    if (proc_) {
        try {
            // best-effort shutdown
            nlohmann::json req{{"jsonrpc", "2.0"},
                               {"id", next_id_++},
                               {"method", "shutdown"},
                               {"params", nlohmann::json::object()}};
            proc_->WriteLine(req.dump());
        } catch (...) {
        }
        proc_->Close();
        proc_.reset();
    }
    started_ = false;
    mock_ = false;
}

bool JsHost::Running() const {
    std::lock_guard lock(mu_);
    if (mock_) return started_;
    return proc_ && proc_->Alive();
}

nlohmann::json JsHost::Status() const {
    nlohmann::json init;
    { std::lock_guard sl(state_mu_); init = last_init_; }
    std::lock_guard lock(mu_);
    return {{"running", started_},
            {"mock", mock_},
            {"plugins_dir", plugins_dir_.string()},
            {"bridge", bridge_js_.string()},
            {"force_mock", ForceMock()},
            {"last_error", last_error_},
            {"init", init},
            {"impl", mock_ ? "real-sim" : "real"},
            {"gaps", nlohmann::json::array({"registerChannel/registerProvider/registerHttpRoute unsupported",
                                            "no hook lifecycle / channel bindings",
                                            "stdio JSON-RPC subset only",
                                            "requires Node.js for live mode"})}};
}

nlohmann::json JsHost::MockHandle(const std::string& method, const nlohmann::json& params) {
    // Mirrors sample_js without executing JS.
    if (method == "initialize" || method == "ping") {
        return {{"ok", true},
                {"plugins", nlohmann::json::array({"sample_js"})},
                {"tools", nlohmann::json::array({"js.sample_echo", "js.sample_add"})},
                {"host", "openclaw-cpp-js-mock"},
                {"version", "1.12.0-dev"},
                {"mock", true}};
    }
    if (method == "list_plugins") {
        return {{"plugins",
                 nlohmann::json::array(
                     {{{"name", "sample_js"},
                       {"tools", nlohmann::json::array({"sample_echo", "sample_add"})}}})}};
    }
    if (method == "list_tools") {
        return {
            {"tools",
             nlohmann::json::array(
                 {{{"name", "js.sample_echo"},
                   {"description", "Echo text from the sample JS plugin (js.sample_echo)"},
                   {"parameters",
                    {{"type", "object"},
                     {"properties", {{"text", {{"type", "string"}}}}},
                     {"required", nlohmann::json::array({"text"})}}},
                   {"plugin", "sample_js"}},
                  {{"name", "js.sample_add"},
                   {"description", "Add two numbers in JS (js.sample_add)"},
                   {"parameters",
                    {{"type", "object"},
                     {"properties",
                      {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}}}},
                   {"plugin", "sample_js"}}})}};
    }
    if (method == "call_tool") {
        const auto name = params.value("name", "");
        const auto args = params.value("arguments", nlohmann::json::object());
        if (name == "js.sample_echo") {
            return {{"ok", true},
                    {"content", std::string("js-echo:") + args.value("text", "")},
                    {"impl", "real-sim"},
                    {"mock", true}};
        }
        if (name == "js.sample_add") {
            const double a = args.value("a", 0.0);
            const double b = args.value("b", 0.0);
            return {{"ok", true},
                    {"content", std::to_string(a + b)},
                    {"sum", a + b},
                    {"impl", "real-sim"},
                    {"mock", true}};
        }
        return {{"error", {{"message", "unknown tool: " + name}}}};
    }
    if (method == "shutdown") return {{"ok", true}};
    return {{"error", {{"message", "unknown method"}}}};
}

nlohmann::json JsHost::Request(const std::string& method, const nlohmann::json& params) {
    std::lock_guard lock(mu_);
    if (mock_ || !proc_) {
        return MockHandle(method, params);
    }
    const int id = next_id_++;
    inflight_ = method;
    in_request_.store(true, std::memory_order_release);
    struct ClearInflight {
        JsHost* self;
        ~ClearInflight() { self->inflight_.clear(); self->in_request_.store(false, std::memory_order_release); }
    } clear{this};
    nlohmann::json req{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    try {
        {
            std::lock_guard w(write_mu_);
            proc_->WriteLine(req.dump());
        }
        nlohmann::json resp;
        for (int guard = 0; guard < 1000; ++guard) {  // skip any stray/non-matching lines
            const auto line = proc_->ReadLine();
            try { resp = nlohmann::json::parse(line); } catch (...) { continue; }
            if (resp.contains("hostCall")) {  // reverse call from a plugin (api.runtime.*)
                const long long hid = resp["hostCall"].get<long long>();
                nlohmann::json out;
                std::string err;
                try {
                    if (!host_call) err = "host calls not supported";
                    else out = host_call(hid, resp.value("method", ""), resp.value("params", nlohmann::json::object()));
                } catch (const std::exception& e) { err = e.what(); }
                if (out.is_object() && out.value("__deferred", false)) { guard = 0; continue; }
                nlohmann::json reply{{"hostReply", hid}};
                if (!err.empty()) reply["error"] = err; else reply["result"] = out;
                {
                    std::lock_guard w(write_mu_);
                    proc_->WriteLine(reply.dump());
                }
                guard = 0;
                continue;
            }
            if (resp.contains("id") && resp["id"] == id) break;
        }
        if (resp.contains("error")) return {{"error", resp["error"]}};
        return resp.value("result", nlohmann::json::object());
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return {{"error", {{"message", e.what()}}}};
    }
}

void JsHost::SendHostReply(long long id, const nlohmann::json& result, const std::string& error) {
    if (mock_) return;
    nlohmann::json reply{{"hostReply", id}};
    if (!error.empty()) reply["error"] = error; else reply["result"] = result;
    std::lock_guard w(write_mu_);
    if (!proc_) return;
    try { proc_->WriteLine(reply.dump()); } catch (...) {}
}

void JsHost::SendHostEvent(long long id, const std::string& type, const nlohmann::json& payload) {
    if (mock_) return;
    nlohmann::json ev{{"hostEvent", id}, {"type", type}};
    if (payload.is_object()) for (auto& [k, v] : payload.items()) ev[k] = v;
    else ev["payload"] = payload;
    std::lock_guard w(write_mu_);
    if (!proc_) return;
    try { proc_->WriteLine(ev.dump()); } catch (...) {}
}

nlohmann::json JsHost::Initialize() {
    nlohmann::json dirs = nlohmann::json::array({plugins_dir_.string()});
    nlohmann::json cfg;
    {
        std::lock_guard l(state_mu_);
        for (auto& d : extra_dirs_) dirs.push_back(d.string());
        cfg = config_;
    }
    return Request("initialize", {{"plugins_dir", plugins_dir_.string()}, {"plugins_dirs", dirs}, {"config", cfg}});
}

nlohmann::json JsHost::ListCommands() { return Request("list_commands", nlohmann::json::object()); }

namespace {
bool InitListHas(const nlohmann::json& init, const char* key, const std::string& v) {
    if (!init.contains(key) || !init[key].is_array()) return false;
    for (auto& x : init[key]) if (x.is_string() && x.get<std::string>() == v) return true;
    return false;
}
}  // namespace

nlohmann::json JsHost::MatchHttpRoute(const std::string& path) const {
    std::lock_guard l(state_mu_);
    nlohmann::json any;
    for (auto& r : last_init_.value("httpRoutes", nlohmann::json::array())) {
        const auto p = r.value("path", ""), m = r.value("match", "exact");
        if (m == "any") { if (any.is_null()) any = r; continue; }
        if ((m == "prefix" && path.rfind(p, 0) == 0) || path == p) return r;
    }
    return any;
}

std::vector<std::string> JsHost::InitList(const char* key) const {
    std::lock_guard l(state_mu_);
    std::vector<std::string> out;
    if (last_init_.contains(key) && last_init_[key].is_array())
        for (auto& x : last_init_[key]) if (x.is_string()) out.push_back(x);
    return out;
}

bool JsHost::HasCommand(const std::string& n) const {
    std::string low; for (char c : n) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::lock_guard l(state_mu_); return InitListHas(last_init_, "commands", low);
}
bool JsHost::HasHook(const std::string& e) const { std::lock_guard l(state_mu_); return InitListHas(last_init_, "hooks", e); }
bool JsHost::HasRegistration(const std::string& k) const {
    std::lock_guard l(state_mu_);
    return last_init_.contains("registrations") && last_init_["registrations"].is_object() && last_init_["registrations"].value(k, 0) > 0;
}
bool JsHost::HasGatewayMethod(const std::string& m) const { std::lock_guard l(state_mu_); return InitListHas(last_init_, "gatewayMethods", m); }

nlohmann::json JsHost::RunCommand(const std::string& name, const std::string& args, const nlohmann::json& ctx) {
    nlohmann::json p = ctx.is_object() ? ctx : nlohmann::json::object();
    p["name"] = name; p["args"] = args;
    return Request("run_command", p);
}

nlohmann::json JsHost::EmitHook(const std::string& event, const nlohmann::json& payload, const nlohmann::json& ctx) {
    if (RequestInFlight())
        return {{"handlers", 0}, {"results", nlohmann::json::array()}, {"merged", payload}, {"skipped", "reentrant"}};
    return Request("emit_hook", {{"event", event}, {"payload", payload}, {"ctx", ctx.is_null() ? nlohmann::json::object() : ctx}});
}

nlohmann::json JsHost::CallGatewayMethod(const std::string& method, const nlohmann::json& params) {
    return Request("gateway_method", {{"method", method}, {"params", params}});
}

nlohmann::json JsHost::ListTools() { return Request("list_tools", nlohmann::json::object()); }

nlohmann::json JsHost::CallTool(const std::string& name, const nlohmann::json& arguments) {
    return Request("call_tool", {{"name", name}, {"arguments", arguments}});
}

nlohmann::json JsHost::ListPlugins() { return Request("list_plugins", nlohmann::json::object()); }

std::size_t JsHost::RegisterTools(tools::Registry& registry) {
    auto listed = ListTools();
    if (!listed.contains("tools") || !listed["tools"].is_array()) return 0;
    std::size_t n = 0;
    // shared_ptr to this is unsafe; capture raw and rely on host outliving registry handlers
    // Callers must keep JsHost alive as long as tools are used.
    for (const auto& t : listed["tools"]) {
        const std::string name = t.value("name", "");
        if (name.empty()) continue;
        tools::ToolSpec spec;
        spec.name = name;
        spec.description = t.value("description", "JS plugin tool");
        spec.parameters = t.value("parameters", nlohmann::json{{"type", "object"}});
        spec.handler = [this, name](const nlohmann::json& args) -> std::string {
            auto r = CallTool(name, args);
            if (r.contains("content") && r["content"].is_string()) return r["content"].get<std::string>();
            return r.dump();
        };
        registry.Register(std::move(spec));
        ++n;
    }
    return n;
}

}  // namespace openclaw::plugins
