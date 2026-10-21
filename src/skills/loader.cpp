#include "openclaw/skills/loader.hpp"

#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace openclaw::skills {
namespace fs = std::filesystem;

namespace {

std::string ReadFile(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Minimal YAML-ish frontmatter: ---\nkey: value\n---
bool ParseFrontmatter(const std::string& text, std::string& name, std::string& description,
                      std::string& body, nlohmann::json* meta = nullptr, bool* dmi = nullptr) {
    if (text.rfind("---", 0) != 0) {
        body = text;
        return false;
    }
    auto end = text.find("\n---", 3);
    if (end == std::string::npos) {
        body = text;
        return false;
    }
    const std::string fm = text.substr(3, end - 3);
    body = text.substr(end + 4);
    while (!body.empty() && (body.front() == '\n' || body.front() == '\r')) {
        body.erase(body.begin());
    }
    std::istringstream iss(fm);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.erase(val.begin());
        }
        // strip quotes
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        if (key == "name") {
            name = val;
        } else if (key == "description") {
            description = val;
        } else if (key == "metadata" && meta) {
            try { *meta = nlohmann::json::parse(val); } catch (...) {}
        } else if (key == "disable-model-invocation" && dmi) {
            *dmi = (val == "true");
        } else if (key == "user-invocable" && meta) {
            (*meta)["__userInvocable"] = (val != "false");
        } else if ((key == "command-dispatch" || key == "command-tool" || key == "command-arg-mode") && meta) {
            (*meta)["__" + key] = val;
        }
    }
    // multiline description: " |" style — keep first line only for skeleton
    return !name.empty();
}

bool HasBin(const std::string& bin) {
    const char* p = std::getenv("PATH");
    if (!p || bin.empty()) return false;
    std::string path = p;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        fs::path cand = fs::path(path.substr(start, end - start)) / bin;
        std::error_code ec;
        if (!cand.empty() && fs::is_regular_file(cand, ec) &&
            (fs::status(cand, ec).permissions() & fs::perms::others_exec) != fs::perms::none)
            return true;
        start = end + 1;
    }
    return false;
}

std::string XmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

}  // namespace

void EvaluateGating(Skill& s) { EvaluateGating(s, nlohmann::json()); }

std::string SkillKey(const Skill& s) {
    if (s.metadata.contains("openclaw") && s.metadata["openclaw"].is_object()) {
        auto k = s.metadata["openclaw"].value("skillKey", std::string());
        if (!k.empty()) return k;
    }
    return s.name;
}

void EvaluateGating(Skill& s, const nlohmann::json& entry) {
    s.missing.clear();
    s.enabled = !(entry.is_object() && entry.value("enabled", true) == false);
    if (!s.enabled) { s.missing.push_back("disabled"); s.eligible = false; return; }
    std::string primary_env;
    if (s.metadata.contains("openclaw") && s.metadata["openclaw"].is_object())
        primary_env = s.metadata["openclaw"].value("primaryEnv", std::string());
    auto env_from_entry = [&](const std::string& var) {
        if (!entry.is_object()) return false;
        if (entry.contains("env") && entry["env"].is_object() && entry["env"].contains(var) &&
            entry["env"][var].is_string() && !entry["env"][var].get<std::string>().empty()) return true;
        return var == primary_env && entry.contains("apiKey") && entry["apiKey"].is_string() && !entry["apiKey"].get<std::string>().empty();
    };
    nlohmann::json oc = s.metadata.contains("openclaw") ? s.metadata["openclaw"] : nlohmann::json::object();
    if (!oc.is_object()) oc = nlohmann::json::object();
    if (oc.value("always", false)) { s.eligible = true; return; }
    auto req = oc.value("requires", nlohmann::json::object());
    if (req.is_object()) {
        for (const auto& b : req.value("bins", nlohmann::json::array()))
            if (b.is_string() && !HasBin(b.get<std::string>())) s.missing.push_back("bin:" + b.get<std::string>());
        auto any = req.value("anyBins", nlohmann::json::array());
        if (any.is_array() && !any.empty()) {
            bool found = false;
            for (const auto& b : any) if (b.is_string() && HasBin(b.get<std::string>())) found = true;
            if (!found) s.missing.push_back("anyBins:" + any.dump());
        }
        for (const auto& e : req.value("env", nlohmann::json::array())) {
            if (!e.is_string()) continue;
            const char* v = std::getenv(e.get<std::string>().c_str());
            if ((!v || !*v) && !env_from_entry(e.get<std::string>())) s.missing.push_back("env:" + e.get<std::string>());
        }
    }
    auto os = oc.value("os", nlohmann::json::array());
    if (os.is_array() && !os.empty()) {
#if defined(__APPLE__)
        const std::string me = "darwin";
#elif defined(_WIN32)
        const std::string me = "win32";
#else
        const std::string me = "linux";
#endif
        bool ok = false;
        for (const auto& o : os) if (o.is_string() && o.get<std::string>() == me) ok = true;
        if (!ok) s.missing.push_back("os:" + me);
    }
    s.eligible = s.missing.empty();
}

void Library::ApplyEntries(const nlohmann::json& entries) {
    for (auto& s : skills_) {
        auto key = SkillKey(s);
        EvaluateGating(s, entries.is_object() && entries.contains(key) ? entries[key] : nlohmann::json());
    }
}

std::vector<std::pair<std::string, std::string>> SkillEnvOverrides(const std::vector<Skill>& skills,
                                                                   const nlohmann::json& entries) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!entries.is_object()) return out;
    for (const auto& s : skills) {
        if (!s.eligible) continue;
        auto key = SkillKey(s);
        if (!entries.contains(key) || !entries[key].is_object()) continue;
        const auto& e = entries[key];
        auto add = [&](const std::string& k, const std::string& v) {
            const char* cur = std::getenv(k.c_str());
            if (cur && *cur) return;
            for (auto& p : out) if (p.first == k) return;
            out.push_back({k, v});
        };
        if (e.contains("env") && e["env"].is_object())
            for (auto it = e["env"].begin(); it != e["env"].end(); ++it) if (it.value().is_string()) add(it.key(), it.value());
        std::string primary;
        if (s.metadata.contains("openclaw") && s.metadata["openclaw"].is_object()) primary = s.metadata["openclaw"].value("primaryEnv", std::string());
        if (!primary.empty() && e.contains("apiKey") && e["apiKey"].is_string()) add(primary, e["apiKey"]);
    }
    return out;
}

std::string SlashName(const std::string& n) {
    std::string o;
    for (unsigned char c : n) {
        if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
        else if (!o.empty() && o.back() != '_') o += '_';
    }
    while (!o.empty() && o.back() == '_') o.pop_back();
    if (o.size() > 32) o.resize(32);
    return o;
}

std::optional<SkillCommand> MatchSkillCommand(const std::vector<Skill>& skills, const std::string& text) {
    std::size_t i = text.find_first_not_of(" \t\r\n");
    if (i == std::string::npos || text[i] != '/') return std::nullopt;
    std::size_t e = text.find_first_of(" \t\r\n", i);
    std::string cmd = text.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1);
    std::string rest = e == std::string::npos ? "" : text.substr(e);
    auto trim = [](std::string s) {
        auto a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    };
    rest = trim(rest);
    std::string want;
    if (cmd == "skill") {
        auto sp = rest.find_first_of(" \t\r\n");
        want = SlashName(rest.substr(0, sp));
        rest = sp == std::string::npos ? "" : trim(rest.substr(sp));
    } else {
        want = cmd;
    }
    for (const auto& s : skills) {
        if (!s.user_invocable || !s.eligible || SlashName(s.name) != want) continue;
        SkillCommand sc{s.name, rest, "", "", "/" + want};
        if (s.command_dispatch == "tool" && !s.command_tool.empty()) { sc.dispatch_tool = s.command_tool; return sc; }
        sc.prompt = "Use the \"" + s.name + "\" skill for this request.\n\n<skill name=\"" + s.name + "\" location=\"" +
                    (s.path / "SKILL.md").string() + "\">\n" + s.body + "\n</skill>\n\nUser input:\n" +
                    (rest.empty() ? std::string("(none)") : rest);
        return sc;
    }
    return std::nullopt;
}

nlohmann::json DispatchArgs(const SkillCommand& c) {
    return {{"command", c.args}, {"commandName", c.command_name}, {"skillName", c.skill}};
}

std::string FormatSkillsForPrompt(const std::vector<Skill>& skills) {
    std::ostringstream o;
    int n = 0;
    for (const auto& s : skills) {
        if (!s.eligible || s.disable_model_invocation) continue;
        if (n++ == 0) o << "<available_skills>\n";
        o << "  <skill>\n    <name>" << XmlEscape(s.name) << "</name>\n    <description>" << XmlEscape(s.description)
          << "</description>\n    <location>" << XmlEscape((s.path / "SKILL.md").string()) << "</location>\n  </skill>\n";
    }
    if (n) o << "</available_skills>";
    return o.str();
}

void Library::ScanRoots(const std::vector<ScanRoot>& roots) {
    skills_.clear();
    std::set<std::string> seen;
    for (const auto& r : roots) {
        if (r.path.empty() || !fs::is_directory(r.path)) continue;
        std::vector<fs::path> mds;
        if (fs::exists(r.path / "SKILL.md")) mds.push_back(r.path / "SKILL.md");
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(r.path)) if (e.is_directory()) dirs.push_back(e.path());
        std::sort(dirs.begin(), dirs.end());
        for (auto& d : dirs) if (fs::exists(d / "SKILL.md")) mds.push_back(d / "SKILL.md");
        for (auto& md : mds) {
            auto s = ParseSkillFile(md);
            if (!s || seen.count(s->name)) continue;
            s->source = r.source;
            seen.insert(s->name);
            skills_.push_back(*s);
        }
    }
}

std::optional<Skill> ParseSkillFile(const fs::path& skill_md) {
    if (!fs::exists(skill_md)) {
        return std::nullopt;
    }
    Skill s;
    s.path = skill_md.parent_path();
    const std::string text = ReadFile(skill_md);
    if (!ParseFrontmatter(text, s.name, s.description, s.body, &s.metadata, &s.disable_model_invocation)) {
        s.name = skill_md.parent_path().filename().string();
        s.body = text;
    }
    if (s.name.empty()) {
        s.name = skill_md.parent_path().filename().string();
    }
    if (!s.metadata.is_object()) s.metadata = nlohmann::json::object();
    if (s.metadata.contains("__userInvocable")) { s.user_invocable = s.metadata["__userInvocable"]; s.metadata.erase("__userInvocable"); }
    for (auto [k, dst] : {std::pair<const char*, std::string*>{"__command-dispatch", &s.command_dispatch},
                          {"__command-tool", &s.command_tool}, {"__command-arg-mode", &s.command_arg_mode}}) {
        if (s.metadata.contains(k)) { *dst = s.metadata[k].get<std::string>(); s.metadata.erase(k); }
    }
    EvaluateGating(s);
    return s;
}

void Library::Scan(const std::vector<fs::path>& roots) {
    skills_.clear();
    for (const auto& root : roots) {
        if (!fs::exists(root)) {
            continue;
        }
        // root/SKILL.md or root/*/SKILL.md
        if (fs::exists(root / "SKILL.md")) {
            if (auto s = ParseSkillFile(root / "SKILL.md")) {
                skills_.push_back(*s);
            }
        }
        if (!fs::is_directory(root)) {
            continue;
        }
        for (const auto& e : fs::directory_iterator(root)) {
            if (!e.is_directory()) {
                continue;
            }
            const auto md = e.path() / "SKILL.md";
            if (auto s = ParseSkillFile(md)) {
                skills_.push_back(*s);
            }
        }
    }
}

std::optional<Skill> Library::Find(const std::string& name) const {
    for (const auto& s : skills_) {
        if (s.name == name) {
            return s;
        }
    }
    return std::nullopt;
}

nlohmann::json Library::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : skills_) {
        arr.push_back({{"name", s.name},
                       {"description", s.description},
                       {"path", s.path.string()},
                       {"source", s.source},
                       {"eligible", s.eligible},
                       {"missing", s.missing},
                       {"disableModelInvocation", s.disable_model_invocation},
                       {"enabled", s.enabled},
                       {"userInvocable", s.user_invocable},
                       {"command", s.user_invocable ? "/" + SlashName(s.name) : ""},
                       {"commandDispatch", s.command_dispatch.empty() ? "model" : s.command_dispatch},
                       {"commandTool", s.command_tool}});
    }
    return arr;
}

nlohmann::json Library::StatusJson() const {
    return {{"count", skills_.size()}, {"skills", ListJson()}};
}

}  // namespace openclaw::skills
