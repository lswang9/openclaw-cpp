#include "openclaw/config/config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace openclaw::config {
namespace fs = std::filesystem;

fs::path DefaultPath() {
    if (const char* p = std::getenv("OPENCLAW_CONFIG_PATH"); p && *p) return fs::path(p);
    const char* home = std::getenv("HOME");
    if (!home || !*home) return fs::path("/tmp/openclaw-cpp-config.json");
    fs::path up = fs::path(home) / ".openclaw" / "openclaw.json";
    fs::path legacy = fs::path(home) / ".openclaw-cpp" / "config.json";
    std::error_code ec;
    if (!fs::exists(up, ec) && fs::exists(legacy, ec)) return legacy;
    return up;
}

// ---------------- JSON5 subset ----------------
nlohmann::json ParseJson5(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0, n = in.size();
    int line = 1;
    auto fail = [&](const std::string& m) { throw std::runtime_error("json5 line " + std::to_string(line) + ": " + m); };
    auto is_id0 = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; };
    auto is_id = [&](char c) { return is_id0(c) || std::isdigit(static_cast<unsigned char>(c)); };
    // Emit pending tokens; track last significant char for trailing-comma removal.
    auto drop_trailing_comma = [&]() {
        std::size_t k = out.size();
        while (k > 0 && std::isspace(static_cast<unsigned char>(out[k - 1]))) --k;
        if (k > 0 && out[k - 1] == ',') out.erase(k - 1, 1);
    };
    auto last_sig = [&]() -> char {
        for (std::size_t k = out.size(); k > 0; --k)
            if (!std::isspace(static_cast<unsigned char>(out[k - 1]))) return out[k - 1];
        return 0;
    };
    while (i < n) {
        char c = in[i];
        if (c == '\n') { ++line; out += c; ++i; continue; }
        if (c == '/' && i + 1 < n && in[i + 1] == '/') { while (i < n && in[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < n && in[i + 1] == '*') {
            auto e = in.find("*/", i + 2);
            if (e == std::string::npos) fail("unterminated comment");
            for (std::size_t k = i; k < e; ++k) if (in[k] == '\n') { ++line; out += '\n'; }
            i = e + 2; continue;
        }
        if (c == '"' || c == '\'') {
            char q = c; ++i; std::string s = "\"";
            while (i < n && in[i] != q) {
                if (in[i] == '\n') fail("newline in string");
                if (in[i] == '\\' && i + 1 < n) {
                    char e = in[i + 1];
                    if (e == '\'') s += '\'';
                    else if (e == '\n') { ++line; }
                    else { s += '\\'; s += e; }
                    i += 2; continue;
                }
                if (in[i] == '"' && q == '\'') s += "\\\"";
                else s += in[i];
                ++i;
            }
            if (i >= n) fail("unterminated string");
            ++i; s += '"'; out += s; continue;
        }
        if (c == '}' || c == ']') { drop_trailing_comma(); out += c; ++i; continue; }
        if (is_id0(c)) {
            std::size_t j = i; while (j < n && is_id(in[j])) ++j;
            std::string word = in.substr(i, j - i);
            std::size_t k = j; while (k < n && (in[k] == ' ' || in[k] == '\t')) ++k;
            char ls = last_sig();
            if (k < n && in[k] == ':' && (ls == '{' || ls == ',')) out += "\"" + word + "\"";
            else if (word == "Infinity" || word == "NaN") fail("Infinity/NaN not supported");
            else out += word;  // true/false/null
            i = j; continue;
        }
        if (c == '+' && i + 1 < n && (std::isdigit(static_cast<unsigned char>(in[i + 1])) || in[i + 1] == '.')) { ++i; continue; }
        if (c == '0' && i + 1 < n && (in[i + 1] == 'x' || in[i + 1] == 'X')) {
            std::size_t j = i + 2; while (j < n && std::isxdigit(static_cast<unsigned char>(in[j]))) ++j;
            out += std::to_string(std::stoll(in.substr(i + 2, j - i - 2), nullptr, 16)); i = j; continue;
        }
        if (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(in[i + 1]))) { out += "0."; ++i; continue; }
        out += c; ++i;
    }
    try {
        return nlohmann::json::parse(out);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(std::string("config parse: ") + e.what());
    }
}

nlohmann::json SubstituteEnv(const nlohmann::json& j) {
    if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        std::string o;
        for (std::size_t i = 0; i < s.size();) {
            if (s.compare(i, 3, "$${") == 0) { o += "${"; i += 3; continue; }
            if (s.compare(i, 2, "${") == 0) {
                auto e = s.find('}', i + 2);
                std::string name = e == std::string::npos ? "" : s.substr(i + 2, e - i - 2);
                bool valid = !name.empty() && (std::isupper(static_cast<unsigned char>(name[0])) || name[0] == '_');
                for (char ch : name) if (!(std::isupper(static_cast<unsigned char>(ch)) || std::isdigit(static_cast<unsigned char>(ch)) || ch == '_')) valid = false;
                if (valid) {
                    const char* v = std::getenv(name.c_str());
                    if (!v || !*v) throw std::runtime_error("missing env var referenced in config: " + name);
                    o += v; i = e + 1; continue;
                }
            }
            o += s[i++];
        }
        return o;
    }
    if (j.is_object()) {
        nlohmann::json r = nlohmann::json::object();
        for (auto it = j.begin(); it != j.end(); ++it) r[it.key()] = SubstituteEnv(it.value());
        return r;
    }
    if (j.is_array()) {
        nlohmann::json r = nlohmann::json::array();
        for (const auto& v : j) r.push_back(SubstituteEnv(v));
        return r;
    }
    return j;
}

// ---------------- paths ----------------
namespace {
struct Seg { std::string key; long idx = -1; };
std::optional<std::vector<Seg>> SplitPath(const std::string& p) {
    std::vector<Seg> segs;
    std::string cur;
    for (std::size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        if (c == '.') { if (cur.empty() && (segs.empty() || segs.back().idx < 0)) return std::nullopt; if (!cur.empty()) segs.push_back({cur}); cur.clear(); }
        else if (c == '[') {
            if (!cur.empty()) segs.push_back({cur}); cur.clear();
            auto e = p.find(']', i);
            if (e == std::string::npos) return std::nullopt;
            try { segs.push_back({"", std::stol(p.substr(i + 1, e - i - 1))}); } catch (...) { return std::nullopt; }
            i = e;
        } else cur += c;
    }
    if (!cur.empty()) segs.push_back({cur});
    if (segs.empty()) return std::nullopt;
    return segs;
}
}  // namespace

std::optional<nlohmann::json> GetPath(const nlohmann::json& root, const std::string& path) {
    auto segs = SplitPath(path);
    if (!segs) return std::nullopt;
    const nlohmann::json* cur = &root;
    for (auto& s : *segs) {
        if (s.idx >= 0) { if (!cur->is_array() || s.idx >= static_cast<long>(cur->size())) return std::nullopt; cur = &(*cur)[s.idx]; }
        else { if (!cur->is_object() || !cur->contains(s.key)) return std::nullopt; cur = &(*cur)[s.key]; }
    }
    return *cur;
}

bool SetPath(nlohmann::json& root, const std::string& path, const nlohmann::json& value) {
    auto segs = SplitPath(path);
    if (!segs) return false;
    nlohmann::json* cur = &root;
    for (std::size_t k = 0; k < segs->size(); ++k) {
        auto& s = (*segs)[k];
        bool last = k + 1 == segs->size();
        if (s.idx >= 0) {
            if (!cur->is_array()) return false;
            if (s.idx > static_cast<long>(cur->size())) return false;
            if (s.idx == static_cast<long>(cur->size())) cur->push_back(nullptr);
            cur = &(*cur)[s.idx];
        } else {
            if (cur->is_null()) *cur = nlohmann::json::object();
            if (!cur->is_object()) return false;
            cur = &(*cur)[s.key];
        }
        if (last) *cur = value;
        else if (cur->is_null()) *cur = (*segs)[k + 1].idx >= 0 ? nlohmann::json::array() : nlohmann::json::object();
    }
    return true;
}

bool UnsetPath(nlohmann::json& root, const std::string& path) {
    auto segs = SplitPath(path);
    if (!segs) return false;
    nlohmann::json* cur = &root;
    for (std::size_t k = 0; k + 1 < segs->size(); ++k) {
        auto& s = (*segs)[k];
        if (s.idx >= 0) { if (!cur->is_array() || s.idx >= static_cast<long>(cur->size())) return false; cur = &(*cur)[s.idx]; }
        else { if (!cur->is_object() || !cur->contains(s.key)) return false; cur = &(*cur)[s.key]; }
    }
    auto& l = segs->back();
    if (l.idx >= 0) { if (!cur->is_array() || l.idx >= static_cast<long>(cur->size())) return false; cur->erase(l.idx); return true; }
    if (!cur->is_object() || !cur->contains(l.key)) return false;
    cur->erase(l.key);
    return true;
}

// ---------------- load/save ----------------
namespace {
void ApplyTyped(Snapshot& s, const nlohmann::json& j) {
    if (j.contains("gateway") && j["gateway"].is_object()) {
        const auto& g = j["gateway"];
        if (g.contains("bind") && g["bind"].is_string()) {
            auto b = g["bind"].get<std::string>();
            if (b == "loopback") s.gateway_host = "127.0.0.1";
            else if (b == "lan" || b == "all") s.gateway_host = "0.0.0.0";
        }
        s.gateway_host = g.value("host", s.gateway_host);
        if (g.contains("port") && g["port"].is_number_integer()) s.gateway_port = static_cast<std::uint16_t>(g["port"].get<int>());
    }
    if (auto m = GetPath(j, "agents.defaults.model")) {
        if (m->is_string()) s.deepseek_model = m->get<std::string>();
        else if (m->is_object() && m->contains("primary") && (*m)["primary"].is_string()) {
            auto p = (*m)["primary"].get<std::string>();
            auto slash = p.find('/');
            s.deepseek_model = slash == std::string::npos ? p : p.substr(slash + 1);
        }
    }
    if (j.contains("session_dir") && j["session_dir"].is_string()) s.session_dir = j["session_dir"];
    if (j.contains("align_target") && j["align_target"].is_string()) s.align_target = j["align_target"];
}
}  // namespace

LoadResult Load(const fs::path& path) {
    LoadResult r;
    std::error_code ec;
    if (!fs::exists(path, ec)) return r;
    r.exists = true;
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    try {
        auto j = ParseJson5(ss.str());
        if (!j.is_object()) throw std::runtime_error("config root must be an object");
        // legacy openclaw-cpp files nested the whole doc under "raw": unwrap.
        if (j.contains("raw") && j["raw"].is_object()) {
            auto inner = j["raw"]; j.erase("raw"); inner.merge_patch(j); j = inner;
            while (j.contains("raw") && j["raw"].is_object()) { auto x = j["raw"]; j.erase("raw"); x.merge_patch(j); j = x; }
        }
        r.snap.raw = j;  // keep unsubstituted for round-trip writes
        ApplyTyped(r.snap, SubstituteEnv(j));
    } catch (const std::exception& e) {
        r.error = e.what();
        r.snap = Snapshot{};
    }
    return r;
}

Snapshot LoadOrDefault(const fs::path& path) { return Load(path).snap; }

nlohmann::json ToFileJson(const Snapshot& snap) {
    nlohmann::json j = snap.raw.is_object() ? snap.raw : nlohmann::json::object();
    j.erase("raw");
    j["gateway"]["host"] = snap.gateway_host;
    j["gateway"]["port"] = snap.gateway_port;
    auto m = GetPath(j, "agents.defaults.model");
    if (m && m->is_object()) {
        std::string p = m->value("primary", "");
        auto slash = p.find('/');
        j["agents"]["defaults"]["model"]["primary"] = (slash == std::string::npos ? "" : p.substr(0, slash + 1)) + snap.deepseek_model;
    } else {
        j["agents"]["defaults"]["model"] = snap.deepseek_model;
    }
    if (snap.session_dir != Snapshot{}.session_dir) j["session_dir"] = snap.session_dir;
    return j;
}

void SaveRaw(const fs::path& path, const nlohmann::json& file_json) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    std::error_code ec;
    if (fs::exists(path, ec)) fs::copy_file(path, fs::path(path.string() + ".bak"), fs::copy_options::overwrite_existing, ec);
    auto tmp = fs::path(path.string() + ".tmp");
    { std::ofstream out(tmp); out << file_json.dump(2) << "\n"; }
    fs::rename(tmp, path);
}

void Save(const fs::path& path, const Snapshot& snap) { SaveRaw(path, ToFileJson(snap)); }

}  // namespace openclaw::config
