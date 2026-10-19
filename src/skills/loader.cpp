#include "openclaw/skills/loader.hpp"

#include <fstream>
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
                      std::string& body) {
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
        }
    }
    // multiline description: " |" style — keep first line only for skeleton
    return !name.empty();
}

}  // namespace

std::optional<Skill> ParseSkillFile(const fs::path& skill_md) {
    if (!fs::exists(skill_md)) {
        return std::nullopt;
    }
    Skill s;
    s.path = skill_md.parent_path();
    const std::string text = ReadFile(skill_md);
    if (!ParseFrontmatter(text, s.name, s.description, s.body)) {
        s.name = skill_md.parent_path().filename().string();
        s.body = text;
    }
    if (s.name.empty()) {
        s.name = skill_md.parent_path().filename().string();
    }
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
                       {"path", s.path.string()}});
    }
    return arr;
}

nlohmann::json Library::StatusJson() const {
    return {{"count", skills_.size()}, {"skills", ListJson()}};
}

}  // namespace openclaw::skills
