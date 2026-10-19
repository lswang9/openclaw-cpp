#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::skills {

struct Skill {
    std::string name;
    std::string description;
    std::filesystem::path path;  // directory containing SKILL.md
    std::string body;            // markdown body after frontmatter
};

class Library {
public:
    // Scan roots for */SKILL.md (OpenClaw skill layout).
    void Scan(const std::vector<std::filesystem::path>& roots);
    std::vector<Skill> List() const { return skills_; }
    std::optional<Skill> Find(const std::string& name) const;
    nlohmann::json ListJson() const;
    nlohmann::json StatusJson() const;

private:
    std::vector<Skill> skills_;
};

std::optional<Skill> ParseSkillFile(const std::filesystem::path& skill_md);

}  // namespace openclaw::skills
