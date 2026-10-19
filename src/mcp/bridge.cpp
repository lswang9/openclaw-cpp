#include "openclaw/mcp/bridge.hpp"

namespace openclaw::mcp {

std::shared_ptr<Client> MakeMockClient() {
    auto inbox = std::make_shared<std::string>();
    auto write = [inbox](const std::string& line) {
        *inbox = Client::MockServerHandle(nlohmann::json::parse(line)).dump();
    };
    auto read = [inbox]() { return *inbox; };
    return std::make_shared<Client>(write, read);
}

std::size_t RegisterMcpTools(tools::Registry& registry, std::shared_ptr<Client> client,
                             const std::string& prefix) {
    if (!client) return 0;
    client->Initialize();
    auto tools = client->ListTools();
    std::size_t n = 0;
    for (const auto& t : tools) {
        tools::ToolSpec spec;
        spec.name = prefix + t.name;
        spec.description = "[mcp] " + (t.description.empty() ? t.name : t.description);
        spec.parameters = {{"type", "object"}, {"additionalProperties", true}};
        auto cli = client;
        auto remote_name = t.name;
        spec.handler = [cli, remote_name](const nlohmann::json& args) -> std::string {
            try {
                auto result = cli->CallTool(remote_name, args);
                return result.dump();
            } catch (const std::exception& e) {
                return nlohmann::json{{"error", e.what()}}.dump();
            }
        };
        registry.Register(std::move(spec));
        ++n;
    }
    return n;
}

}  // namespace openclaw::mcp
