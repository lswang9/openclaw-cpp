#pragma once
#include <cstdint>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
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

// Upstream-shaped file JSON (flat; raw merged with typed fields). Never nests "raw".
nlohmann::json ToFileJson(const Snapshot& snap);

// JSON5 subset -> JSON: // and /* */ comments, trailing commas, unquoted keys, single-quoted strings,
// +numbers, hex ints. Throws std::runtime_error with line info on failure.
nlohmann::json ParseJson5(const std::string& text);
// Replace ${VAR} in all string values (upstream env substitution); missing vars throw. $${VAR} escapes.
nlohmann::json SubstituteEnv(const nlohmann::json& j);

// Dot/bracket paths: "agents.defaults.model", "channels.list[0].id".
std::optional<nlohmann::json> GetPath(const nlohmann::json& root, const std::string& path);
bool SetPath(nlohmann::json& root, const std::string& path, const nlohmann::json& value);
bool UnsetPath(nlohmann::json& root, const std::string& path);

struct LoadResult {
    Snapshot snap;
    bool exists = false;
    std::string error;  // parse / env error (snap stays default)
};
LoadResult Load(const std::filesystem::path& path);
Snapshot LoadOrDefault(const std::filesystem::path& path);
// Write raw file JSON atomically (tmp + rename), keeping a .bak of the previous file.
void SaveRaw(const std::filesystem::path& path, const nlohmann::json& file_json);
void Save(const std::filesystem::path& path, const Snapshot& snap);
// OPENCLAW_CONFIG_PATH > ~/.openclaw/openclaw.json > legacy ~/.openclaw-cpp/config.json (if only it exists).
std::filesystem::path DefaultPath();

}  // namespace openclaw::config
