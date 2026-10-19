#include "openclaw/tools/registry.hpp"
#include "openclaw/approvals/store.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <stdexcept>

namespace openclaw::tools {

void Registry::Register(ToolSpec spec) { tools_[spec.name] = std::move(spec); }

bool Registry::Has(const std::string& name) const { return tools_.find(name) != tools_.end(); }

const ToolSpec* Registry::Get(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::string Registry::Invoke(const std::string& name, const nlohmann::json& args) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return nlohmann::json{{"error", "unknown tool: " + name}}.dump();
    }
    try {
        return it->second.handler(args);
    } catch (const std::exception& e) {
        return nlohmann::json{{"error", e.what()}}.dump();
    }
}

nlohmann::json Registry::OpenAiToolsArray() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [_, spec] : tools_) {
        arr.push_back({
            {"type", "function"},
            {"function",
             {{"name", spec.name},
              {"description", spec.description},
              {"parameters", spec.parameters}}},
        });
    }
    return arr;
}

std::vector<std::string> Registry::Names() const {
    std::vector<std::string> names;
    for (const auto& [n, _] : tools_) names.push_back(n);
    return names;
}

Registry MakeDefaultRegistry(RegistryOptions opts) {
    Registry r;

    ToolSpec echo;
    echo.name = "echo";
    echo.description = "Echo back the provided text.";
    echo.parameters = {
        {"type", "object"},
        {"properties", {{"text", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"text"})},
    };
    echo.handler = [](const nlohmann::json& args) {
        return nlohmann::json{{"echoed", args.value("text", "")}}.dump();
    };
    r.Register(std::move(echo));

    ToolSpec now;
    now.name = "time_now";
    now.description = "Return current UTC time ISO-8601.";
    now.parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    now.handler = [](const nlohmann::json&) {
        using clock = std::chrono::system_clock;
        auto t = clock::to_time_t(clock::now());
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return nlohmann::json{{"utc", buf}}.dump();
    };
    r.Register(std::move(now));

    static nlohmann::json g_mem = nlohmann::json::object();
    ToolSpec mset;
    mset.name = "memory_set";
    mset.description = "Store a short memory note by key.";
    mset.parameters = {
        {"type", "object"},
        {"properties", {{"key", {{"type", "string"}}}, {"value", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"key", "value"})},
    };
    mset.handler = [](const nlohmann::json& args) {
        g_mem[args.at("key").get<std::string>()] = args.at("value").get<std::string>();
        return nlohmann::json{{"ok", true}}.dump();
    };
    r.Register(std::move(mset));

    ToolSpec mget;
    mget.name = "memory_get";
    mget.description = "Retrieve a memory note by key.";
    mget.parameters = {
        {"type", "object"},
        {"properties", {{"key", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"key"})},
    };
    mget.handler = [](const nlohmann::json& args) {
        auto key = args.at("key").get<std::string>();
        if (!g_mem.contains(key)) return nlohmann::json{{"found", false}}.dump();
        return nlohmann::json{{"found", true}, {"value", g_mem[key]}}.dump();
    };
    r.Register(std::move(mget));

    if (opts.enable_shell) {
        if (const char* env = std::getenv("OPENCLAW_SHELL_REQUIRE_APPROVAL"); env && std::string(env) == "0") {
            opts.shell_requires_approval = false;
        }
        ToolSpec shell;
        shell.name = "shell";
        shell.description =
            "Mock shell tool (records command; does not exec by default). Requires approval when gated.";
        shell.requires_approval = opts.shell_requires_approval;
        shell.parameters = {
            {"type", "object"},
            {"properties",
             {{"command", {{"type", "string"}}},
              {"approval_id", {{"type", "string"}, {"description", "Approved approval id"}}}}},
            {"required", nlohmann::json::array({"command"})},
        };
        auto* appr = opts.approvals;
        bool need = opts.shell_requires_approval;
        shell.handler = [appr, need](const nlohmann::json& args) -> std::string {
            const std::string cmd = args.value("command", "");
            if (need) {
                if (!appr) {
                    return nlohmann::json{{"error", "approvals store required for shell"},
                                          {"needs_approval", true}}.dump();
                }
                const std::string approval_id = args.value("approval_id", "");
                if (approval_id.empty()) {
                    auto id = appr->Create("exec", "shell: " + cmd);
                    return nlohmann::json{{"needs_approval", true},
                                          {"approval_id", id},
                                          {"status", "pending"},
                                          {"command", cmd}}.dump();
                }
                auto info = appr->GetJson(approval_id);
                if (info.value("status", "") != "approved") {
                    return nlohmann::json{{"error", "approval not approved"},
                                          {"approval", info}}.dump();
                }
            }
            // Mock execution only (safe default).
            return nlohmann::json{{"ok", true},
                                 {"mocked", true},
                                 {"command", cmd},
                                 {"stdout", "mock: executed (no real shell)"}}.dump();
        };
        r.Register(std::move(shell));
    }

    return r;
}

}  // namespace openclaw::tools
