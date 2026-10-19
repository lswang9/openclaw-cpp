#include "openclaw/mcp/client.hpp"

#include <stdexcept>

namespace openclaw::mcp {

Client::Client(WriteLineFn write, ReadLineFn read)
    : write_(std::move(write)), read_(std::move(read)) {}

nlohmann::json Client::Request(const std::string& method, const nlohmann::json& params) {
    nlohmann::json req{{"jsonrpc", "2.0"}, {"id", next_id_++}, {"method", method}};
    if (!params.is_null()) {
        req["params"] = params;
    }
    write_(req.dump());
    const std::string line = read_();
    auto resp = nlohmann::json::parse(line);
    if (resp.contains("error")) {
        throw std::runtime_error("mcp error: " + resp["error"].dump());
    }
    return resp.value("result", nlohmann::json::object());
}

bool Client::Initialize() {
    auto result = Request("initialize", {
                                            {"protocolVersion", "2024-11-05"},
                                            {"capabilities", nlohmann::json::object()},
                                            {"clientInfo", {{"name", "openclaw-cpp"}, {"version", "0.4.0-dev"}}},
                                        });
    return result.value("protocolVersion", "").size() > 0 || result.contains("serverInfo");
}

std::vector<ToolInfo> Client::ListTools() {
    auto result = Request("tools/list");
    std::vector<ToolInfo> out;
    if (result.contains("tools") && result["tools"].is_array()) {
        for (const auto& t : result["tools"]) {
            out.push_back({t.value("name", ""), t.value("description", "")});
        }
    }
    return out;
}

nlohmann::json Client::CallTool(const std::string& name, const nlohmann::json& arguments) {
    return Request("tools/call", {{"name", name}, {"arguments", arguments}});
}

nlohmann::json Client::MockServerHandle(const nlohmann::json& req) {
    const std::string method = req.value("method", "");
    const auto id = req.contains("id") ? req["id"] : nlohmann::json();
    auto ok = [&](nlohmann::json result) {
        return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    };
    if (method == "initialize") {
        return ok({{"protocolVersion", "2024-11-05"},
                   {"capabilities", {{"tools", nlohmann::json::object()}}},
                   {"serverInfo", {{"name", "openclaw-cpp-mock-mcp"}, {"version", "0.1"}}}});
    }
    if (method == "tools/list") {
        return ok({{"tools", nlohmann::json::array({
                                  {{"name", "mock_echo"},
                                   {"description", "Echo arguments back"},
                                   {"inputSchema", {{"type", "object"}}}},
                              })}});
    }
    if (method == "tools/call") {
        const auto params = req.value("params", nlohmann::json::object());
        return ok({{"content", nlohmann::json::array({
                                    {{"type", "text"},
                                     {"text", nlohmann::json{{"echo", params}}.dump()}},
                                })}});
    }
    return nlohmann::json{{"jsonrpc", "2.0"},
                          {"id", id},
                          {"error", {{"code", -32601}, {"message", "method not found"}}}};
}

}  // namespace openclaw::mcp
