#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace openclaw::mcp {

struct ToolInfo {
    std::string name;
    std::string description;
};

// Transport: write a line, read a line (newline-delimited JSON-RPC).
using WriteLineFn = std::function<void(const std::string&)>;
using ReadLineFn = std::function<std::string()>;

class Client {
public:
    Client(WriteLineFn write, ReadLineFn read);

    nlohmann::json Request(const std::string& method, const nlohmann::json& params = {});
    bool Initialize();
    std::vector<ToolInfo> ListTools();
    nlohmann::json CallTool(const std::string& name, const nlohmann::json& arguments);
    nlohmann::json ListResources();
    nlohmann::json ListResourceTemplates();
    nlohmann::json ReadResource(const std::string& uri);

    // In-process mock server loop for self-test (handles one request from a queue).
    static nlohmann::json MockServerHandle(const nlohmann::json& req);

private:
    WriteLineFn write_;
    ReadLineFn read_;
    int next_id_ = 1;
};

}  // namespace openclaw::mcp
