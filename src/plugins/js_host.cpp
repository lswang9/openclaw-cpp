#include "openclaw/plugins/js_host.hpp"

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
    // Prefer PATH via execvp("node"); empty means use "node"
    return "node";
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
        last_init_ = MockHandle("initialize", {{"plugins_dir", plugins_dir_.string()}});
        return true;
    }

    auto proc = std::make_unique<mcp::StdioProcess>();
    if (!proc->Spawn({FindNode().string(), bridge_js_.string()})) {
        last_error_ = "failed to spawn node bridge — falling back to mock";
        EnableMock();
        last_init_ = MockHandle("initialize", {{"plugins_dir", plugins_dir_.string()}});
        return true;
    }
    {
        std::lock_guard lock(mu_);
        proc_ = std::move(proc);
        mock_ = false;
        started_ = true;
        last_error_.clear();
    }
    last_init_ = Initialize();
    if (last_init_.contains("error")) {
        last_error_ = last_init_["error"].dump();
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
    std::lock_guard lock(mu_);
    return {{"running", started_},
            {"mock", mock_},
            {"plugins_dir", plugins_dir_.string()},
            {"bridge", bridge_js_.string()},
            {"force_mock", ForceMock()},
            {"last_error", last_error_},
            {"init", last_init_},
            {"impl", mock_ ? "real-sim" : "real"},
            {"gaps", nlohmann::json::array({"not full OpenClaw JS/TS plugin API",
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
    nlohmann::json req{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    try {
        proc_->WriteLine(req.dump());
        const auto line = proc_->ReadLine();
        auto resp = nlohmann::json::parse(line);
        if (resp.contains("error")) return {{"error", resp["error"]}};
        return resp.value("result", nlohmann::json::object());
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return {{"error", {{"message", e.what()}}}};
    }
}

nlohmann::json JsHost::Initialize() {
    return Request("initialize", {{"plugins_dir", plugins_dir_.string()}});
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
