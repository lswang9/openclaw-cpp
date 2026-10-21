#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace openclaw::workspace {

// Agent workspace (OpenClaw-style): files the agent may read/write/edit, plus Markdown memory
// (MEMORY.md + memory/**/*.md) searchable with BM25 over line-ranged chunks.
// Root: OPENCLAW_WORKSPACE, else $OPENCLAW_SESSION_DIR/workspace, else /tmp/openclaw-cpp-sessions/workspace.
// Local, offline embedding: signed feature hashing of word tokens + char trigrams (+CJK unigrams), L2-normalised.
// Not a neural model; gives fuzzy/morphological recall without network or credentials.
std::vector<float> LocalEmbed(const std::string& text, int dims = 512);
double Cosine(const std::vector<float>& a, const std::vector<float>& b);

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
    // mode: hybrid (default; 0.7 vector + 0.3 BM25) | bm25 | vector
    nlohmann::json MemorySearch(const std::string& query, int max_results = 6, double min_score = 0.0,
                                const std::string& mode = "hybrid") const;
    nlohmann::json MemoryGet(const std::string& path, int from = 1, int lines = 0) const;
    // Append "- key: value" under today's memory/YYYY-MM-DD.md (UTC); returns path written.
    nlohmann::json MemoryAppend(const std::string& text) const;
    bool IsMemoryPath(const std::string& rel) const;

    // Optional embedding provider + persisted index (default: local hashed embeddings, no index).
    void SetEmbedder(std::shared_ptr<class EmbeddingProvider> p, std::filesystem::path index_file);
    nlohmann::json IndexStats() const;

private:
    std::filesystem::path root_;
    std::shared_ptr<class EmbeddingProvider> embedder_;
    std::shared_ptr<class VectorIndex> index_;
};

}  // namespace openclaw::workspace
