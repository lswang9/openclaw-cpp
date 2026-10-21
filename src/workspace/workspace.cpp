#include "openclaw/workspace/workspace.hpp"
#include "openclaw/workspace/embeddings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace openclaw::workspace {
namespace {
constexpr std::uintmax_t kMaxFileBytes = 4u << 20;

std::string Slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}
std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::vector<std::string> Tokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] { if (cur.size() >= 2) out.push_back(cur); cur.clear(); };
    for (unsigned char c : s) {
        if (std::isalnum(c)) cur += static_cast<char>(std::tolower(c));
        else if (c >= 0x80) {  // keep UTF-8 bytes (CJK) as part of tokens; also index single CJK chars below
            cur += static_cast<char>(c);
        } else flush();
    }
    flush();
    // Add CJK character unigrams so Chinese queries match without a segmenter.
    std::vector<std::string> extra;
    for (const auto& t : out) {
        for (std::size_t i = 0; i < t.size();) {
            unsigned char c = t[i];
            std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (n >= 3 && i + n <= t.size()) extra.push_back(t.substr(i, n));
            i += n;
        }
    }
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}
}  // namespace

std::vector<float> LocalEmbed(const std::string& text, int dims) {
    std::vector<float> v(static_cast<std::size_t>(dims), 0.f);
    auto add = [&](const std::string& f, float w) {
        std::uint64_t h = 1469598103934665603ull;
        for (unsigned char ch : f) { h ^= ch; h *= 1099511628211ull; }
        v[h % dims] += ((h >> 32) & 1) ? w : -w;
    };
    for (const auto& t : Tokens(text)) {
        add("w:" + t, 1.0f);
        bool ascii = true;
        for (unsigned char ch : t) if (ch >= 0x80) ascii = false;
        if (ascii && t.size() >= 4) {
            std::string p = "^" + t + "$";
            for (std::size_t i = 0; i + 3 <= p.size(); ++i) add("g:" + p.substr(i, 3), 0.5f);
        }
    }
    double n = 0; for (float x : v) n += double(x) * x;
    if (n > 0) { float inv = static_cast<float>(1.0 / std::sqrt(n)); for (auto& x : v) x *= inv; }
    return v;
}

double Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0; for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) s += double(a[i]) * b[i];
    return s;
}

namespace {
std::string Rel(const fs::path& root, const fs::path& p) { return fs::relative(p, root).generic_string(); }
bool Within(const fs::path& root, const fs::path& p) {
    auto r = root.lexically_normal().generic_string();
    auto s = p.lexically_normal().generic_string();
    if (!r.empty() && r.back() == '/') r.pop_back();
    return s == r || s.rfind(r + "/", 0) == 0;
}
struct Chunk { std::string path; int start; int end; std::string text; };
}  // namespace

Workspace::Workspace(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    fs::create_directories(root_, ec);
    root_ = fs::weakly_canonical(root_, ec);
}

fs::path Workspace::DefaultRoot() {
    if (const char* w = std::getenv("OPENCLAW_WORKSPACE"); w && *w) return w;
    const char* s = std::getenv("OPENCLAW_SESSION_DIR");
    return fs::path(s && *s ? s : "/tmp/openclaw-cpp-sessions") / "workspace";
}

std::optional<fs::path> Workspace::Resolve(const std::string& rel) const {
    if (rel.empty() || rel.find('\0') != std::string::npos) return std::nullopt;
    fs::path p(rel);
    if (p.is_absolute()) { if (!Within(root_, p)) return std::nullopt; }
    else p = root_ / p;
    if (!Within(root_, p.lexically_normal())) return std::nullopt;
    std::error_code ec;
    auto canon = fs::weakly_canonical(p, ec);  // follows existing symlinks
    if (ec || !Within(root_, canon)) return std::nullopt;
    return canon;
}

nlohmann::json Workspace::Read(const std::string& path, int offset, int limit) const {
    auto p = Resolve(path);
    if (!p) return {{"ok", false}, {"error", "path outside workspace"}};
    std::error_code ec;
    if (!fs::is_regular_file(*p, ec)) return {{"ok", false}, {"error", "not found"}, {"path", path}};
    if (fs::file_size(*p, ec) > kMaxFileBytes) return {{"ok", false}, {"error", "file too large"}};
    auto lines = SplitLines(Slurp(*p));
    offset = std::max(1, offset);
    limit = limit <= 0 ? 2000 : std::min(limit, 5000);
    std::ostringstream out;
    int last = offset - 1;
    for (int i = offset; i <= static_cast<int>(lines.size()) && i < offset + limit; ++i) {
        out << lines[i - 1] << '\n';
        last = i;
    }
    return {{"ok", true}, {"path", Rel(root_, *p)}, {"content", out.str()}, {"from", offset}, {"to", last},
            {"total_lines", lines.size()}, {"truncated", last < static_cast<int>(lines.size())}};
}

nlohmann::json Workspace::Write(const std::string& path, const std::string& content) const {
    auto p = Resolve(path);
    if (!p) return {{"ok", false}, {"error", "path outside workspace"}};
    if (content.size() > kMaxFileBytes) return {{"ok", false}, {"error", "content too large"}};
    std::error_code ec;
    fs::create_directories(p->parent_path(), ec);
    auto tmp = *p; tmp += ".oc-tmp";
    { std::ofstream out(tmp, std::ios::binary); out << content; if (!out) return {{"ok", false}, {"error", "write failed"}}; }
    fs::rename(tmp, *p, ec);
    if (ec) return {{"ok", false}, {"error", ec.message()}};
    return {{"ok", true}, {"path", Rel(root_, *p)}, {"bytes", content.size()}};
}

nlohmann::json Workspace::Edit(const std::string& path, const std::string& old_s, const std::string& new_s,
                               bool replace_all) const {
    if (old_s.empty()) return {{"ok", false}, {"error", "old_string must be non-empty"}};
    auto p = Resolve(path);
    if (!p) return {{"ok", false}, {"error", "path outside workspace"}};
    std::error_code ec;
    if (!fs::is_regular_file(*p, ec)) return {{"ok", false}, {"error", "not found"}, {"path", path}};
    auto s = Slurp(*p);
    std::size_t count = 0;
    for (auto pos = s.find(old_s); pos != std::string::npos; pos = s.find(old_s, pos + old_s.size())) ++count;
    if (count == 0) return {{"ok", false}, {"error", "old_string not found"}};
    if (count > 1 && !replace_all)
        return {{"ok", false}, {"error", "old_string matches " + std::to_string(count) + " times; add context or set replace_all"}};
    std::string out;
    std::size_t prev = 0;
    for (auto pos = s.find(old_s); pos != std::string::npos; pos = s.find(old_s, pos + old_s.size())) {
        out.append(s, prev, pos - prev); out += new_s; prev = pos + old_s.size();
        if (!replace_all) break;
    }
    out.append(s, prev, std::string::npos);
    auto w = Write(path, out);
    if (!w.value("ok", false)) return w;
    return {{"ok", true}, {"path", Rel(root_, *p)}, {"replacements", replace_all ? count : 1}};
}

bool Workspace::IsMemoryPath(const std::string& rel) const {
    auto p = Resolve(rel);
    if (!p) return false;
    auto r = Rel(root_, *p);
    bool md = r.size() > 3 && r.substr(r.size() - 3) == ".md";
    return r == "MEMORY.md" || r == "memory.md" || (md && r.rfind("memory/", 0) == 0);
}

nlohmann::json Workspace::MemorySearch(const std::string& query, int max_results, double min_score,
                                       const std::string& mode) const {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const char* top : {"MEMORY.md", "memory.md"})
        if (fs::is_regular_file(root_ / top, ec)) files.push_back(root_ / top);
    if (fs::is_directory(root_ / "memory", ec))
        for (auto it = fs::recursive_directory_iterator(root_ / "memory", fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && it->path().extension() == ".md" && Within(root_, fs::weakly_canonical(it->path(), ec)))
                files.push_back(it->path());
        }
    // Chunk: split on headings / blank lines, cap ~12 lines.
    std::vector<Chunk> chunks;
    for (const auto& f : files) {
        if (fs::file_size(f, ec) > kMaxFileBytes) continue;
        auto lines = SplitLines(Slurp(f));
        int start = 0; std::string buf;
        auto flush = [&](int end) {
            if (buf.find_first_not_of(" \t\r\n") != std::string::npos)
                chunks.push_back({Rel(root_, f), start + 1, end, buf});
            buf.clear();
        };
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            const auto& l = lines[i];
            bool heading = !l.empty() && l[0] == '#';
            if ((heading || l.find_first_not_of(" \t\r") == std::string::npos || i - start >= 12) && !buf.empty()) {
                flush(i); start = i;
            }
            if (buf.empty()) start = i;
            buf += l; buf += '\n';
        }
        flush(static_cast<int>(lines.size()));
    }
    // Hybrid: BM25 (keyword) + local hashed-feature embeddings (cosine), weights 0.3/0.7 (upstream defaults).
    auto q = Tokens(query);
    nlohmann::json results = nlohmann::json::array();
    const std::string backend = mode == "bm25" ? "bm25" : mode == "vector" ? "vector-local" : "hybrid-local";
    if (q.empty() || chunks.empty())
        return {{"ok", true}, {"results", results}, {"files", files.size()}, {"chunks", chunks.size()}, {"backend", backend}};
    std::vector<std::unordered_map<std::string, int>> tf(chunks.size());
    std::unordered_map<std::string, int> df;
    double avg = 0;
    std::vector<int> len(chunks.size());
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        auto toks = Tokens(chunks[i].text);
        len[i] = static_cast<int>(toks.size()); avg += len[i];
        for (auto& t : toks) if (tf[i][t]++ == 0) ++df[t];
    }
    avg /= chunks.size();
    const double k1 = 1.2, b = 0.75, N = static_cast<double>(chunks.size());
    std::vector<double> bm(chunks.size(), 0.0), vec(chunks.size(), 0.0);
    double bmax = 0;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        double s = 0;
        for (auto& t : q) {
            auto it = tf[i].find(t);
            if (it == tf[i].end()) continue;
            double n = df[t];
            double idf = std::log(1 + (N - n + 0.5) / (n + 0.5));
            double f = it->second;
            s += idf * f * (k1 + 1) / (f + k1 * (1 - b + b * len[i] / std::max(1.0, avg)));
        }
        bm[i] = s; bmax = std::max(bmax, s);
    }
    std::string vec_backend = "local", fallback;
    nlohmann::json index_info;
    if (mode != "bm25") {
        bool done = false;
        if (embedder_ && index_) {
            try {
                std::vector<std::string> texts;
                for (auto& c : chunks) texts.push_back(c.text);
                auto vs = index_->Get(*embedder_, texts);
                auto qv = embedder_->Embed({query}).at(0);
                for (std::size_t i = 0; i < chunks.size(); ++i) vec[i] = std::max(0.0, Cosine(qv, vs[i]));
                vec_backend = embedder_->Id();
                index_info = {{"hits", index_->last_hits}, {"misses", index_->last_misses}};
                done = true;
            } catch (const std::exception& e) {
                fallback = e.what();  // remote provider failed → local embeddings
            }
        }
        if (!done) {
            auto qv = LocalEmbed(query);
            for (std::size_t i = 0; i < chunks.size(); ++i) vec[i] = std::max(0.0, Cosine(qv, LocalEmbed(chunks[i].text)));
        }
    }
    std::vector<std::pair<double, std::size_t>> scored;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        double s;
        if (mode == "bm25") s = bm[i];
        else if (mode == "vector") s = vec[i] >= 0.12 ? vec[i] : 0;
        else {
            double tn = bmax > 0 ? bm[i] / bmax : 0;
            s = (bm[i] > 0 || vec[i] >= 0.12) ? 0.7 * vec[i] + 0.3 * tn : 0;
        }
        if (s > 0 && s >= min_score) scored.push_back({s, i});
    }
    std::sort(scored.begin(), scored.end(), [&](auto& a, auto& c) {
        if (a.first != c.first) return a.first > c.first;
        return chunks[a.second].path > chunks[c.second].path;  // newer daily files first on ties
    });
    max_results = std::clamp(max_results, 1, 50);
    for (std::size_t k = 0; k < scored.size() && static_cast<int>(k) < max_results; ++k) {
        auto& c = chunks[scored[k].second];
        auto snip = c.text.size() > 700 ? c.text.substr(0, 700) + "…" : c.text;
        results.push_back({{"path", c.path}, {"start_line", c.start}, {"end_line", c.end},
                           {"score", std::round(scored[k].first * 1000) / 1000},
                           {"textScore", std::round(bm[scored[k].second] * 1000) / 1000},
                           {"vectorScore", std::round(vec[scored[k].second] * 1000) / 1000}, {"snippet", snip}});
    }
    nlohmann::json out{{"ok", true}, {"results", results}, {"files", files.size()}, {"chunks", chunks.size()},
                       {"backend", mode == "bm25" ? backend : (mode == "vector" ? "vector-" : "hybrid-") + vec_backend}};
    if (!fallback.empty()) out["fallback"] = fallback;
    if (!index_info.is_null()) out["index"] = index_info;
    return out;
}

void Workspace::SetEmbedder(std::shared_ptr<EmbeddingProvider> p, fs::path index_file) {
    embedder_ = std::move(p);
    index_ = embedder_ ? std::make_shared<VectorIndex>(std::move(index_file)) : nullptr;
}

nlohmann::json Workspace::IndexStats() const {
    if (!index_) return {{"provider", "local"}, {"persisted", false}};
    auto s = index_->Stats();
    s["provider"] = embedder_->Id(); s["model"] = embedder_->Model(); s["persisted"] = true;
    return s;
}

nlohmann::json Workspace::MemoryGet(const std::string& path, int from, int lines) const {
    if (!IsMemoryPath(path)) return {{"ok", false}, {"error", "memory_get only reads MEMORY.md or memory/**/*.md"}};
    auto r = Read(path, from, lines <= 0 ? 400 : lines);
    if (r.contains("content")) r["text"] = r["content"];
    return r;
}

nlohmann::json Workspace::MemoryAppend(const std::string& text) const {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{}; gmtime_r(&t, &tm);
    char day[16], hm[16];
    std::strftime(day, sizeof day, "%Y-%m-%d", &tm);
    std::strftime(hm, sizeof hm, "%H:%M", &tm);
    auto rel = std::string("memory/") + day + ".md";
    auto p = Resolve(rel);
    if (!p) return {{"ok", false}, {"error", "bad workspace"}};
    std::error_code ec;
    fs::create_directories(p->parent_path(), ec);
    bool fresh = !fs::exists(*p, ec);
    std::ofstream out(*p, std::ios::app);
    if (fresh) out << "# " << day << "\n\n";
    out << "- " << hm << "Z " << text << "\n";
    return {{"ok", true}, {"path", rel}};
}

}  // namespace openclaw::workspace
