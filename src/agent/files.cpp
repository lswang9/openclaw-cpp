#include "openclaw/agent/files.hpp"
#include <cctype>

#include <fstream>

namespace openclaw::agent {
namespace fs = std::filesystem;

FileStore::FileStore(fs::path root) : root_(std::move(root)) {
    fs::create_directories(root_);
}

fs::path FileStore::DirFor(const std::string& agent_id) const {
    std::string safe;
    for (char c : agent_id) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') safe.push_back(c);
    }
    if (safe.empty()) safe = "main";
    auto d = root_ / safe;
    fs::create_directories(d);
    return d;
}

std::vector<std::string> FileStore::List(const std::string& agent_id) const {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(DirFor(agent_id))) {
        if (e.is_regular_file()) names.push_back(e.path().filename().string());
    }
    return names;
}

std::optional<std::string> FileStore::Get(const std::string& agent_id, const std::string& name) const {
    auto path = DirFor(agent_id) / name;
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream in(path);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s;
}

void FileStore::Set(const std::string& agent_id, const std::string& name, const std::string& content) {
    std::ofstream out(DirFor(agent_id) / name);
    out << content;
}

bool FileStore::Remove(const std::string& agent_id, const std::string& name) {
    auto path = DirFor(agent_id) / name;
    if (!fs::exists(path)) return false;
    return fs::remove(path);
}

nlohmann::json FileStore::ListJson(const std::string& agent_id) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : List(agent_id)) arr.push_back({{"name", n}});
    return arr;
}

}  // namespace openclaw::agent
