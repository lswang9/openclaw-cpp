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
    // Query-side embedding (providers with task types: Gemini RETRIEVAL_QUERY, Voyage input_type=query).
    virtual Vec EmbedQuery(const std::string& text) { return Embed({text}).at(0); }
    // Backend counters (llama.cpp: n_ctx, n_seq, decodes, windows); null when not applicable.
    virtual nlohmann::json Stats() const { return nullptr; }
};

// Offline hashed-feature embeddings (LocalEmbed).
std::shared_ptr<EmbeddingProvider> MakeLocalEmbedder(int dims = 512);
// OpenAI-compatible POST {baseUrl}/embeddings {model, input:[...]} (Bearer apiKey). baseUrl like https://api.openai.com/v1
std::shared_ptr<EmbeddingProvider> MakeOpenAiEmbedder(std::string base_url, std::string api_key, std::string model);
// Gemini: POST {baseUrl}/models/{model}:batchEmbedContents (x-goog-api-key), taskType RETRIEVAL_DOCUMENT/QUERY,
// optional outputDimensionality. baseUrl default https://generativelanguage.googleapis.com/v1beta, model gemini-embedding-001.
std::shared_ptr<EmbeddingProvider> MakeGeminiEmbedder(std::string base_url, std::string api_key, std::string model, int dims = 0);
// Ollama: POST {baseUrl}/api/embeddings {model, prompt} (one text per call; we batch sequentially).
// Default baseUrl http://127.0.0.1:11434, model nomic-embed-text.
std::shared_ptr<EmbeddingProvider> MakeOllamaEmbedder(std::string base_url, std::string model);
// Local GGUF: validates the GGUF magic/header and uses the offline hashed embedder keyed by the model
// file stem (no llama.cpp link — neural inference is real-sim). dims from memorySearch.dimensions.
std::shared_ptr<EmbeddingProvider> MakeGgufEmbedder(std::filesystem::path model_path, int dims = 512);
// Voyage: POST {baseUrl}/embeddings {input, model, input_type: document|query} (Bearer). Default https://api.voyageai.com/v1, voyage-3.5.
std::shared_ptr<EmbeddingProvider> MakeVoyageEmbedder(std::string base_url, std::string api_key, std::string model);
// From agents.defaults.memorySearch: provider local|openai|gemini|voyage|auto (+model, remote.baseUrl, remote.apiKey; apiKey
// falls back to OPENAI_API_KEY). auto = openai when a key is available, else local. Disabled → nullptr.
std::shared_ptr<EmbeddingProvider> MakeEmbedderFromConfig(const nlohmann::json& raw);

// Persistent vector index + embedding cache, keyed by sha256(text) per provider/model so unchanged chunks are
// never re-embedded. Backend by file extension:
//  *.sqlite → SQLite store (sqlite-vec-equivalent: float32 BLOB vectors, `vec_distance_cosine()` SQL function,
//             multiple provider/models side by side, stale-row pruning, legacy *.index.json auto-import);
//  else     → legacy JSON file.
class VectorIndex {
public:
    explicit VectorIndex(std::filesystem::path file);
    ~VectorIndex();
    VectorIndex(const VectorIndex&) = delete;
    VectorIndex& operator=(const VectorIndex&) = delete;
    // Returns vectors for texts, embedding only cache misses; persists when anything new was added.
    // prune=true drops rows of this provider/model whose hash is not in `texts` (texts = full chunk set).
    std::vector<Vec> Get(EmbeddingProvider& prov, const std::vector<std::string>& texts, bool prune = false);
    // Brute-force KNN in SQL (ORDER BY vec_distance_cosine) — SQLite backend only.
    std::vector<std::pair<std::string, double>> Knn(const std::string& provider, const std::string& model, const Vec& q, int k);
    // FTS5 full-text index of chunk texts (same sqlite file). No-op on JSON backend / if FTS5 unavailable.
    void UpsertFts(const std::vector<std::string>& texts);
    // Returns {hash, -bm25} best-first (higher is better). Empty if FTS unavailable.
    std::vector<std::pair<std::string, double>> SearchFts(const std::string& query, int k = 50);
    bool FtsEnabled() const;
    nlohmann::json Stats() const;
    std::size_t last_misses = 0, last_hits = 0, last_pruned = 0;

private:
    void LoadLocked();
    void SaveLocked() const;
    bool OpenSqliteLocked();
    std::filesystem::path file_;
    mutable std::mutex mu_;
    bool loaded_ = false;
    bool sqlite_ = false;
    bool fts_ = false;
    void* db_ = nullptr;  // sqlite3*
    std::string key_;  // provider:model of loaded cache (JSON backend)
    std::unordered_map<std::string, Vec> cache_;
};

std::string Sha256Hex(const std::string& s);

}  // namespace openclaw::workspace
