#include <sqlite3.h>
#include <cstring>
#include "openclaw/workspace/embeddings.hpp"
#include "openclaw/workspace/workspace.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace openclaw::workspace {
namespace fs = std::filesystem;

std::string Sha256Hex(const std::string& s) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_Digest(s.data(), s.size(), md, &n, EVP_sha256(), nullptr);
    static const char* hex = "0123456789abcdef";
    std::string o;
    for (unsigned i = 0; i < n; ++i) { o += hex[md[i] >> 4]; o += hex[md[i] & 15]; }
    return o;
}

namespace {
class LocalEmbedder : public EmbeddingProvider {
public:
    explicit LocalEmbedder(int d) : d_(d) {}
    std::string Id() const override { return "local"; }
    std::string Model() const override { return "hash-" + std::to_string(d_); }
    std::vector<Vec> Embed(const std::vector<std::string>& t) override {
        std::vector<Vec> o;
        for (auto& s : t) o.push_back(LocalEmbed(s, d_));
        return o;
    }
private:
    int d_;
};

std::size_t Sink(char* p, std::size_t sz, std::size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * n);
    return sz * n;
}

class OpenAiEmbedder : public EmbeddingProvider {
public:
    OpenAiEmbedder(std::string b, std::string k, std::string m) : base_(std::move(b)), key_(std::move(k)), model_(std::move(m)) {
        while (!base_.empty() && base_.back() == '/') base_.pop_back();
    }
    std::string Id() const override { return "openai"; }
    std::string Model() const override { return model_; }
    std::vector<Vec> Embed(const std::vector<std::string>& texts) override {
        std::vector<Vec> out;
        for (std::size_t off = 0; off < texts.size(); off += 64) {  // batch
            nlohmann::json req{{"model", model_}, {"input", nlohmann::json::array()}};
            for (std::size_t i = off; i < texts.size() && i < off + 64; ++i) req["input"].push_back(texts[i]);
            std::string body = req.dump(), resp;
            CURL* c = curl_easy_init();
            if (!c) throw std::runtime_error("curl init failed");
            curl_slist* h = nullptr;
            h = curl_slist_append(h, "Content-Type: application/json");
            if (!key_.empty()) h = curl_slist_append(h, ("Authorization: Bearer " + key_).c_str());
            const std::string url = base_ + "/embeddings";
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Sink);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
            auto rc = curl_easy_perform(c);
            long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            curl_slist_free_all(h); curl_easy_cleanup(c);
            if (rc != CURLE_OK) throw std::runtime_error(std::string("embeddings request failed: ") + curl_easy_strerror(rc));
            if (code != 200) throw std::runtime_error("embeddings HTTP " + std::to_string(code));
            auto j = nlohmann::json::parse(resp);
            auto data = j.at("data");
            std::vector<Vec> batch(data.size());
            for (const auto& d : data) {
                std::size_t idx = d.value("index", std::size_t{0});
                if (idx >= batch.size()) throw std::runtime_error("bad embedding index");
                Vec v = d.at("embedding").get<Vec>();
                double n = 0; for (float x : v) n += double(x) * x;
                if (n > 0) { float inv = static_cast<float>(1.0 / std::sqrt(n)); for (auto& x : v) x *= inv; }
                batch[idx] = std::move(v);
            }
            for (auto& v : batch) out.push_back(std::move(v));
        }
        if (out.size() != texts.size()) throw std::runtime_error("embedding count mismatch");
        return out;
    }
private:
    std::string base_, key_, model_;
};
}  // namespace

std::shared_ptr<EmbeddingProvider> MakeLocalEmbedder(int dims) { return std::make_shared<LocalEmbedder>(dims); }
std::shared_ptr<EmbeddingProvider> MakeOpenAiEmbedder(std::string b, std::string k, std::string m) {
    return std::make_shared<OpenAiEmbedder>(std::move(b), std::move(k), std::move(m));
}

std::shared_ptr<EmbeddingProvider> MakeEmbedderFromConfig(const nlohmann::json& raw) {
    nlohmann::json ms = nlohmann::json::object();
    try { ms = raw.at("agents").at("defaults").at("memorySearch"); } catch (...) {}
    if (!ms.is_object()) ms = nlohmann::json::object();
    if (!ms.value("enabled", true)) return nullptr;
    std::string prov = ms.value("provider", std::string("local"));
    auto remote = ms.value("remote", nlohmann::json::object());
    std::string key = remote.value("apiKey", std::string());
    if (key.empty()) if (const char* e = std::getenv("OPENAI_API_KEY")) key = e;
    std::string base = remote.value("baseUrl", std::string("https://api.openai.com/v1"));
    if (prov == "auto") prov = key.empty() ? "local" : "openai";
    if (prov == "openai") return MakeOpenAiEmbedder(base, key, ms.value("model", std::string("text-embedding-3-small")));
    return MakeLocalEmbedder();
}

namespace {
std::string VecBlob(const Vec& v) { return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float)); }
Vec BlobVec(const void* p, int bytes) {
    Vec v(static_cast<std::size_t>(bytes) / sizeof(float));
    if (!v.empty()) std::memcpy(v.data(), p, v.size() * sizeof(float));
    return v;
}
void CosineDistanceFn(sqlite3_context* ctx, int, sqlite3_value** argv) {
    int na = sqlite3_value_bytes(argv[0]), nb = sqlite3_value_bytes(argv[1]);
    auto* a = static_cast<const float*>(sqlite3_value_blob(argv[0]));
    auto* b = static_cast<const float*>(sqlite3_value_blob(argv[1]));
    if (!a || !b || na != nb || na == 0) { sqlite3_result_null(ctx); return; }
    double dot = 0, x = 0, y = 0;
    for (int i = 0; i < na / 4; ++i) { dot += double(a[i]) * b[i]; x += double(a[i]) * a[i]; y += double(b[i]) * b[i]; }
    sqlite3_result_double(ctx, (x == 0 || y == 0) ? 1.0 : 1.0 - dot / (std::sqrt(x) * std::sqrt(y)));
}
}  // namespace

VectorIndex::VectorIndex(fs::path file) : file_(std::move(file)) { sqlite_ = file_.extension() == ".sqlite"; }
VectorIndex::~VectorIndex() { if (db_) sqlite3_close(static_cast<sqlite3*>(db_)); }

bool VectorIndex::OpenSqliteLocked() {
    if (db_) return true;
    std::error_code ec;
    if (file_.has_parent_path()) fs::create_directories(file_.parent_path(), ec);
    sqlite3* db = nullptr;
    if (sqlite3_open(file_.c_str(), &db) != SQLITE_OK) { if (db) sqlite3_close(db); throw std::runtime_error("cannot open vector store " + file_.string()); }
    db_ = db;
    sqlite3_busy_timeout(db, 5000);
    sqlite3_create_function(db, "vec_distance_cosine", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, CosineDistanceFn, nullptr, nullptr);
    sqlite3_exec(db,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS embedding_cache(provider TEXT NOT NULL, model TEXT NOT NULL, hash TEXT NOT NULL,"
        " dims INTEGER NOT NULL, embedding BLOB NOT NULL, updated_at INTEGER NOT NULL, PRIMARY KEY(provider, model, hash));"
        "INSERT OR IGNORE INTO meta(key,value) VALUES('schema','openclaw-cpp-vec-1');",
        nullptr, nullptr, nullptr);
    // import legacy JSON cache living next to the store (main.index.json → main.sqlite)
    fs::path legacy = file_; legacy.replace_extension(".index.json");
    if (!fs::exists(legacy, ec)) { legacy = file_; legacy.replace_extension(".json"); }
    if (fs::exists(legacy, ec)) {
        try {
            std::ifstream in(legacy);
            auto j = nlohmann::json::parse(in);
            std::string key = j.value("key", "");
            auto colon = key.find(':');
            std::string p = key.substr(0, colon), m = colon == std::string::npos ? "" : key.substr(colon + 1);
            sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO embedding_cache VALUES(?,?,?,?,?,strftime('%s','now'))", -1, &st, nullptr);
            for (auto it = j["entries"].begin(); it != j["entries"].end(); ++it) {
                Vec v = it.value().get<Vec>();
                auto blob = VecBlob(v);
                sqlite3_bind_text(st, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, m.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, it.key().c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 4, static_cast<int>(v.size()));
                sqlite3_bind_blob(st, 5, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
                sqlite3_step(st); sqlite3_reset(st);
            }
            sqlite3_finalize(st);
            sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
            fs::rename(legacy, fs::path(legacy.string() + ".migrated"), ec);
        } catch (...) {}
    }
    return true;
}

void VectorIndex::LoadLocked() {
    loaded_ = true;
    std::error_code ec;
    if (!fs::exists(file_, ec)) return;
    try {
        std::ifstream in(file_);
        auto j = nlohmann::json::parse(in);
        key_ = j.value("key", "");
        for (auto it = j["entries"].begin(); it != j["entries"].end(); ++it) cache_[it.key()] = it.value().get<Vec>();
    } catch (...) { cache_.clear(); key_.clear(); }
}

void VectorIndex::SaveLocked() const {
    if (file_.has_parent_path()) fs::create_directories(file_.parent_path());
    nlohmann::json j{{"version", 1}, {"key", key_}, {"entries", nlohmann::json::object()}};
    for (auto& [h, v] : cache_) j["entries"][h] = v;
    auto tmp = fs::path(file_.string() + ".tmp");
    { std::ofstream out(tmp); out << j.dump(); }
    fs::rename(tmp, file_);
}

std::vector<Vec> VectorIndex::Get(EmbeddingProvider& prov, const std::vector<std::string>& texts, bool prune) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<std::string> hashes;
    for (auto& t : texts) hashes.push_back(Sha256Hex(t));
    last_pruned = 0;
    if (sqlite_) {
        OpenSqliteLocked();
        auto* db = static_cast<sqlite3*>(db_);
        const std::string p = prov.Id(), m = prov.Model();
        std::unordered_map<std::string, Vec> have;
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT hash, embedding FROM embedding_cache WHERE provider=? AND model=?", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, p.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, m.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW)
            have[reinterpret_cast<const char*>(sqlite3_column_text(st, 0))] = BlobVec(sqlite3_column_blob(st, 1), sqlite3_column_bytes(st, 1));
        sqlite3_finalize(st);
        std::vector<std::string> miss_text;
        std::vector<std::size_t> miss_idx;
        std::unordered_map<std::string, bool> queued;
        for (std::size_t i = 0; i < texts.size(); ++i)
            if (!have.count(hashes[i]) && !queued[hashes[i]]) { queued[hashes[i]] = true; miss_text.push_back(texts[i]); miss_idx.push_back(i); }
        last_misses = miss_text.size();
        last_hits = texts.size() - miss_text.size();
        sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        if (!miss_text.empty()) {
            std::vector<Vec> vecs;
            try { vecs = prov.Embed(miss_text); } catch (...) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
            sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO embedding_cache VALUES(?,?,?,?,?,strftime('%s','now'))", -1, &st, nullptr);
            for (std::size_t k = 0; k < vecs.size(); ++k) {
                auto blob = VecBlob(vecs[k]);
                sqlite3_bind_text(st, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, m.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, hashes[miss_idx[k]].c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 4, static_cast<int>(vecs[k].size()));
                sqlite3_bind_blob(st, 5, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
                sqlite3_step(st); sqlite3_reset(st);
                have[hashes[miss_idx[k]]] = std::move(vecs[k]);
            }
            sqlite3_finalize(st);
        }
        if (prune) {
            std::unordered_map<std::string, bool> keep;
            for (auto& hsh : hashes) keep[hsh] = true;
            sqlite3_prepare_v2(db, "DELETE FROM embedding_cache WHERE provider=? AND model=? AND hash=?", -1, &st, nullptr);
            for (auto& [hsh, v] : have) {
                if (keep.count(hsh)) continue;
                sqlite3_bind_text(st, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, m.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, hsh.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st); sqlite3_reset(st);
                ++last_pruned;
            }
            sqlite3_finalize(st);
        }
        sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        std::vector<Vec> out;
        for (auto& hsh : hashes) out.push_back(have[hsh]);
        return out;
    }
    if (!loaded_) LoadLocked();
    const std::string key = prov.Id() + ":" + prov.Model();
    if (key != key_) { cache_.clear(); key_ = key; }  // provider/model changed → full reindex
    std::vector<std::string> miss_text;
    std::vector<std::size_t> miss_idx;
    for (std::size_t i = 0; i < texts.size(); ++i)
        if (!cache_.count(hashes[i])) { miss_text.push_back(texts[i]); miss_idx.push_back(i); }
    last_misses = miss_text.size();
    last_hits = texts.size() - miss_text.size();
    bool dirty = false;
    if (!miss_text.empty()) {
        auto vecs = prov.Embed(miss_text);
        for (std::size_t k = 0; k < vecs.size(); ++k) cache_[hashes[miss_idx[k]]] = std::move(vecs[k]);
        dirty = true;
    }
    if (prune) {
        std::unordered_map<std::string, bool> keep;
        for (auto& hsh : hashes) keep[hsh] = true;
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (!keep.count(it->first)) { it = cache_.erase(it); ++last_pruned; dirty = true; } else ++it;
        }
    }
    if (dirty) SaveLocked();
    std::vector<Vec> out;
    for (auto& hsh : hashes) out.push_back(cache_[hsh]);
    return out;
}

std::vector<std::pair<std::string, double>> VectorIndex::Knn(const std::string& provider, const std::string& model, const Vec& q, int k) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<std::pair<std::string, double>> out;
    if (!sqlite_) return out;
    OpenSqliteLocked();
    auto* db = static_cast<sqlite3*>(db_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT hash, vec_distance_cosine(embedding, ?) AS d FROM embedding_cache WHERE provider=? AND model=? AND dims=? "
        "ORDER BY d ASC LIMIT ?", -1, &st, nullptr);
    auto blob = VecBlob(q);
    sqlite3_bind_blob(st, 1, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, static_cast<int>(q.size()));
    sqlite3_bind_int(st, 5, k);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), sqlite3_column_double(st, 1));
    sqlite3_finalize(st);
    return out;
}

nlohmann::json VectorIndex::Stats() const {
    std::lock_guard<std::mutex> g(mu_);
    if (sqlite_) {
        const_cast<VectorIndex*>(this)->OpenSqliteLocked();
        nlohmann::json models = nlohmann::json::array();
        sqlite3_stmt* st = nullptr;
        long long total = 0;
        sqlite3_prepare_v2(static_cast<sqlite3*>(db_), "SELECT provider, model, dims, COUNT(*) FROM embedding_cache GROUP BY 1,2,3", -1, &st, nullptr);
        while (sqlite3_step(st) == SQLITE_ROW) {
            long long n = sqlite3_column_int64(st, 3);
            total += n;
            models.push_back({{"provider", reinterpret_cast<const char*>(sqlite3_column_text(st, 0))},
                              {"model", reinterpret_cast<const char*>(sqlite3_column_text(st, 1))},
                              {"dims", sqlite3_column_int(st, 2)}, {"entries", n}});
        }
        sqlite3_finalize(st);
        return {{"file", file_.string()}, {"backend", "sqlite"}, {"entries", total}, {"models", models}, {"pruned", last_pruned}};
    }
    return {{"file", file_.string()}, {"backend", "json"}, {"entries", cache_.size()}, {"key", key_}};
}

}  // namespace openclaw::workspace
