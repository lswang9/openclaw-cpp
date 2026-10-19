#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::tools {

// Tool handler: arguments JSON object → result string (tool message content).
using ToolHandler = std::function<std::string(const nlohmann::json& args)>;

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters;  // JSON Schema object
    ToolHandler handler;
};

// Tools registry — core now; sibling openclaw-cpp-tools will host richer packs.
class Registry {
public:
    void Register(ToolSpec spec);
    bool Has(const std::string& name) const;
    std::string Invoke(const std::string& name, const nlohmann::json& args) const;
    nlohmann::json OpenAiToolsArray() const;  // for chat completions tools=
    std::vector<std::string> Names() const;

private:
    std::unordered_map<std::string, ToolSpec> tools_;
};

Registry MakeDefaultRegistry();  // registers built-in echo

}  // namespace openclaw::tools
