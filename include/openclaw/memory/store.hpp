#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::memory {

// Simple key/value memory notes — toward OpenClaw memory / context-engine.
class Store {
public:
    explicit Store(std::filesystem::path root);
    void Set(const std::string& key, const std::string& value);
    std::optional<std::string> Get(const std::string& key) const;
    bool Remove(const std::string& key);
    std::vector<std::string> Keys() const;
    nlohmann::json Dump() const;

private:
    std::filesystem::path path_;
    nlohmann::json data_ = nlohmann::json::object();
    void Load();
    void Save() const;
};

}  // namespace openclaw::memory
