#pragma once

#include "openclaw/mcp/stdio.hpp"
#include "openclaw/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openclaw::plugins {

// Thin JS plugin host: Node stdio JSON-RPC bridge (plugins/js_bridge/host.mjs).
// Not the full OpenClaw Node plugin API. FORCE_MOCK uses in-process stubs.
class JsHost {
public:
    JsHost();
    ~JsHost();

    JsHost(const JsHost&) = delete;
    JsHost& operator=(const JsHost&) = delete;

    // plugins_dir: repo plugins/; bridge_js: path to host.mjs (optional auto-detect)
    bool Start(const std::filesystem::path& plugins_dir,
               const std::filesystem::path& bridge_js = {});
    void Stop();

    bool Running() const;
    nlohmann::json Status() const;

    nlohmann::json Initialize();
    nlohmann::json ListTools();
    nlohmann::json CallTool(const std::string& name, const nlohmann::json& arguments);
    nlohmann::json ListPlugins();

    // Register js.* tools into a tools::Registry (handlers call CallTool).
    std::size_t RegisterTools(tools::Registry& registry);

    // In-process mock (no Node) — used when FORCE_MOCK or spawn fails in tests.
    void EnableMock();

private:
    nlohmann::json Request(const std::string& method, const nlohmann::json& params);
    nlohmann::json MockHandle(const std::string& method, const nlohmann::json& params);

    mutable std::mutex mu_;
    std::unique_ptr<mcp::StdioProcess> proc_;
    std::filesystem::path plugins_dir_;
    std::filesystem::path bridge_js_;
    bool mock_ = false;
    bool started_ = false;
    std::string last_error_;
    nlohmann::json last_init_;
    int next_id_ = 1;
};

}  // namespace openclaw::plugins
