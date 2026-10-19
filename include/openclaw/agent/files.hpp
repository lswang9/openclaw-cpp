#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::agent {

// Per-agent workspace files (agents.files.* RPCs).
class FileStore {
public:
    explicit FileStore(std::filesystem::path root);

    std::vector<std::string> List(const std::string& agent_id) const;
    std::optional<std::string> Get(const std::string& agent_id, const std::string& name) const;
    void Set(const std::string& agent_id, const std::string& name, const std::string& content);
    bool Remove(const std::string& agent_id, const std::string& name);
    nlohmann::json ListJson(const std::string& agent_id) const;

private:
    std::filesystem::path DirFor(const std::string& agent_id) const;
    std::filesystem::path root_;
};

}  // namespace openclaw::agent
