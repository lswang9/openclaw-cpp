#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openclaw::secrets {

// File-backed secret KV (values never logged by gateway status).
class Store {
public:
    explicit Store(std::filesystem::path path);
    void Set(const std::string& key, const std::string& value);
    std::optional<std::string> Get(const std::string& key) const;
    bool Delete(const std::string& key);
    std::vector<std::string> ListKeys() const;  // keys only
    nlohmann::json ListJson() const;            // keys only, no values

private:
    std::filesystem::path path_;
    nlohmann::json data_ = nlohmann::json::object();
    void Load();
    void Save() const;
};

}  // namespace openclaw::secrets
