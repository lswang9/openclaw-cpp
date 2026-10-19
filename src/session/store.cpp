#include "openclaw/session/store.hpp"
#include <cctype>
#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace openclaw::session {
namespace fs = std::filesystem;

Store::Store(fs::path root) : root_(std::move(root)) {
    fs::create_directories(root_);
}

std::shared_ptr<std::recursive_mutex> Store::MutexFor(const std::string& id) const {
    std::lock_guard<std::recursive_mutex> g(map_mu_);
    auto it = locks_.find(id);
    if (it != locks_.end()) return it->second;
    auto m = std::make_shared<std::recursive_mutex>();
    locks_[id] = m;
    return m;
}

Store::Guard Store::Lock(const std::string& id) const { return Guard(MutexFor(id)); }

fs::path Store::PathFor(const std::string& id) const {
    std::string safe;
    for (char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            safe.push_back(c);
        }
    }
    if (safe.empty()) safe = "default";
    return root_ / (safe + ".json");
}

std::vector<std::string> Store::List() const {
    std::vector<std::string> ids;
    if (!fs::exists(root_)) return ids;
    for (const auto& e : fs::directory_iterator(root_)) {
        if (e.path().extension() == ".json") ids.push_back(e.path().stem().string());
    }
    return ids;
}

bool Store::Exists(const std::string& id) const {
    auto g = Lock(id);
    return fs::exists(PathFor(id));
}

std::vector<agent::Message> Store::Load(const std::string& id) const {
    auto g = Lock(id);
    const auto path = PathFor(id);
    if (!fs::exists(path)) return {};
    std::ifstream in(path);
    nlohmann::json j;
    in >> j;
    std::vector<agent::Message> msgs;
    if (j.contains("messages") && j["messages"].is_array()) {
        for (const auto& m : j["messages"]) msgs.push_back(agent::Message::FromJson(m));
    }
    return msgs;
}

void Store::Save(const std::string& id, const std::vector<agent::Message>& messages) const {
    auto g = Lock(id);
    nlohmann::json j;
    j["id"] = id;
    j["messages"] = nlohmann::json::array();
    for (const auto& m : messages) j["messages"].push_back(m.ToJson());
    std::ofstream out(PathFor(id));
    out << j.dump(2);
}

void Store::Append(const std::string& id, const agent::Message& message) const {
    // Lock once via Save path — but Load also locks; avoid double-lock deadlock by inlining
    auto g = Lock(id);
    const auto path = PathFor(id);
    std::vector<agent::Message> msgs;
    if (fs::exists(path)) {
        std::ifstream in(path);
        nlohmann::json j;
        in >> j;
        if (j.contains("messages") && j["messages"].is_array()) {
            for (const auto& m : j["messages"]) msgs.push_back(agent::Message::FromJson(m));
        }
    }
    msgs.push_back(message);
    nlohmann::json j;
    j["id"] = id;
    j["messages"] = nlohmann::json::array();
    for (const auto& m : msgs) j["messages"].push_back(m.ToJson());
    std::ofstream out(path);
    out << j.dump(2);
}

bool Store::Remove(const std::string& id) const {
    auto g = Lock(id);
    return fs::remove(PathFor(id));
}

}  // namespace openclaw::session
