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
    std::string source;          // workspace | managed | bundled | extra
    nlohmann::json metadata = nlohmann::json::object();  // frontmatter `metadata` (JSON), e.g. {"openclaw":{"requires":{...}}}
    bool disable_model_invocation = false;               // frontmatter disable-model-invocation
    bool user_invocable = true;                          // frontmatter user-invocable (slash command)
    bool enabled = true;                                 // skills.entries.<key>.enabled
    bool eligible = true;                                // gating result
    std::vector<std::string> missing;                    // unmet requirements ("bin:x", "env:Y", "os:z")
};

struct ScanRoot {
    std::filesystem::path path;
    std::string source;
};

// OpenClaw gating: metadata.openclaw.requires.{bins,anyBins,env} and metadata.openclaw.os.
void EvaluateGating(Skill& s);
// Same, but env requirements are also satisfied by skills.entries.<key>.env / apiKey (primaryEnv).
void EvaluateGating(Skill& s, const nlohmann::json& entry);
// Config key for a skill: metadata.openclaw.skillKey or name.
std::string SkillKey(const Skill& s);

// Env vars to inject for an agent run from skills.entries (eligible+enabled skills only; never overrides
// variables already set in the process).
std::vector<std::pair<std::string, std::string>> SkillEnvOverrides(const std::vector<Skill>& skills,
                                                                   const nlohmann::json& entries);

// RAII: set vars for the duration of a run, restoring previous state. Process-global (not per-thread).
class ScopedEnv {
public:
    explicit ScopedEnv(const std::vector<std::pair<std::string, std::string>>& vars);
    ~ScopedEnv();
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
private:
    std::vector<std::string> set_;
};

// Slash command name for a skill: lowercase, [a-z0-9_], max 32 chars (upstream sanitisation).
std::string SlashName(const std::string& skill_name);
struct SkillCommand { std::string skill; std::string args; std::string prompt; };
// "/skill <name> [args]" or "/<slashname> [args]" for user-invocable, eligible skills.
std::optional<SkillCommand> MatchSkillCommand(const std::vector<Skill>& skills, const std::string& text);

// Upstream-style XML block listing eligible, model-invocable skills (name/description/location).
std::string FormatSkillsForPrompt(const std::vector<Skill>& skills);

class Library {
public:
    // Scan roots for */SKILL.md (OpenClaw skill layout).
    void Scan(const std::vector<std::filesystem::path>& roots);
    // Precedence scan: earlier roots win on name collision (workspace > managed > bundled).
    void ScanRoots(const std::vector<ScanRoot>& roots);
    // Apply skills.entries config (enabled / env / apiKey) and re-gate.
    void ApplyEntries(const nlohmann::json& entries);
    std::vector<Skill> List() const { return skills_; }
    std::optional<Skill> Find(const std::string& name) const;
    nlohmann::json ListJson() const;
    nlohmann::json StatusJson() const;

private:
    std::vector<Skill> skills_;
};

std::optional<Skill> ParseSkillFile(const std::filesystem::path& skill_md);

}  // namespace openclaw::skills
