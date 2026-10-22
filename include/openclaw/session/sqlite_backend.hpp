#pragma once
// 1.131: SQLite session storage in the upstream agent database layout (openclaw-agent.sqlite, schema v25):
// session_nodes (index entry), session_windows (one per sessionId generation) and transcript_events (JSONL lines).
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct sqlite3;

namespace openclaw::session {
class SqliteBackend {
public:
    // Opens/creates <root>/openclaw-agent.sqlite and applies the upstream schema. Throws on failure.
    SqliteBackend(const std::filesystem::path& root, const std::string& agent_id);
    ~SqliteBackend();
    SqliteBackend(const SqliteBackend&) = delete;
    SqliteBackend& operator=(const SqliteBackend&) = delete;

    std::filesystem::path Path() const { return path_; }
    std::vector<std::string> Keys();
    bool Exists(const std::string& key);
    nlohmann::json Entry(const std::string& key);  // null when absent
    nlohmann::json AllEntries();
    // Transcript lines (parsed JSON) of the key's current window, ordered by seq.
    std::vector<nlohmann::json> Lines(const std::string& key);
    // Replace the current window's lines and upsert the entry.
    void Replace(const std::string& key, const nlohmann::json& entry, const std::vector<nlohmann::json>& lines);
    void AppendLine(const std::string& key, const nlohmann::json& entry, const nlohmann::json& line);
    void PutEntry(const std::string& key, const nlohmann::json& entry);
    // Start a new window (previous one is kept as history). reason: upstream window reason.
    void NewWindow(const std::string& key, const nlohmann::json& entry, const std::string& prev_sid, const std::string& reason);
    bool Remove(const std::string& key);
    bool Empty();
    nlohmann::json Stats();

private:
    void Exec(const std::string& sql);
    void EnsureNodeAndWindow(const std::string& key, const nlohmann::json& entry);
    std::filesystem::path path_;
    std::string agent_id_;
    sqlite3* db_ = nullptr;
    std::recursive_mutex mu_;
};

// "json" (default) | "sqlite"; OPENCLAW_SESSION_STORE overrides; set from config session.store at startup.
void SetSessionBackend(const std::string& kind);
std::string SessionBackend();
}  // namespace openclaw::session
