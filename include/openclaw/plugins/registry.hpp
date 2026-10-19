#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace openclaw::plugins {

struct Plugin {
    std::string id;
    std::string name;
    bool enabled = false;
    std::filesystem::path path;
};

class Registry {
public:
    void Scan(const std::filesystem::path& root);
    std::vector<Plugin> List() const { return plugins_; }
    nlohmann::json ListJson() const;

private:
    std::vector<Plugin> plugins_;
};

}  // namespace openclaw::plugins
