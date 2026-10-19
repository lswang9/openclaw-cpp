#pragma once

#include "openclaw/agent/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::session {

// File-backed session store (JSON). Aligns with OpenClaw session persistence
// concept; format is simplified for phase expansion toward 2026.10.5.
class Store {
public:
    explicit Store(std::filesystem::path root);

    std::filesystem::path Root() const { return root_; }
    std::vector<std::string> List() const;
    bool Exists(const std::string& id) const;
    std::vector<agent::Message> Load(const std::string& id) const;
    void Save(const std::string& id, const std::vector<agent::Message>& messages) const;
    void Append(const std::string& id, const agent::Message& message) const;
    bool Remove(const std::string& id) const;

private:
    std::filesystem::path PathFor(const std::string& id) const;
    std::filesystem::path root_;
};

}  // namespace openclaw::session
