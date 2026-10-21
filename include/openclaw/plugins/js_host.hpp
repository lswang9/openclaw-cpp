#pragma once

#include "openclaw/mcp/stdio.hpp"
#include "openclaw/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <functional>
#include <atomic>
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

    // Upstream plugin API surface (via host.mjs v2).
    void SetConfig(nlohmann::json cfg) { std::lock_guard l(state_mu_); config_ = std::move(cfg); }
    void SetExtraDirs(std::vector<std::filesystem::path> dirs) { std::lock_guard l(state_mu_); extra_dirs_ = std::move(dirs); }
    nlohmann::json ListCommands();
    bool HasCommand(const std::string& name) const;
    bool HasHook(const std::string& event) const;
    bool HasGatewayMethod(const std::string& method) const;
    // {text, raw} or {error}
    nlohmann::json RunCommand(const std::string& name, const std::string& args, const nlohmann::json& ctx);
    // {handlers, results, merged} — merged = payload with handler results shallow-merged in order
    nlohmann::json EmitHook(const std::string& event, const nlohmann::json& payload, const nlohmann::json& ctx = {});
    nlohmann::json CallGatewayMethod(const std::string& method, const nlohmann::json& params);
    // Raw RPC (http_request, run_cli, channel_*, provider_complete, list_*).
    nlohmann::json Rpc(const std::string& method, const nlohmann::json& params) { return Request(method, params); }
    // Matching registered HTTP route ({path,match,auth,plugin}) or null.
    nlohmann::json MatchHttpRoute(const std::string& path) const;
    std::vector<std::string> InitList(const char* key) const;
    // Plugin→host calls (`{"hostCall":id,method,params}` lines). Return a result object, or {"__deferred":true}
    // and later call SendHostReply / SendHostEvent from another thread. host_call runs while the request
    // mutex is held: must not call Request/Rpc synchronously. SendHostReply only takes the write mutex, so a
    // deferred agent.reply from a tool/command cannot deadlock.
    std::function<nlohmann::json(long long id, const std::string& method, const nlohmann::json& params)> host_call;
    // Optional stream sink for host→plugin events while a deferred call is in flight
    // (e.g. {"hostEvent":id,"type":"reply.block","text":"...","kind":"block"|"final"}).
    const std::string& InflightMethod() const { return inflight_; }
    // True while Request() holds the pipe lock — nested DeliverInbound must not EmitHook/RunCommand.
    bool RequestInFlight() const { return in_request_.load(std::memory_order_acquire); }
    void SendHostReply(long long id, const nlohmann::json& result, const std::string& error = "");
    void SendHostEvent(long long id, const std::string& type, const nlohmann::json& payload);
    nlohmann::json LastInit() const { std::lock_guard l(state_mu_); return last_init_; }

    // In-process mock (no Node) — used when FORCE_MOCK or spawn fails in tests.
    void EnableMock();

private:
    nlohmann::json Request(const std::string& method, const nlohmann::json& params);
    nlohmann::json MockHandle(const std::string& method, const nlohmann::json& params);

    mutable std::mutex mu_;       // serializes Request() (one in-flight RPC)
    mutable std::mutex write_mu_; // WriteLine only — held briefly; SendHostReply/Event use this alone
    mutable std::mutex state_mu_; // last_init_/config_/extra_dirs_ — readable during Request
    std::string inflight_;
    std::atomic<bool> in_request_{false};
    std::unique_ptr<mcp::StdioProcess> proc_;
    std::filesystem::path plugins_dir_;
    std::filesystem::path bridge_js_;
    bool mock_ = false;
    bool started_ = false;
    std::string last_error_;
    nlohmann::json last_init_;
    int next_id_ = 1;
    nlohmann::json config_ = nlohmann::json::object();
    std::vector<std::filesystem::path> extra_dirs_;
};

}  // namespace openclaw::plugins
