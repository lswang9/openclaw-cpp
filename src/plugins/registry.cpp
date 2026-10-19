#include "openclaw/plugins/registry.hpp"

#include <fstream>

namespace openclaw::plugins {
namespace fs = std::filesystem;

void Registry::Scan(const fs::path& root) {
    plugins_.clear();
    if (!fs::exists(root) || !fs::is_directory(root)) {
        return;
    }
    for (const auto& e : fs::directory_iterator(root)) {
        if (!e.is_directory()) {
            continue;
        }
        Plugin p;
        p.id = e.path().filename().string();
        p.name = p.id;
        p.path = e.path();
        p.enabled = false;
        const auto manifest = e.path() / "plugin.json";
        if (fs::exists(manifest)) {
            try {
                std::ifstream in(manifest);
                nlohmann::json j;
                in >> j;
                p.name = j.value("name", p.name);
                p.enabled = j.value("enabled", false);
            } catch (...) {
            }
        }
        plugins_.push_back(std::move(p));
    }
}

nlohmann::json Registry::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : plugins_) {
        arr.push_back({{"id", p.id},
                       {"name", p.name},
                       {"enabled", p.enabled},
                       {"path", p.path.string()}});
    }
    return arr;
}

}  // namespace openclaw::plugins
