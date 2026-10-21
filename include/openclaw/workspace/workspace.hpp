#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <string>

namespace openclaw::workspace {

// Agent workspace (OpenClaw-style): files the agent may read/write/edit, plus Markdown memory
// (MEMORY.md + memory/**/*.md) searchable with BM25 over line-ranged chunks.
// Root: OPENCLAW_WORKSPACE, else $OPENCLAW_SESSION_DIR/workspace, else /tmp/openclaw-cpp-sessions/workspace.
class Workspace {
public:
    explicit Workspace(std::filesystem::path root);
    static std::filesystem::path DefaultRoot();
    const std::filesystem::path& Root() const { return root_; }

    // Resolve a relative (or root-prefixed absolute) path; nullopt if it escapes the root (.., symlinks).
    std::optional<std::filesystem::path> Resolve(const std::string& rel) const;

    nlohmann::json Read(const std::string& path, int offset = 1, int limit = 2000) const;
    nlohmann::json Write(const std::string& path, const std::string& content) const;
    nlohmann::json Edit(const std::string& path, const std::string& old_s, const std::string& new_s,
                        bool replace_all = false) const;

    // Memory
    nlohmann::json MemorySearch(const std::string& query, int max_results = 6, double min_score = 0.0) const;
    nlohmann::json MemoryGet(const std::string& path, int from = 1, int lines = 0) const;
    // Append "- key: value" under today's memory/YYYY-MM-DD.md (UTC); returns path written.
    nlohmann::json MemoryAppend(const std::string& text) const;
    bool IsMemoryPath(const std::string& rel) const;

private:
    std::filesystem::path root_;
};

}  // namespace openclaw::workspace
