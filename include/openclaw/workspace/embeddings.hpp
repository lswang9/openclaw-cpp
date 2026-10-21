#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::workspace {

using Vec = std::vector<float>;

class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    virtual std::string Id() const = 0;       // local | openai | ...
    virtual std::string Model() const = 0;
    // Batch embed; throws std::runtime_error on failure.
    virtual std::vector<Vec> Embed(const std::vector<std::string>& texts) = 0;
};

// Offline hashed-feature embeddings (LocalEmbed).
std::shared_ptr<EmbeddingProvider> MakeLocalEmbedder(int dims = 512);
// OpenAI-compatible POST {baseUrl}/embeddings {model, input:[...]} (Bearer apiKey). baseUrl like https://api.openai.com/v1
std::shared_ptr<EmbeddingProvider> MakeOpenAiEmbedder(std::string base_url, std::string api_key, std::string model);
// From agents.defaults.memorySearch: provider local|openai|auto (+model, remote.baseUrl, remote.apiKey; apiKey
// falls back to OPENAI_API_KEY). auto = openai when a key is available, else local. Disabled → nullptr.
std::shared_ptr<EmbeddingProvider> MakeEmbedderFromConfig(const nlohmann::json& raw);

// Persistent vector index + embedding cache (JSON file). Entries keyed by sha256(text) per provider/model,
// so unchanged chunks are never re-embedded across searches or restarts.
class VectorIndex {
public:
    explicit VectorIndex(std::filesystem::path file);
    // Returns vectors for texts, embedding only cache misses; persists when anything new was added.
    std::vector<Vec> Get(EmbeddingProvider& prov, const std::vector<std::string>& texts);
    nlohmann::json Stats() const;
    std::size_t last_misses = 0, last_hits = 0;

private:
    void LoadLocked();
    void SaveLocked() const;
    std::filesystem::path file_;
    mutable std::mutex mu_;
    bool loaded_ = false;
    std::string key_;  // provider:model of loaded cache
    std::unordered_map<std::string, Vec> cache_;
};

std::string Sha256Hex(const std::string& s);

}  // namespace openclaw::workspace
