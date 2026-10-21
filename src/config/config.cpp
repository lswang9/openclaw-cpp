#include <set>
#include "openclaw/config/config.hpp"

#include <algorithm>
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


bool IsSecretRef(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("source") || !j.contains("id") || !j["source"].is_string() || !j["id"].is_string()) return false;
    const auto s = j["source"].get<std::string>();
    if (s != "env" && s != "file" && s != "exec") return false;
    for (auto it = j.begin(); it != j.end(); ++it) if (it.key() != "source" && it.key() != "id" && it.key() != "provider") return false;
    return true;
}
namespace {
std::string JsonPointerGet(const nlohmann::json& doc, const std::string& ptr, std::string* err) {
    if (ptr.empty() || ptr[0] != '/') { *err = "file ref id must be a JSON pointer (\"/a/b\")"; return {}; }
    const nlohmann::json* cur = &doc; std::size_t i = 1;
    while (true) {
        auto e = ptr.find('/', i); std::string tok = ptr.substr(i, e == std::string::npos ? std::string::npos : e - i);
        for (std::size_t k; (k = tok.find("~1")) != std::string::npos;) tok.replace(k, 2, "/");
        for (std::size_t k; (k = tok.find("~0")) != std::string::npos;) tok.replace(k, 2, "~");
        if (cur->is_object() && cur->contains(tok)) cur = &(*cur)[tok];
        else if (cur->is_array() && !tok.empty() && std::all_of(tok.begin(), tok.end(), ::isdigit) && std::stoul(tok) < cur->size()) cur = &(*cur)[std::stoul(tok)];
        else { *err = "JSON pointer " + ptr + " not found"; return {}; }
        if (e == std::string::npos) break; i = e + 1;
    }
    if (!cur->is_string()) { *err = "value at " + ptr + " is not a string"; return {}; }
    return cur->get<std::string>();
}
std::string ExpandTilde(const std::string& p) { if (p.rfind("~/", 0) == 0) if (const char* h = std::getenv("HOME")) return std::string(h) + p.substr(1); return p; }
}  // namespace
std::string ResolveSecretRef(const nlohmann::json& ref, const nlohmann::json& raw, std::string* err) {
    const auto src = ref.value("source", std::string()), id = ref.value("id", std::string());
    const auto pname = ref.value("provider", std::string("default"));
    nlohmann::json prov = nlohmann::json::object();
    if (raw.contains("secrets") && raw["secrets"].is_object() && raw["secrets"].contains("providers") && raw["secrets"]["providers"].is_object() && raw["secrets"]["providers"].contains(pname))
        prov = raw["secrets"]["providers"][pname];
    if (prov.is_object() && prov.contains("source") && prov["source"] != src) { *err = "provider \"" + pname + "\" is a " + prov["source"].get<std::string>() + " provider, ref uses " + src; return {}; }
    if (src == "env") {
        if (prov.is_object() && prov.contains("allowlist") && prov["allowlist"].is_array()) {
            bool ok = false; for (auto& a : prov["allowlist"]) ok = ok || (a.is_string() && a.get<std::string>() == id);
            if (!ok) { *err = "env var " + id + " is not in secrets.providers." + pname + ".allowlist"; return {}; }
        }
        const char* v = std::getenv(id.c_str()); if (!v || !*v) { *err = "env var " + id + " is not set"; return {}; }
        return v;
    }
    if (src == "file") {
        if (!prov.is_object() || !prov.contains("path")) { *err = "file provider \"" + pname + "\" is not configured (secrets.providers." + pname + ".path)"; return {}; }
        std::ifstream f(ExpandTilde(prov["path"].get<std::string>()), std::ios::binary); if (!f) { *err = "cannot read secrets file for provider \"" + pname + "\""; return {}; }
        std::stringstream ss; ss << f.rdbuf(); std::string body = ss.str();
        if (prov.value("mode", std::string("json")) == "singleValue") {
            if (id != "value") { *err = "singleValue file provider \"" + pname + "\" expects ref id \"value\"."; return {}; }
            while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back(); return body;
        }
        nlohmann::json doc; try { doc = nlohmann::json::parse(body); } catch (...) { *err = "secrets file for provider \"" + pname + "\" is not valid JSON"; return {}; }
        return JsonPointerGet(doc, id, err);
    }
    if (src == "exec") {
        if (!prov.is_object() || !prov.contains("command")) { *err = "exec provider \"" + pname + "\" is not configured"; return {}; }
        std::string cmd = "'" + prov["command"].get<std::string>() + "'";
        if (prov.contains("args") && prov["args"].is_array()) for (auto& a : prov["args"]) { std::string s = a.get<std::string>(), q; for (char ch : s) q += ch == '\'' ? std::string("'\\''") : std::string(1, ch); cmd += " '" + q + "'"; }
        std::string envp; if (prov.contains("env") && prov["env"].is_object()) for (auto it = prov["env"].begin(); it != prov["env"].end(); ++it) envp += it.key() + "='" + it.value().get<std::string>() + "' ";
        std::string pass; if (prov.contains("passEnv") && prov["passEnv"].is_array()) for (auto& k : prov["passEnv"]) if (const char* v = std::getenv(k.get<std::string>().c_str())) pass += k.get<std::string>() + "='" + v + "' ";
        const std::string input = nlohmann::json{{"protocolVersion", 1}, {"provider", pname}, {"ids", {id}}}.dump();
        const std::string full = "printf '%s' '" + input + "' | env -i PATH=/usr/bin:/bin " + pass + envp + "timeout " + std::to_string(std::max(1, prov.value("timeoutMs", 5000) / 1000)) + " " + cmd + " 2>/dev/null";
        FILE* p = popen(full.c_str(), "r"); if (!p) { *err = "exec provider failed to start"; return {}; }
        std::string out; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, p)) > 0 && out.size() < (1u << 20)) out.append(b, n);
        const int rc = pclose(p); if (rc != 0) { *err = "exec provider \"" + pname + "\" exited with status " + std::to_string(rc); return {}; }
        try {
            auto j = nlohmann::json::parse(out);
            if (j.value("protocolVersion", 0) != 1 || !j.contains("values") || !j["values"].contains(id)) { *err = "exec provider response missing values." + id; return {}; }
            return j["values"][id].is_string() ? j["values"][id].get<std::string>() : j["values"][id].dump();
        } catch (...) {
            if (!prov.value("jsonOnly", true)) { while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back(); return out; }
            *err = "exec provider returned invalid JSON"; return {};
        }
    }
    *err = "unknown secret source " + src; return {};
}
nlohmann::json ResolveSecretRefs(const nlohmann::json& j, const nlohmann::json& raw, const std::string& path) {
    if (IsSecretRef(j)) {
        if (path.rfind("secrets.", 0) == 0) return j;
        std::string err; auto v = ResolveSecretRef(j, raw, &err);
        if (!err.empty()) throw std::runtime_error("unresolved SecretRef at " + path + ": " + err);
        return v;
    }
    if (j.is_object()) { nlohmann::json r = nlohmann::json::object(); for (auto it = j.begin(); it != j.end(); ++it) r[it.key()] = ResolveSecretRefs(it.value(), raw, path.empty() ? it.key() : path + "." + it.key()); return r; }
    if (j.is_array()) { nlohmann::json r = nlohmann::json::array(); std::size_t i = 0; for (const auto& v : j) r.push_back(ResolveSecretRefs(v, raw, path + "[" + std::to_string(i++) + "]")); return r; }
    return j;
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

// ---------------- $include ----------------
namespace {
void DeepMerge(nlohmann::json& dst, const nlohmann::json& src) {
    if (!dst.is_object() || !src.is_object()) { dst = src; return; }
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (dst.contains(it.key()) && dst[it.key()].is_object() && it.value().is_object()) DeepMerge(dst[it.key()], it.value());
        else dst[it.key()] = it.value();
    }
}
std::string Slurp(const fs::path& p) { std::ifstream in(p); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
}  // namespace

nlohmann::json ResolveIncludes(const nlohmann::json& doc, const fs::path& base, nlohmann::json* spec_out,
                               nlohmann::json* included_out, int depth, std::vector<std::string>* stack) {
    if (!doc.is_object() || !doc.contains("$include")) return doc;
    if (depth >= 10) throw std::runtime_error("$include nesting too deep");
    std::vector<std::string> local_stack;
    if (!stack) stack = &local_stack;
    const auto spec = doc["$include"];
    std::vector<std::string> paths;
    if (spec.is_string()) paths.push_back(spec.get<std::string>());
    else if (spec.is_array()) { for (auto& p : spec) { if (!p.is_string()) throw std::runtime_error("$include entries must be strings"); paths.push_back(p); } }
    else throw std::runtime_error("$include must be a string or array");
    nlohmann::json merged = nlohmann::json::object();
    for (const auto& p : paths) {
        fs::path fp = fs::path(p).is_absolute() ? fs::path(p) : base / p;
        std::error_code ec;
        auto canon = fs::weakly_canonical(fp, ec).string();
        if (std::find(stack->begin(), stack->end(), canon) != stack->end()) throw std::runtime_error("$include cycle at " + p);
        if (!fs::is_regular_file(fp, ec)) throw std::runtime_error("$include not found: " + fp.string());
        stack->push_back(canon);
        auto sub = ResolveIncludes(ParseJson5(Slurp(fp)), fp.parent_path(), nullptr, nullptr, depth + 1, stack);
        stack->pop_back();
        if (!sub.is_object()) throw std::runtime_error("$include target must be an object: " + p);
        DeepMerge(merged, sub);
    }
    nlohmann::json own = doc; own.erase("$include");
    if (spec_out) *spec_out = spec;
    if (included_out) *included_out = merged;
    DeepMerge(merged, own);
    return merged;
}

nlohmann::json ToFileDoc(const Snapshot& snap, const nlohmann::json& effective) {
    if (snap.include_spec.is_null()) return effective;
    nlohmann::json out = nlohmann::json::object();
    out["$include"] = snap.include_spec;
    for (auto it = effective.begin(); it != effective.end(); ++it)
        if (!snap.included.contains(it.key()) || snap.included[it.key()] != it.value()) out[it.key()] = it.value();
    return out;
}

// ---------------- schema ----------------
SchemaReport ValidateSchema(const nlohmann::json& raw) {
    SchemaReport r;
    static const std::vector<std::string> known = {
        "$schema", "meta", "auth", "env", "wizard", "diagnostics", "logging", "update", "browser", "ui", "models",
        "nodeHost", "agents", "tools", "bindings", "broadcast", "audio", "media", "messages", "commands", "approvals",
        "session", "cron", "hooks", "web", "channels", "discovery", "canvasHost", "talk", "gateway", "memory", "skills",
        "plugins", "acp", "secrets", "mcp",
        // openclaw-cpp extensions / gateway-managed state
        "session_dir", "align_target", "agents_list", "audit_log"};
    if (!raw.is_object()) { r.errors.push_back("<root>: must be an object"); return r; }
    for (auto it = raw.begin(); it != raw.end(); ++it)
        if (std::find(known.begin(), known.end(), it.key()) == known.end()) r.warnings.push_back(it.key() + ": unknown key");
    auto obj = [&](const char* p) {
        auto v = GetPath(raw, p);
        if (v && !v->is_object()) r.errors.push_back(std::string(p) + ": expected object");
        return v && v->is_object();
    };
    auto expect = [&](const std::string& p, auto pred, const char* what) {
        if (auto v = GetPath(raw, p); v && !pred(*v) && !IsSecretRef(*v)) r.errors.push_back(p + ": expected " + what);
    };
    auto isint = [](const nlohmann::json& v) { return v.is_number_integer(); };
    auto isstr = [](const nlohmann::json& v) { return v.is_string(); };
    auto isbool = [](const nlohmann::json& v) { return v.is_boolean(); };
    for (auto p : {"gateway", "agents", "agents.defaults", "session", "session.reset", "session.resetByType",
                   "session.resetByChannel", "skills", "skills.entries", "channels", "tools", "memory"}) obj(p);
    expect("gateway.port", [](const nlohmann::json& v) { return v.is_number_integer() && v.get<long>() >= 1 && v.get<long>() <= 65535; }, "integer 1-65535");
    expect("gateway.host", isstr, "string");
    expect("gateway.bind", [](const nlohmann::json& v) { return v.is_string() && (v == "loopback" || v == "lan" || v == "all" || v == "tailnet" || v == "auto" || v == "custom"); }, "loopback|lan|tailnet|auto|custom");
    expect("gateway.reload.mode", [](const nlohmann::json& v) { return v.is_string() && (v == "off" || v == "hot" || v == "restart" || v == "hybrid"); }, "off|hot|restart|hybrid");
    expect("agents.defaults.model", [](const nlohmann::json& v) {
        if (v.is_string()) return true;
        if (!v.is_object()) return false;
        if (v.contains("primary") && !v["primary"].is_string()) return false;
        if (v.contains("fallbacks") && !v["fallbacks"].is_array()) return false;
        return true; }, "string or {primary, fallbacks[]}");
    expect("agents.defaults.workspace", isstr, "string");
    expect("agents.defaults.contextTokens", [](const nlohmann::json& v) { return v.is_number_unsigned() && v.get<long>() > 0; }, "positive integer");
    expect("agents.defaults.compaction.mode", [](const nlohmann::json& v) { return v == "default" || v == "safeguard" || v == "off"; }, "default|safeguard|off");
    expect("agents.defaults.compaction.reserveTokens", [](const nlohmann::json& v) { return v.is_number_unsigned(); }, "non-negative integer");
    expect("agents.defaults.compaction.memoryFlush.enabled", isbool, "boolean");
    auto reset = [&](const std::string& base) {
        expect(base + ".mode", [](const nlohmann::json& v) { return v == "daily" || v == "idle" || v == "off"; }, "daily|idle|off");
        expect(base + ".atHour", [](const nlohmann::json& v) { return v.is_number_integer() && v.get<int>() >= 0 && v.get<int>() <= 23; }, "integer 0-23");
        expect(base + ".idleMinutes", [](const nlohmann::json& v) { return v.is_number_unsigned(); }, "non-negative integer");
    };
    reset("session.reset");
    for (auto grp : {"session.resetByType", "session.resetByChannel"})
        if (auto g = GetPath(raw, grp); g && g->is_object())
            for (auto it = g->begin(); it != g->end(); ++it) reset(std::string(grp) + "." + it.key());
    if (auto e = GetPath(raw, "skills.entries"); e && e->is_object())
        for (auto it = e->begin(); it != e->end(); ++it) {
            auto b = "skills.entries." + it.key();
            if (!it.value().is_object()) { r.errors.push_back(b + ": expected object"); continue; }
            expect(b + ".enabled", isbool, "boolean");
            expect(b + ".apiKey", isstr, "string");
            expect(b + ".env", [](const nlohmann::json& v) {
                if (!v.is_object()) return false;
                for (auto& x : v) if (!x.is_string()) return false;
                return true; }, "object of strings");
        }
    auto isstrarr = [](const nlohmann::json& v) {
        if (!v.is_array()) return false;
        for (auto& x : v) if (!x.is_string() && !x.is_number()) return false;
        return true;
    };
    auto oneof = [](std::initializer_list<const char*> opts) {
        std::vector<std::string> o(opts.begin(), opts.end());
        return [o](const nlohmann::json& v) { return v.is_string() && std::find(o.begin(), o.end(), v.get<std::string>()) != o.end(); };
    };
    auto isobj = [](const nlohmann::json& v) { return v.is_object(); };
    auto ispos = [](const nlohmann::json& v) { return v.is_number_unsigned() && v.get<long>() > 0; };
    // session
    expect("session.dmScope", oneof({"main", "per-peer", "per-channel-peer", "per-account-channel-peer"}), "main|per-peer|per-channel-peer|per-account-channel-peer");
    expect("session.keyFormat", oneof({"upstream", "legacy"}), "upstream|legacy");
    // gateway auth
    expect("gateway.auth", isobj, "object");
    expect("gateway.auth.mode", oneof({"token", "password", "none", "trusted-proxy"}), "token|password|none|trusted-proxy");
    expect("gateway.auth.token", isstr, "string");
    expect("gateway.auth.password", isstr, "string");
    // logging
    expect("logging.level", oneof({"silent", "fatal", "error", "warn", "info", "debug", "trace"}), "silent|fatal|error|warn|info|debug|trace");
    // tools
    expect("tools.allow", isstrarr, "array of strings");
    expect("tools.deny", isstrarr, "array of strings");
    expect("tools.profile", oneof({"minimal", "coding", "messaging", "full"}), "minimal|coding|messaging|full");
    expect("tools.exec.host", oneof({"sandbox", "gateway", "node"}), "sandbox|gateway|node");
    expect("tools.exec.security", oneof({"deny", "allowlist", "full"}), "deny|allowlist|full");
    expect("tools.exec.timeoutSec", ispos, "positive integer");
    // memory search
    expect("agents.defaults.memorySearch.enabled", isbool, "boolean");
    expect("agents.defaults.memorySearch.provider", oneof({"local", "openai", "gemini", "voyage", "mistral", "ollama", "gguf", "local-gguf", "auto"}), "local|openai|gemini|voyage|mistral|ollama|auto");
    expect("agents.defaults.memorySearch.model", isstr, "string");
    expect("agents.defaults.memorySearch.remote.baseUrl", isstr, "string");
    expect("agents.defaults.memorySearch.remote.apiKey", isstr, "string");
    expect("agents.defaults.memorySearch.query.hybrid.vectorWeight", [](const nlohmann::json& v) { return v.is_number() && v.get<double>() >= 0 && v.get<double>() <= 1; }, "number 0-1");
    expect("agents.defaults.memorySearch.query.hybrid.textWeight", [](const nlohmann::json& v) { return v.is_number() && v.get<double>() >= 0 && v.get<double>() <= 1; }, "number 0-1");
    expect("agents.defaults.memorySearch.cache.enabled", isbool, "boolean");
    expect("agents.defaults.timeoutSeconds", ispos, "positive integer");
    expect("agents.defaults.maxConcurrent", ispos, "positive integer");
    // agents.list / bindings
    if (auto al = GetPath(raw, "agents.list")) {
        if (!al->is_array()) r.errors.push_back("agents.list: expected array");
        else for (std::size_t i = 0; i < al->size(); ++i) {
            const auto& a = (*al)[i];
            auto b = "agents.list[" + std::to_string(i) + "]";
            if (!a.is_object() || !a.contains("id") || !a["id"].is_string() || a["id"].get<std::string>().empty())
                r.errors.push_back(b + ".id: required non-empty string");
            if (a.is_object() && a.contains("default") && !a["default"].is_boolean()) r.errors.push_back(b + ".default: expected boolean");
            if (a.is_object() && a.contains("workspace") && !a["workspace"].is_string()) r.errors.push_back(b + ".workspace: expected string");
        }
    }
    if (auto bl = GetPath(raw, "bindings")) {
        if (!bl->is_array()) r.errors.push_back("bindings: expected array");
        else for (std::size_t i = 0; i < bl->size(); ++i) {
            const auto& b = (*bl)[i];
            auto p = "bindings[" + std::to_string(i) + "]";
            if (!b.is_object() || !b.contains("agentId") || !b["agentId"].is_string()) r.errors.push_back(p + ".agentId: required string");
            if (b.is_object() && b.contains("match") && !b["match"].is_object()) r.errors.push_back(p + ".match: expected object");
        }
    }
    // channels.<id>
    if (auto ch = GetPath(raw, "channels"); ch && ch->is_object())
        for (auto it = ch->begin(); it != ch->end(); ++it) {
            auto b = "channels." + it.key();
            if (it.key() == "defaults") continue;
            if (!it.value().is_object()) { r.errors.push_back(b + ": expected object"); continue; }
            expect(b + ".enabled", isbool, "boolean");
            expect(b + ".dmPolicy", oneof({"pairing", "allowlist", "open", "disabled"}), "pairing|allowlist|open|disabled");
            expect(b + ".groupPolicy", oneof({"open", "allowlist", "disabled"}), "open|allowlist|disabled");
            expect(b + ".allowFrom", isstrarr, "array of strings");
            expect(b + ".groupAllowFrom", isstrarr, "array of strings");
            for (auto k : {"botToken", "appToken", "token", "signingSecret", "appId", "appSecret"}) expect(b + "." + k, isstr, "string");
        }
    // plugins
    expect("plugins.enabled", isbool, "boolean");
    expect("plugins.allow", isstrarr, "array of strings");
    expect("plugins.deny", isstrarr, "array of strings");
    if (auto pe = GetPath(raw, "plugins.entries"); pe) {
        if (!pe->is_object()) r.errors.push_back("plugins.entries: expected object");
        else for (auto it = pe->begin(); it != pe->end(); ++it) {
            auto b = "plugins.entries." + it.key();
            expect(b + ".enabled", isbool, "boolean");
            expect(b + ".config", isobj, "object");
        }
    }
    // models.providers
    if (auto mp = GetPath(raw, "models.providers"); mp) {
        if (!mp->is_object()) r.errors.push_back("models.providers: expected object");
        else for (auto it = mp->begin(); it != mp->end(); ++it) {
            auto b = "models.providers." + it.key();
            expect(b + ".baseUrl", isstr, "string");
            expect(b + ".apiKey", isstr, "string");
            expect(b + ".api", oneof({"openai-completions", "openai-responses", "anthropic-messages", "google-generative-ai"}), "openai-completions|openai-responses|anthropic-messages|google-generative-ai");
            expect(b + ".models", [](const nlohmann::json& v) { return v.is_array(); }, "array");
        }
    }
    expect("cron.enabled", isbool, "boolean");
    expect("hooks.enabled", isbool, "boolean");
    expect("hooks.token", isstr, "string");
    (void)isint;
    return r;
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
        j = ResolveIncludes(j, path.parent_path(), &r.snap.include_spec, &r.snap.included);
        auto schema = ValidateSchema(j);
        r.warnings = schema.warnings;
        if (!schema.errors.empty()) {
            std::string m = "invalid config:";
            for (auto& e : schema.errors) m += " " + e + ";";
            throw std::runtime_error(m);
        }
        ExpandAgentEntries(j);
        r.snap.raw = j;  // keep unsubstituted for round-trip writes
        { auto sub = SubstituteEnv(j); ApplyTyped(r.snap, ResolveSecretRefs(sub, sub)); }
    } catch (const std::exception& e) {
        r.error = e.what();
        r.snap = Snapshot{};
    }
    return r;
}


// Upstream v2026.10.5 declares agents as agents.entries {<id>: {...}}. Internally every consumer reads
// agents.list [{id,...}], so loads expand entries into list items tagged "__fromEntries", and writes fold
// them back into entries (edits made through list-based code paths are preserved).
void ExpandAgentEntries(nlohmann::json& raw) {
    if (!raw.is_object() || !raw.contains("agents") || !raw["agents"].is_object()) return;
    auto& ag = raw["agents"];
    if (!ag.contains("entries") || !ag["entries"].is_object() || ag["entries"].empty()) return;
    if (!ag.contains("list") || !ag["list"].is_array()) ag["list"] = nlohmann::json::array();
    for (auto& [id, e] : ag["entries"].items()) {
        if (!e.is_object()) continue;
        bool dup = false;
        for (auto& x : ag["list"]) dup = dup || (x.is_object() && x.value("id", std::string()) == id);
        if (dup) continue;
        nlohmann::json item = e; item["id"] = id; item["__fromEntries"] = true;
        ag["list"].push_back(item);
    }
}
nlohmann::json FoldAgentEntries(nlohmann::json doc) {
    if (!doc.is_object() || !doc.contains("agents") || !doc["agents"].is_object() || !doc["agents"].contains("list") || !doc["agents"]["list"].is_array()) return doc;
    auto& ag = doc["agents"];
    nlohmann::json keep = nlohmann::json::array();
    bool any = false;
    std::set<std::string> live;
    for (auto& x : ag["list"]) {
        if (x.is_object() && x.value("__fromEntries", false)) {
            any = true;
            auto id = x.value("id", std::string()); nlohmann::json e = x; e.erase("id"); e.erase("__fromEntries");
            if (!ag.contains("entries") || !ag["entries"].is_object()) ag["entries"] = nlohmann::json::object();
            ag["entries"][id] = e; live.insert(id);
        } else keep.push_back(x);
    }
    if (any && ag.contains("entries")) {  // entries whose expanded item was removed by list-based code were deleted
        std::vector<std::string> gone;
        for (auto& [id, e] : ag["entries"].items()) if (!live.count(id)) { bool in_list = false; for (auto& x : keep) in_list = in_list || (x.is_object() && x.value("id", std::string()) == id); if (!in_list) gone.push_back(id); }
        for (auto& g : gone) ag["entries"].erase(g);
    }
    if (keep.empty()) ag.erase("list"); else ag["list"] = keep;
    return doc;
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
    { std::ofstream out(tmp); out << FoldAgentEntries(file_json).dump(2) << "\n"; }
    fs::rename(tmp, path);
}

void Save(const fs::path& path, const Snapshot& snap) { SaveRaw(path, ToFileDoc(snap, ToFileJson(snap))); }


nlohmann::json SavePreserving(const fs::path& path, const nlohmann::json& effective) {
    nlohmann::json out{{"ok", true}, {"path", path.string()}, {"preservedInclude", false}, {"preservedComments", false},
                       {"warnings", nlohmann::json::array()}};
    if (!effective.is_object()) return {{"ok", false}, {"error", "effective config must be an object"}};
    auto rep = ValidateSchema(effective);
    if (!rep.errors.empty()) return {{"ok", false}, {"error", "rejected: " + rep.errors.front()}, {"warnings", rep.warnings}};
    out["warnings"] = rep.warnings;
    std::error_code ec;
    std::string leading_comments;
    Snapshot snap;
    if (fs::exists(path, ec)) {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos) { leading_comments += line + "\n"; continue; }
            if (line.compare(first, 2, "//") == 0) { leading_comments += line + "\n"; continue; }
            break;
        }
        try { snap = Load(path).snap; } catch (...) {}
    }
    nlohmann::json file_doc = FoldAgentEntries(ToFileDoc(snap, effective));
    if (!snap.include_spec.is_null()) out["preservedInclude"] = true;

    auto indent_dump = [](const nlohmann::json& val) {
        std::string dumped = val.dump(2), out;
        for (std::size_t i = 0; i < dumped.size(); ++i) {
            out.push_back(dumped[i]);
            if (dumped[i] == '\n' && i + 1 < dumped.size()) out += "  ";
        }
        return out;
    };
    std::string body = leading_comments;
    if (!leading_comments.empty()) out["preservedComments"] = true;
    body += "{\n";
    bool first = true;
    auto emit = [&](const std::string& key, const nlohmann::json& val) {
        if (!first) body += ",\n";
        first = false;
        if (key == "$include") body += "  $include: " + val.dump();
        else body += "  \"" + key + "\": " + indent_dump(val);
    };
    if (file_doc.contains("$include")) emit("$include", file_doc["$include"]);
    for (auto it = file_doc.begin(); it != file_doc.end(); ++it)
        if (it.key() != "$include") emit(it.key(), it.value());
    body += "\n}\n";

    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    if (fs::exists(path, ec)) fs::copy_file(path, fs::path(path.string() + ".bak"), fs::copy_options::overwrite_existing, ec);
    auto tmp = fs::path(path.string() + ".tmp");
    { std::ofstream o(tmp); o << body; }
    fs::rename(tmp, path, ec);
    return out;
}


}  // namespace openclaw::config
