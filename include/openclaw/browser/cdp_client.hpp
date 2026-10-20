#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <filesystem>
#include <optional>
#include <string>

namespace openclaw::browser {

// Minimal Chrome DevTools Protocol client over WebSocket.
// Connects to an already-running Chromium/Chrome with --remote-debugging-port.
// Env: OPENCLAW_BROWSER_CDP_URL = http://127.0.0.1:9222  OR  ws://host:port/devtools/...
//      OPENCLAW_BROWSER_CDP_MOCK = 1  → in-process fake (self-test, no Chrome).

struct CdpConfig {
    std::string endpoint;  // empty → read env / default http://127.0.0.1:9222
    std::chrono::milliseconds timeout{15000};
    bool mock = false;
};

class CdpClient {
public:
    explicit CdpClient(CdpConfig cfg = {});
    ~CdpClient();

    CdpClient(const CdpClient&) = delete;
    CdpClient& operator=(const CdpClient&) = delete;

    // Discover + open page target WebSocket. ok=false when CDP down (caller may curl-fallback).
    nlohmann::json Connect();
    void Disconnect();
    bool Connected() const;

    nlohmann::json Status() const;

    // High-level ops (auto Connect if needed).
    nlohmann::json Navigate(const std::string& url);
    nlohmann::json Evaluate(const std::string& expression);
    nlohmann::json ScreenshotPng(const std::filesystem::path& out_path = {});
    nlohmann::json TitleAndUrl();

    // Low-level CDP command (method + params) → full result object or error.
    nlohmann::json Call(const std::string& method, const nlohmann::json& params = nlohmann::json::object());

    static CdpConfig FromEnv();
    static bool MockEnabled();
    static std::string ResolveHttpBase(const std::string& endpoint);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Framing helpers (also used by self-test).
std::string WsClientMaskFrame(const std::string& payload);
bool WsParseServerFrame(const std::string& buf, size_t& consumed, std::string& out_payload, uint8_t& opcode);

}  // namespace openclaw::browser
