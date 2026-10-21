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

VectorIndex::VectorIndex(fs::path file) : file_(std::move(file)) {}

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

std::vector<Vec> VectorIndex::Get(EmbeddingProvider& prov, const std::vector<std::string>& texts) {
    std::lock_guard<std::mutex> g(mu_);
    if (!loaded_) LoadLocked();
    const std::string key = prov.Id() + ":" + prov.Model();
    if (key != key_) { cache_.clear(); key_ = key; }  // provider/model changed → full reindex
    std::vector<std::string> hashes;
    std::vector<std::string> miss_text;
    std::vector<std::size_t> miss_idx;
    for (std::size_t i = 0; i < texts.size(); ++i) {
        hashes.push_back(Sha256Hex(texts[i]));
        if (!cache_.count(hashes.back())) { miss_text.push_back(texts[i]); miss_idx.push_back(i); }
    }
    last_misses = miss_text.size();
    last_hits = texts.size() - miss_text.size();
    if (!miss_text.empty()) {
        auto vecs = prov.Embed(miss_text);
        for (std::size_t k = 0; k < vecs.size(); ++k) cache_[hashes[miss_idx[k]]] = std::move(vecs[k]);
        SaveLocked();
    }
    std::vector<Vec> out;
    for (auto& h : hashes) out.push_back(cache_[h]);
    return out;
}

nlohmann::json VectorIndex::Stats() const {
    std::lock_guard<std::mutex> g(mu_);
    return {{"file", file_.string()}, {"entries", cache_.size()}, {"key", key_}};
}

}  // namespace openclaw::workspace
