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
    nlohmann::json raw = nlohmann::json::object();       // effective doc ($include merged, env unsubstituted)
    nlohmann::json include_spec;                          // original "$include" value (null if none)
    nlohmann::json included = nlohmann::json::object();   // merged content of included files

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
// SecretRef {source: env|file|exec, provider?, id} resolution against secrets.providers (upstream secrets contract).
bool IsSecretRef(const nlohmann::json& j);
std::string ResolveSecretRef(const nlohmann::json& ref, const nlohmann::json& raw, std::string* err);
// Replace every SecretRef in `j` (using providers from `raw`); throws on an unresolvable ref.
nlohmann::json ResolveSecretRefs(const nlohmann::json& j, const nlohmann::json& raw, const std::string& path = "");

// Dot/bracket paths: "agents.defaults.model", "channels.list[0].id".
std::optional<nlohmann::json> GetPath(const nlohmann::json& root, const std::string& path);
bool SetPath(nlohmann::json& root, const std::string& path, const nlohmann::json& value);
bool UnsetPath(nlohmann::json& root, const std::string& path);

// Resolve "$include" (string or array of paths, relative to the including file; JSON5; nested, max depth 10,
// cycles rejected). Included docs are deep-merged in order, then the including doc's own keys override.
nlohmann::json ResolveIncludes(const nlohmann::json& doc, const std::filesystem::path& base_dir,
                               nlohmann::json* include_spec = nullptr, nlohmann::json* included = nullptr, int depth = 0,
                               std::vector<std::string>* stack = nullptr);
// File document to write back: keeps "$include" and only the top-level keys that differ from included content.
nlohmann::json ToFileDoc(const Snapshot& snap, const nlohmann::json& effective);

struct SchemaReport {
    std::vector<std::string> errors;    // invalid types/values on known keys → config rejected
    std::vector<std::string> warnings;  // unknown keys
};
SchemaReport ValidateSchema(const nlohmann::json& raw);

struct LoadResult {
    Snapshot snap;
    bool exists = false;
    std::string error;  // parse / env / include / schema error (snap stays default)
    std::vector<std::string> warnings;
};
LoadResult Load(const std::filesystem::path& path);
// agents.entries <-> agents.list bridging (see config.cpp).
void ExpandAgentEntries(nlohmann::json& raw);
nlohmann::json FoldAgentEntries(nlohmann::json doc);
Snapshot LoadOrDefault(const std::filesystem::path& path);
// Write raw file JSON atomically (tmp + rename), keeping a .bak of the previous file.
void SaveRaw(const std::filesystem::path& path, const nlohmann::json& file_json);
void Save(const std::filesystem::path& path, const Snapshot& snap);
// Write `effective` while keeping on-disk `$include` (via ToFileDoc) and, when possible, JSON5 comments /
// formatting by overlaying changed top-level values into the original text. Falls back to SaveRaw.
// Returns {ok, path, preservedInclude, preservedComments, warnings[]}.
nlohmann::json SavePreserving(const std::filesystem::path& path, const nlohmann::json& effective);
// OPENCLAW_CONFIG_PATH > ~/.openclaw/openclaw.json > legacy ~/.openclaw-cpp/config.json (if only it exists).
std::filesystem::path DefaultPath();

}  // namespace openclaw::config
