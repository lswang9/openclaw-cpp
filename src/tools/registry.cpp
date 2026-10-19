#include "openclaw/tools/registry.hpp"

#include <chrono>
#include <ctime>
#include <stdexcept>

namespace openclaw::tools {

void Registry::Register(ToolSpec spec) {
    tools_[spec.name] = std::move(spec);
}

bool Registry::Has(const std::string& name) const {
    return tools_.find(name) != tools_.end();
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
             {
                 {"name", spec.name},
                 {"description", spec.description},
                 {"parameters", spec.parameters},
             }},
        });
    }
    return arr;
}

std::vector<std::string> Registry::Names() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& [n, _] : tools_) {
        names.push_back(n);
    }
    return names;
}

Registry MakeDefaultRegistry() {
    Registry r;

    ToolSpec echo;
    echo.name = "echo";
    echo.description = "Echo back the provided text. Smoke / loop fixture tool.";
    echo.parameters = {
        {"type", "object"},
        {"properties", {{"text", {{"type", "string"}, {"description", "Text to echo"}}}}},
        {"required", nlohmann::json::array({"text"})},
    };
    echo.handler = [](const nlohmann::json& args) -> std::string {
        return nlohmann::json{{"echoed", args.value("text", "")}}.dump();
    };
    r.Register(std::move(echo));

    ToolSpec now;
    now.name = "time_now";
    now.description = "Return the current UTC time as ISO-8601.";
    now.parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    now.handler = [](const nlohmann::json&) -> std::string {
        using clock = std::chrono::system_clock;
        const auto t = clock::to_time_t(clock::now());
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return nlohmann::json{{"utc", buf}}.dump();
    };
    r.Register(std::move(now));

    return r;
}

}  // namespace openclaw::tools
