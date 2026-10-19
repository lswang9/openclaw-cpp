#pragma once
#include <cstdint>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace openclaw::config {

// Minimal config snapshot toward OpenClaw gateway/agents config.
struct Snapshot {
    std::string align_target = "OpenClaw 2026.10.5";
    std::string gateway_host = "127.0.0.1";
    std::uint16_t gateway_port = 18789;
    std::string deepseek_model = "deepseek-chat";
    std::string session_dir = "/tmp/openclaw-cpp-sessions";
    nlohmann::json raw = nlohmann::json::object();

    nlohmann::json ToJson() const {
        return nlohmann::json{
            {"align_target", align_target},
            {"gateway", {{"host", gateway_host}, {"port", gateway_port}}},
            {"agents", {{"defaults", {{"model", deepseek_model}}}}},
            {"session_dir", session_dir},
            {"raw", raw},
        };
    }
};

Snapshot LoadOrDefault(const std::filesystem::path& path);
void Save(const std::filesystem::path& path, const Snapshot& snap);
std::filesystem::path DefaultPath();

}  // namespace openclaw::config
