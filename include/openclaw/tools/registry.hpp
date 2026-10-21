#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::approvals {
class Store;
}

namespace openclaw::tools {

using ToolHandler = std::function<std::string(const nlohmann::json& args)>;

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters;
    ToolHandler handler;
    bool requires_approval = false;
};

class Registry {
public:
    void Register(ToolSpec spec);
    bool Has(const std::string& name) const;
    std::string Invoke(const std::string& name, const nlohmann::json& args) const;
    nlohmann::json OpenAiToolsArray() const;
    std::vector<std::string> Names() const;
    const ToolSpec* Get(const std::string& name) const;
    // Copy all tool specs (handlers) from another registry; skips duplicate names.
    std::size_t MergeFrom(const Registry& other);

private:
    std::unordered_map<std::string, ToolSpec> tools_;
};

struct RegistryOptions {
    approvals::Store* approvals = nullptr;
    bool enable_shell = true;
    bool shell_requires_approval = true;  // OPENCLAW_SHELL_REQUIRE_APPROVAL default true
    std::string workspace_root;           // empty = workspace::Workspace::DefaultRoot()
};

Registry MakeDefaultRegistry(RegistryOptions opts = {});

}  // namespace openclaw::tools
